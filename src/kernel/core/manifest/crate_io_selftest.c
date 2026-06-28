/*
 * crate_io_selftest — ironclad in-kernel proof of the crate-boundary straddle
 * fix.
 *
 * Builds two virtually-adjacent, physically NON-adjacent user pages (frame A
 * and frame B, separated by an unmapped physical "spacer" frame) plus a poison
 * guard page, then for a range of straddle offsets k drives both the LEGACY
 * single-frame path and the page-walked crate_io path over a Crate that spans
 * the A|B seam:
 *
 *   WRITE CONTROL  raw vmm_translate_user_addr + memcpy — the overflow lands
 *                  in the SPACER (the physically-next frame) and frame B is
 *                  left untouched. Proves the legacy StorageCrateMap path
 *                  corrupts a foreign frame on a non-contiguous straddle.
 *   WRITE FIX      crate_write — both frame A's tail and frame B's head carry
 *                  the bytes, the spacer is UNtouched, the guard is intact.
 *   READ  CONTROL  raw vmm_translate_user_addr + memcpy — the tail bytes come
 *                  from the SPACER, not frame B. Proves the legacy read path
 *                  snapshots a foreign frame.
 *   READ  FIX      crate_read — the snapshot equals the planted bytes across
 *                  the seam, sourced from the two real frames.
 *
 * The CONTROL assertions FAIL on the unfixed primitive: they are exactly the
 * demonstration that the old path corrupts/reads a foreign frame. The whole
 * test is deterministic — the contiguous 3-frame block guarantees frame B is
 * two physical frames above frame A, so the frame after A is always the
 * spacer.
 */

#include "crate_io_selftest.h"
#include "crate_io.h"
#include "pmm.h"
#include "boxos_crate.h"
#include "op_registry.h"   /* OpContext */
#include "klib.h"

#define CRATE_IO_SELFTEST_VA   0x0000000040000000ULL  /* 1 GiB: 3 free user pages */
#define CRATE_IO_OVERFLOW      64u   /* bytes that overrun the A|B seam into B   */
#define CRATE_IO_POISON        0x5Au /* even; the source pattern is always odd   */

/* Straddle offsets: k bytes precede the seam in frame A, OVERFLOW bytes follow
 * it in frame B. Iterating k moves the seam across the payload. */
static const unsigned CRATE_IO_K[] = {
    1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u
};

/* Source pattern + read-back scratch. Static (BSS), not stack — each is a full
 * page, and a single run never exceeds CRATE_IO_OVERFLOW + max(k) = 2112 bytes.
 * Bytes are always odd so they never collide with the even POISON value. */
static uint8_t g_src[VMM_PAGE_SIZE];
static uint8_t g_dst[VMM_PAGE_SIZE];

static void fill_src(void)
{
    for (size_t i = 0; i < sizeof(g_src); i++)
        g_src[i] = (uint8_t)(i * 2u + 1u);
}

static bool all_bytes(const uint8_t *p, unsigned len, uint8_t val)
{
    for (unsigned i = 0; i < len; i++)
        if (p[i] != val) return false;
    return true;
}

