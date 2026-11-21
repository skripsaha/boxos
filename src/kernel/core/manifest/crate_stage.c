/*
 * CrateStage — implementation.
 *
 * See crate_stage.h for the architectural rationale and the
 * handler/dispatcher ownership protocol.
 */

#include "crate_stage.h"
#include "ktypes.h"
#include "klib.h"
#include "vmm.h"
#include "amp.h"

/*
 * The shelf. Nearly every call names a few crates, and their array lived a
 * kmalloc and a kfree long — a size class's lock taken twice, interrupts off,
 * on a line every core takes, and the heap's charge reckoned at each. So an
 * array of up to CRATE_SHELF_CRATES crates is a block of one region laid out
 * once at boot, every core keeping some on a shelf of its own, taken and put
 * back with its interrupts off and no lock. Ownership is as it was: the block
 * goes with the crates to whoever answers and comes back through a release
 * here — on another core it joins that core's shelf. A shelf full when a block
 * comes spills it to one shared stack, under a lock; an empty one draws from
 * it; and a call naming more crates than a block holds, or finding both empty,
 * has the heap as before.
 */
#define CRATE_SHELF_CRATES     8u
#define CRATE_SHELF_DEPTH      8u
#define CRATE_SHELF_BLOCK      (CRATE_SHELF_CRATES * sizeof(Crate))

typedef struct {
    Crate   *block[2u * CRATE_SHELF_DEPTH];
    uint32_t count;
} __attribute__((aligned(64))) CrateShelf;

static CrateShelf *g_shelves;          /* one a core */
static uint8_t    *g_shelf_region;     /* every block, laid out once */
static size_t      g_shelf_region_bytes;
static Crate     **g_shelf_spill;      /* blocks no shelf had room for */
static uint32_t    g_shelf_spill_count;
static spinlock_t  g_shelf_spill_lock;

error_t CrateShelfInit(void)
{
    const uint32_t cores  = g_amp.total_cores ? g_amp.total_cores : 1u;
    const uint32_t blocks = cores * CRATE_SHELF_DEPTH;
    CrateShelf *shelves = kmalloc(cores * sizeof(CrateShelf));
    uint8_t    *region  = kmalloc((size_t)blocks * CRATE_SHELF_BLOCK);
    Crate     **spill   = kmalloc(blocks * sizeof(Crate *));
    if (!shelves || !region || !spill) {
        if (shelves) kfree(shelves);
        if (region)  kfree(region);
        if (spill)   kfree(spill);
        return ERR_NO_MEMORY;
    }
    memset(shelves, 0, cores * sizeof(CrateShelf));
    for (uint32_t b = 0; b < blocks; b++) {
        CrateShelf *shelf = &shelves[b / CRATE_SHELF_DEPTH];
        shelf->block[shelf->count++] = (Crate *)(region + (size_t)b * CRATE_SHELF_BLOCK);
    }
    spinlock_init(&g_shelf_spill_lock);
    g_shelf_spill        = spill;
    g_shelf_region_bytes = (size_t)blocks * CRATE_SHELF_BLOCK;
    g_shelf_region       = region;
    __atomic_store_n(&g_shelves, shelves, __ATOMIC_RELEASE);
    return OK;
}

static Crate *crate_shelf_take(uint16_t count)
{
    CrateShelf *shelves = __atomic_load_n(&g_shelves, __ATOMIC_ACQUIRE);
    if (!shelves || count > CRATE_SHELF_CRATES)
        return NULL;
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    CrateShelf *shelf = &shelves[amp_get_core_index()];
    Crate *block = shelf->count ? shelf->block[--shelf->count] : NULL;
    __asm__ volatile("push %0; popfq" :: "r"(rflags) : "memory", "cc");
    if (block)
        return block;
    spin_lock(&g_shelf_spill_lock);
    if (g_shelf_spill_count)
        block = g_shelf_spill[--g_shelf_spill_count];
    spin_unlock(&g_shelf_spill_lock);
    return block;
}

/* False for a block that is not the shelf's: it came from the heap. */
static bool crate_shelf_put(Crate *block)
{
    const uint8_t *at = (const uint8_t *)block;
    if (!g_shelf_region || at < g_shelf_region || at >= g_shelf_region + g_shelf_region_bytes)
        return false;
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    CrateShelf *shelf = &g_shelves[amp_get_core_index()];
    const bool kept = shelf->count < 2u * CRATE_SHELF_DEPTH;
    if (kept)
        shelf->block[shelf->count++] = block;
    __asm__ volatile("push %0; popfq" :: "r"(rflags) : "memory", "cc");
    if (!kept) {
        spin_lock(&g_shelf_spill_lock);
        g_shelf_spill[g_shelf_spill_count++] = block;   /* room for every block */
        spin_unlock(&g_shelf_spill_lock);
    }
    return true;
}

Crate *crate_stage_alloc(uint16_t count)
{
    if (count == 0) return NULL;
    Crate *block = crate_shelf_take(count);
    return block ? block : (Crate *)kmalloc((size_t)count * sizeof(Crate));
}

static void crate_stage_free(Crate *crates)
{
    if (!crate_shelf_put(crates))
        kfree(crates);
}

error_t crate_stage_in(vmm_context_t        *cabin,
                        uint64_t              user_uaddr,
                        uint16_t              count,
                        Crate               **out_crates)
{
    if (!cabin || !out_crates) return ERR_NULL_POINTER;
    *out_crates = NULL;
    if (count == 0) return OK;

    size_t bytes = (size_t)count * sizeof(Crate);
    Crate *kbuf = crate_stage_alloc(count);
    if (!kbuf) return ERR_NO_MEMORY;

    error_t rc = vmm_user_buf_in_into(cabin, (uintptr_t)user_uaddr, bytes, kbuf);
    if (rc != OK) {
        crate_stage_free(kbuf);
        return rc;
    }
    *out_crates = kbuf;
    return OK;
}

error_t crate_stage_commit_and_release(Crate                *crates,
                                        uint16_t              count,
                                        vmm_context_t        *cabin,
                                        uint64_t              user_uaddr)
{
    if (!crates) return OK;
    error_t rc = OK;
    /*
     * user_uaddr == 0 is the sentinel for "kernel-internal staging" — used
     * by touch_react_deliver where the Crate descriptor is synthesized
     * inside the kernel and the subscriber's manifest never asked for a
     * user-side write-back. In that mode we skip commit_out entirely and
     * just kfree. The Pocket-dispatch path in guide.c always passes a
     * real user vaddr, so this sentinel only matches the intended caller.
     */
    if (count > 0 && cabin && user_uaddr != 0) {
        size_t bytes = (size_t)count * sizeof(Crate);
        rc = vmm_user_buf_commit_out(cabin, (uintptr_t)user_uaddr, crates, bytes);
        if (rc != OK) {
            debug_printf("[CrateStage] commit_out failed: rc=%d uaddr=0x%lx bytes=%lu\n",
                         rc, (unsigned long)user_uaddr, (unsigned long)bytes);
        }
    }
    /* Always released, even on commit failure — the buffer must not leak. */
    crate_stage_free(crates);
    return rc;
}

void crate_stage_release_no_commit(Crate *crates)
{
    if (crates) crate_stage_free(crates);
}