bool NonContigPagesSetup(NonContigPages *m, uintptr_t base_va)
{
    memset(m, 0, sizeof(*m));
    m->base_va = base_va;

    m->vmm = vmm_create_context();
    if (!m->vmm) return false;

    m->cabin = kmalloc(sizeof(cabin_t));
    m->proc  = kmalloc(sizeof(process_t));
    if (!m->cabin || !m->proc) { NonContigPagesTeardown(m); return false; }
    memset(m->cabin, 0, sizeof(cabin_t));
    memset(m->proc,  0, sizeof(process_t));
    m->cabin->vmm  = m->vmm;
    m->proc->cabin = m->cabin;

    /* One contiguous 3-frame block: frame A | spacer | frame B. */
    m->block       = pmm_alloc(3);
    m->guard_frame = pmm_alloc(1);
    if (!m->block || !m->guard_frame) { NonContigPagesTeardown(m); return false; }

    /* pmm_alloc returns PHYSICAL addresses; reach the frames through the Pull
     * Map (the boot identity map is gone after vmm_init, so a low physical
     * value is not a usable pointer). The physical addresses come straight
     * from the allocated block bases. */
    m->page_a = (uint8_t *)vmm_phys_to_virt((uintptr_t)m->block);
    m->spacer = (uint8_t *)vmm_phys_to_virt((uintptr_t)m->block + VMM_PAGE_SIZE);
    m->page_b = (uint8_t *)vmm_phys_to_virt((uintptr_t)m->block + 2u * VMM_PAGE_SIZE);
    m->guard  = (uint8_t *)vmm_phys_to_virt((uintptr_t)m->guard_frame);

    m->phys_a = (uintptr_t)m->block;
    m->phys_b = (uintptr_t)m->block + 2u * VMM_PAGE_SIZE;
    uintptr_t phys_spacer = (uintptr_t)m->block + VMM_PAGE_SIZE;

    /* Layout sanity (catches an arithmetic slip): the frame after A is the
     * spacer, B is two frames above A. The contiguous 3-frame block is
     * guaranteed by the buddy allocator; the real non-adjacency PROOF is
     * runtime — the WRITE CONTROL below corrupts the spacer and leaves B
     * intact, which can only happen if the frame after A is not B. */
    if (phys_spacer != m->phys_a + VMM_PAGE_SIZE ||
        m->phys_b   != m->phys_a + 2u * VMM_PAGE_SIZE) {
        NonContigPagesTeardown(m);
        return false;
    }

    /* Map A and B at adjacent VAs (spacer deliberately unmapped) and the guard
     * one page above B. USER-RW so the page-walk and vmm_translate_user_addr
     * accept them. */
    vmm_map_result_t ra = vmm_map_page(m->vmm, base_va,
                                       m->phys_a, VMM_FLAGS_USER_RW);
    vmm_map_result_t rb = vmm_map_page(m->vmm, base_va + VMM_PAGE_SIZE,
                                       m->phys_b, VMM_FLAGS_USER_RW);
    vmm_map_result_t rg = vmm_map_page(m->vmm, base_va + 2u * VMM_PAGE_SIZE,
                                       (uintptr_t)m->guard_frame,
                                       VMM_FLAGS_USER_RW);
    if (!ra.success || !rb.success || !rg.success) {
        NonContigPagesTeardown(m);
        return false;
    }

    /* Re-resolve through the page tables: prove the mapping points where we
     * think — the exact translation the crate path will follow. */
    if (vmm_virt_to_phys(m->vmm, base_va) != m->phys_a ||
        vmm_virt_to_phys(m->vmm, base_va + VMM_PAGE_SIZE) != m->phys_b) {
        NonContigPagesTeardown(m);
        return false;
    }
    return true;
}

void NonContigPagesTeardown(NonContigPages *m)
{
    if (!m) return;

    if (m->vmm) {
        vmm_unmap_page(m->vmm, m->base_va);
        vmm_unmap_page(m->vmm, m->base_va + VMM_PAGE_SIZE);
        vmm_unmap_page(m->vmm, m->base_va + 2u * VMM_PAGE_SIZE);
    }
    /* Frames are owned here, not by the context — unmap cleared the PTEs, so
     * vmm_destroy_context frees only the page-table structures, never these. */
    if (m->block)       pmm_free(m->block, 3);
    if (m->guard_frame) pmm_free(m->guard_frame, 1);
    if (m->vmm)         vmm_destroy_context(m->vmm);
    if (m->proc)        kfree(m->proc);
    if (m->cabin)       kfree(m->cabin);

    memset(m, 0, sizeof(*m));
}

#define FAILK(reason, k) do {                                                  \
        kprintf("[CRATE-IO] SELFTEST FAIL: " reason " (k=%u)\n", (unsigned)(k)); \
        return false;                                                          \
    } while (0)

static bool run_k(NonContigPages *m, unsigned k)
{
    const unsigned  over = CRATE_IO_OVERFLOW;
    const unsigned  n    = k + over;                       /* straddling length  */
    const uintptr_t va   = m->base_va + VMM_PAGE_SIZE - k; /* k in A, over in B  */

    OpContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.proc = m->proc;

    Crate crate;
    memset(&crate, 0, sizeof(crate));
    crate.magic    = CRATE_MAGIC;
    crate.kind     = CRATE_KIND_INOUT;
    crate.addr     = (uint64_t)va;
    crate.capacity = n;

    /* ---- WRITE CONTROL — legacy path corrupts the foreign spacer frame. -- */
    memset(m->page_a, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->spacer, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->page_b, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->guard,  CRATE_IO_POISON, VMM_PAGE_SIZE);

    void *legacy = vmm_translate_user_addr(m->vmm, va, n);  /* truncates, non-NULL */
    if (!legacy) FAILK("write control: translate returned NULL", k);
    memcpy(legacy, g_src, n);                               /* overruns A -> spacer */

    if (memcmp(m->page_a + (VMM_PAGE_SIZE - k), g_src, k) != 0)
        FAILK("write control: frame A tail wrong", k);
    if (memcmp(m->spacer, g_src + k, over) != 0)
        FAILK("write control: spacer not corrupted (bug not reproduced)", k);
    if (!all_bytes(m->page_b, over, CRATE_IO_POISON))
        FAILK("write control: frame B was written (non-contiguity broke)", k);

    /* ---- WRITE FIX — crate_write page-walks to BOTH real frames. --------- */
    memset(m->page_a, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->spacer, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->page_b, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->guard,  CRATE_IO_POISON, VMM_PAGE_SIZE);

    crate.size = 0;
    if (crate_write(&crate, &ctx, g_src, n) != OK)
        FAILK("write fix: crate_write returned error", k);
    if (crate.size != n)
        FAILK("write fix: crate.size != n", k);
    if (memcmp(m->page_a + (VMM_PAGE_SIZE - k), g_src, k) != 0)
        FAILK("write fix: frame A tail wrong", k);
    if (memcmp(m->page_b, g_src + k, over) != 0)
        FAILK("write fix: frame B head not delivered", k);
    if (!all_bytes(m->spacer, VMM_PAGE_SIZE, CRATE_IO_POISON))
        FAILK("write fix: spacer was corrupted", k);
    if (!all_bytes(m->guard, VMM_PAGE_SIZE, CRATE_IO_POISON))
        FAILK("write fix: guard page overrun", k);

    /* ---- READ CONTROL — legacy path snapshots the foreign spacer frame. -- */
    memset(m->page_a, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->spacer, CRATE_IO_POISON, VMM_PAGE_SIZE);
    memset(m->page_b, CRATE_IO_POISON, VMM_PAGE_SIZE);
    /* Plant the user's intended source across the seam: k bytes in A's tail,
     * over bytes in B's head. The spacer keeps POISON, distinct from g_src. */
    memcpy(m->page_a + (VMM_PAGE_SIZE - k), g_src, k);
    memcpy(m->page_b, g_src + k, over);

    void *legacy_r = vmm_translate_user_addr(m->vmm, va, n);
    if (!legacy_r) FAILK("read control: translate returned NULL", k);
    memset(g_dst, 0, n);
    memcpy(g_dst, legacy_r, n);                             /* reads A tail then spacer */
    if (memcmp(g_dst, g_src, k) != 0)
        FAILK("read control: frame A tail mis-read", k);
    if (!all_bytes(g_dst + k, over, CRATE_IO_POISON))
        FAILK("read control: did not read the spacer (bug not reproduced)", k);

    /* ---- READ FIX — crate_read page-walks to BOTH real frames. ----------- */
    crate.size = n;
    memset(g_dst, 0, n);
    if (crate_read(&crate, &ctx, g_dst, n) != OK)
        FAILK("read fix: crate_read returned error", k);
    if (memcmp(g_dst, g_src, n) != 0)
        FAILK("read fix: snapshot mismatch across seam", k);

    return true;
}

#undef FAILK

error_t CrateIoSelfTest(void)
{
    debug_printf("[CRATE-IO][selftest] starting\n");
    fill_src();

    NonContigPages map;
    if (!NonContigPagesSetup(&map, CRATE_IO_SELFTEST_VA)) {
        kprintf("[CRATE-IO] SELFTEST FAIL: non-contiguous mapping setup\n");
        return ERR_INTERNAL;
    }

    bool ok = true;
    for (size_t i = 0; i < sizeof(CRATE_IO_K) / sizeof(CRATE_IO_K[0]); i++) {
        if (!run_k(&map, CRATE_IO_K[i])) { ok = false; break; }
    }

    NonContigPagesTeardown(&map);

    if (!ok) return ERR_INTERNAL;   /* run_k already printed the FAIL line */

    kprintf("[CRATE-IO] SELFTEST PASS\n");
    return OK;
}
