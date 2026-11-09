/* klib_mem.c — the kernel heap's front: kmalloc, krealloc and kfree.
 *
 * Four roads, and each allocation takes one:
 *   1. The reserve, for the kernel's own paths (kheap.h), first.
 *   2. The slab — O(1) for allocations <= SLAB_LARGE_THRESHOLD (2048 bytes),
 *      one set of size classes per charge.
 *   3. A large piece — KHEAP_LARGE_MIN_PAGES pages and more: an exact run of
 *      pages of its own (kheap_region.c).
 *   4. The pool — everything between, first fit in regions the heap takes
 *      from the page allocator as it is asked (kheap_region.c); also where
 *      the slab falls through when the machine has no page for it.
 * Before every road but the reserve, the share rule (kheap_charge.c): a class
 * of cabins that would hold more than the machine leaves free is refused.
 *
 * Every pool block carries KLIB_MAGIC_NUMBER while it lives and
 * KLIB_MAGIC_FREE once freed, so a double free is found in O(1) by its
 * tombstone. Every pool block and large piece is wrapped in canary words; the
 * slab validates its own through slab_owns().
 *
 * mem_init also bootstraps the console-print lock by calling
 * klib_print_lock_init() — it lives in klib_print.c but must be wired in
 * one place so the spinlock_t invariants are initialised before the first
 * kprintf-class call. */
#include "klib.h"
#include "kheap.h"
#include "kheap_internal.h"
#include "slab.h"
#include "pmm.h"
#include "vmm.h"
#ifdef CONFIG_VOLUME_TESTS
#include "amp.h"    /* HEAPFAIL: which core is asking */
#include "idt.h"    /* HEAPFAIL: whether it is asking from an interrupt handler */
#endif

typedef struct
{
    uint64_t canary_start;
    size_t size;
    const char *caller;
    size_t canary_end_offset;
} heap_guard_t;

/* Declared in klib_print.c — initialises g_kprintf_lock once. */
void klib_print_lock_init(void);

/* The boot region is all the heap there is until the Pull Map reaches the
 * rest of the machine; the regions it grows by come after that, from
 * vmm_init, once the page allocator can hand out any page there is. */
void mem_activate_pull_map(void)
{
    KHeapRegionsPullMap();
    slab_activate_pull_map();
}

void mem_init(void)
{
    klib_print_lock_init();

    size_t total_ram = pmm_total_pages() * VMM_PAGE_SIZE;
    void  *boot      = pmm_alloc(KLIB_HEAP_BOOTLOADER_SAFE_SIZE / VMM_PAGE_SIZE);
    if (!boot)
        panic("Failed to allocate the kernel heap's boot region from PMM!");
    KHeapRegionsInit(boot, KLIB_HEAP_BOOTLOADER_SAFE_SIZE);

    kprintf("[KLIB] Total RAM: %zu MB; the kernel heap starts from a %zu KB boot "
            "region and takes regions from the page allocator as it is asked, as far "
            "as the machine has room\n",
            total_ram / (1024 * 1024), (size_t)KLIB_HEAP_BOOTLOADER_SAFE_SIZE / 1024);

    slab_init();
}

/* ‼ WHAT THE KERNEL HEAP HOLDS FOR SOMEBODY RIGHT NOW — EVERY ROAD.
 *
 * The pool's blocks and the reserve's with their headers, the large pieces as
 * the pages they take, and the slab's objects at their class size — each road
 * counting what it handed out and has not had back. The difference of two
 * readings is what was kept in between — which is how a mount says what it
 * costs (tagfs.c, the "this mount keeps" line). */
size_t KHeapLiveBytes(void)
{
    return KHeapLiveOutsideSlab() + SlabLiveBytes();
}

/* The figure is evidence, so it is proven before anything reads it: a pool
 * block, a large piece and 64 slab objects of 64 bytes (4096 bytes at class
 * size, one more object than a 64-byte slab page holds) must each be counted,
 * and giving them back must bring the figure to exactly where it started.
 * Called from kernel_main before amp_init, on the BSP alone and after the
 * Pull Map — nothing else allocates while it runs, so every difference is
 * exact, and a large piece is a real one. A road that stopped counting fails
 * here, on every boot, before the first line that relies on the figure. */
void KHeapLiveSelfTest(void)
{
    static const size_t   pool_bytes  = 8192;
    static const size_t   large_bytes = 65536;
    static const size_t   obj_bytes   = 64;
    static void          *objs[64];
    const uint32_t        obj_count   = sizeof(objs) / sizeof(objs[0]);

    size_t start = KHeapLiveBytes();
    void  *block = kmalloc(pool_bytes);
    size_t after_pool = KHeapLiveBytes();
    void  *piece = kmalloc(large_bytes);
    size_t after_large = KHeapLiveBytes();

    uint32_t got = 0;
    for (uint32_t i = 0; i < obj_count; i++) {
        objs[i] = kmalloc(obj_bytes);
        if (objs[i])
            got++;
    }
    size_t after_slab = KHeapLiveBytes();

    bool large_road = piece && KHeapLargeOwns(piece) && block && KHeapPoolOwns(block);

    kfree(block);
    kfree(piece);
    for (uint32_t i = 0; i < obj_count; i++) {
        kfree(objs[i]);
        objs[i] = NULL;
    }
    size_t back = KHeapLiveBytes();

    /* Signed on purpose: a road that counts backwards is a failure to be
     * read as a small negative number, not as a wrapped enormous one. */
    long long pool_rise  = (long long)(after_pool - start);
    long long large_rise = (long long)(after_large - after_pool);
    long long slab_rise  = (long long)(after_slab - after_large);
    if (large_road && got == obj_count &&
        pool_rise >= (long long)pool_bytes && large_rise >= (long long)large_bytes &&
        slab_rise >= (long long)(obj_count * obj_bytes) && back == start) {
        kprintf("[KLIB] heap accounting self-test passed — a %zu-byte pool block, a "
                "%zu-byte large piece and %u slab objects of %zu bytes were counted "
                "(+%lld, +%lld, +%lld) and given back (%zu bytes live before and after)\n",
                pool_bytes, large_bytes, obj_count, obj_bytes,
                pool_rise, large_rise, slab_rise, back);
    } else {
        kprintf("[KLIB] heap accounting self-test FAILED: the pool rose %lld, the large "
                "piece %lld (%s), the slab %lld (%u of %u objects), back to %zu from %zu\n",
                pool_rise, large_rise, large_road ? "on its road" : "NOT on its road",
                slab_rise, got, obj_count, back, start);
    }
}

/* Bytes laid and read back in a pattern no other byte of the test lays. */
static void ReallocFill(uint8_t *bytes, size_t count, uint8_t seed)
{
    for (size_t i = 0; i < count; i++)
        bytes[i] = (uint8_t)(seed + i * 7u);
}

static bool ReallocHolds(const uint8_t *bytes, size_t count, uint8_t seed)
{
    for (size_t i = 0; i < count; i++)
        if (bytes[i] != (uint8_t)(seed + i * 7u))
            return false;
    return true;
}

/* krealloc proven on every road before anything leans on it, as the figure
 * above is — from kernel_main, on the BSP alone and after the Pull Map: a slab
 * object grown inside its class stays where it lies, and grown past the slab
 * moves to the pool with its bytes; a pool block shrunk stays; a large piece
 * grown inside its pages stays, and grown past them moves with its bytes; a
 * NULL block is a fresh one and size 0 gives a block back; and once all is
 * given back the heap holds, to the byte, what it held before. */
void KHeapReallocSelfTest(void)
{
    const size_t start = KHeapLiveBytes();
    const char  *broke = NULL;
    uint8_t     *next;

    /* One block through the slab and the pool — always the one still held,
     * since a krealloc refused leaves the old block the caller's. */
    uint8_t *block = kmalloc(40);
    if (!block)
        broke = "no slab object of 40 bytes was given";
    if (!broke) {
        ReallocFill(block, 40, 1);
        next = krealloc(block, 48);
        if (!next || next != block || !ReallocHolds(next, 40, 1))
            broke = "a slab object grown inside its class did not stay where it lay, its bytes as they were";
        if (next) block = next;
    }
    if (!broke) {
        next = krealloc(block, 3000);
        if (!next || next == block || slab_owns(next) || !ReallocHolds(next, 40, 1))
            broke = "a slab object grown past the slab did not move to the pool with its bytes";
        if (next) block = next;
    }
    if (!broke) {
        ReallocFill(block, 3000, 2);
        next = krealloc(block, 2000);
        if (!next || next != block || !ReallocHolds(next, 2000, 2))
            broke = "a pool block shrunk did not stay where it lay, its bytes as they were";
        if (next) block = next;
    }

    /* And one large piece. */
    uint8_t *piece = broke ? NULL : kmalloc(65536);
    if (!broke && (!piece || !KHeapLargeOwns(piece)))
        broke = "a piece of 64 KiB was not a large piece";
    if (!broke) {
        ReallocFill(piece, 65536, 3);
        next = krealloc(piece, 69000);
        if (!next || next != piece || !ReallocHolds(next, 65536, 3))
            broke = "a large piece grown inside its pages did not stay where it lay, its bytes as they were";
        if (next) piece = next;
    }
    if (!broke) {
        next = krealloc(piece, 131072);
        if (!next || next == piece || !ReallocHolds(next, 65536, 3))
            broke = "a large piece grown past its pages did not move with its bytes";
        if (next) piece = next;
    }

    if (!broke) {
        uint8_t *fresh = krealloc(NULL, 100);
        if (!fresh)
            broke = "a NULL block was not made a fresh one";
        else if (krealloc(fresh, 0) != NULL)
            broke = "size 0 did not give a block back";
    }

    kfree(block);
    kfree(piece);
    const size_t back = KHeapLiveBytes();
    if (!broke && back != start)
        broke = "what the heap held did not come back to where it started";

    if (!broke)
        kprintf("[KLIB] heap krealloc self-test passed — a slab object grown in its class and "
                "past it, a pool block shrunk, a large piece grown in its pages and past them, "
                "a NULL block and a block given back with size 0 (%zu bytes live before and after)\n",
                back);
    else
        kprintf("[KLIB] heap krealloc self-test FAILED: %s (%zu bytes live before, %zu after)\n",
                broke, start, back);
}

#ifdef CONFIG_VOLUME_TESTS
/*
 * HEAPFAIL (klib.h), with the plan under a sequence word: rewritten only while
 * the word is even (nothing armed), copied between two looks at it. Every
 * kmalloc on every core asks, so the question costs one load of one word while
 * nothing is armed and takes no lock.
 *
 * The count is written by the armed core alone — every other core, and every
 * interrupt taken on that one, is turned away before it — so it is a plain
 * increment stored atomically, never a read-modify-write two cores could race.
 */
typedef struct {
    uint8_t  core;
    uint32_t nth;
} HeapFailPlan;

static HeapFailPlan      g_heap_fail;
static volatile uint32_t g_heap_fail_gen;       /* odd while a plan stands */
static volatile uint32_t g_heap_fail_seen;
static volatile uint32_t g_heap_fail_fired;

bool HeapFailDisarm(void)
{
    uint32_t g = __atomic_load_n(&g_heap_fail_gen, __ATOMIC_ACQUIRE);
    while (g & 1u)
    {
        if (__atomic_compare_exchange_n(&g_heap_fail_gen, &g, g + 1u, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;
    }
    return false;
}

void HeapFailArm(uint8_t core, uint32_t nth)
{
    HeapFailDisarm();

    g_heap_fail.core = core;
    g_heap_fail.nth  = nth ? nth : 1u;
    __atomic_store_n(&g_heap_fail_seen, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_heap_fail_fired, 0u, __ATOMIC_RELAXED);

    __atomic_add_fetch(&g_heap_fail_gen, 1u, __ATOMIC_RELEASE);    /* even -> odd */
}

uint32_t HeapFailSeen(void)
{
    return __atomic_load_n(&g_heap_fail_seen, __ATOMIC_ACQUIRE);
}

bool HeapFailFired(void)
{
    return __atomic_load_n(&g_heap_fail_fired, __ATOMIC_ACQUIRE) != 0;
}

static bool HeapFailStrikes(size_t size)
{
    uint32_t g = __atomic_load_n(&g_heap_fail_gen, __ATOMIC_ACQUIRE);
    if (!(g & 1u))
        return false;

    HeapFailPlan plan = g_heap_fail;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_heap_fail_gen, __ATOMIC_RELAXED) != g)
        return false;

    uint8_t me = amp_get_core_index();
    if (me != plan.core)
        return false;               /* another core is not the code under test */
    if (IrqFrameInFlight() != NULL)
        return false;               /* nor is an interrupt taken on this one */

    uint32_t n = __atomic_load_n(&g_heap_fail_seen, __ATOMIC_RELAXED) + 1u;
    __atomic_store_n(&g_heap_fail_seen, n, __ATOMIC_RELEASE);
    if (n != plan.nth || __atomic_load_n(&g_heap_fail_fired, __ATOMIC_ACQUIRE))
        return false;
    __atomic_store_n(&g_heap_fail_fired, 1u, __ATOMIC_RELEASE);

    /* Said here, before any caller can act on the NULL: kprintf takes no
     * allocation of its own, and the caller may be about to print why it
     * failed. */
    kprintf("[HEAPFAIL] core %u: allocation %u since arming (%zu bytes) is "
            "answered NULL, as an exhausted heap answers — nothing was taken "
            "from the heap\n", me, n, size);
    return true;
}
#endif

/* A pool block wrapped in its canaries: the reserve's or the ordinary pool's. */
static void *KHeapGuardedTake(size_t size, KHeapCharge charge, bool reserve)
{
    size_t total_size = sizeof(heap_guard_t) + size + sizeof(uint64_t);
    heap_guard_t *guard = (heap_guard_t *)KHeapPoolTake(total_size, charge, reserve);

    if (!guard)
        return NULL;

    /* A pool block must never live on a page the slab registry claims —
     * that is two allocators owning one page. Catch the cohabitation at
     * birth, where the culprit is on the stack, not at the eventual free. */
    if (slab_owns((uint8_t *)guard + sizeof(heap_guard_t)))
        panic("[HEAP] kmalloc(%zu) pool block %p lands on a slab-registry page — "
              "allocator cohabitation\n",
              size, (void *)((uint8_t *)guard + sizeof(heap_guard_t)));

    guard->canary_start      = HEAP_CANARY_MAGIC;
    guard->size              = size;
    guard->caller            = "kmalloc";
    guard->canary_end_offset = sizeof(heap_guard_t) + size;

    uint64_t *end_canary = (uint64_t *)((uint8_t *)guard + guard->canary_end_offset);
    *end_canary = HEAP_CANARY_MAGIC;

    return (void *)((uint8_t *)guard + sizeof(heap_guard_t));
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    /* No road holds more than the machine has — and every size sum below is
     * then far from wrapping. */
    if (size > pmm_get_total_ram_bytes()) {
        kprintf("[KLIB] ERROR: kmalloc failed for %zu bytes — more than the %llu MB "
                "this machine has\n", size,
                (unsigned long long)(pmm_get_total_ram_bytes() / (1024 * 1024)));
        return NULL;
    }

#ifdef CONFIG_VOLUME_TESTS
    /* Before every road: a refused allocation takes nothing from any of them,
     * and is refused on whichever of them it would have used. */
    if (HeapFailStrikes(size))
        return NULL;
#endif

    KHeapScope who = KHeapScopeHere();

    /* One of the kernel's own paths: the reserve first; and when it can
     * neither serve nor grow, the heap every other path uses, on the
     * kernel's own account, which no share limits. */
    if (who.reserve) {
        void *kept = KHeapGuardedTake(size, KHEAP_CHARGE_KERNEL, true);
        if (kept)
            return kept;
        who.charge = KHEAP_CHARGE_KERNEL;
    }

    if (!KHeapChargeAdmits((KHeapCharge)who.charge, who.pid, size))
        return NULL;

    /* Slab path: O(1) for small allocations. */
    if (size <= SLAB_LARGE_THRESHOLD)
    {
        void *ptr = SlabAllocFor(size, (KHeapCharge)who.charge);
        if (ptr)
            return ptr;
        /* Fall through if slab exhausted — the pool handles it. */
    }

    if (KHeapIsLarge(size))
        return KHeapLargeTake(size, (KHeapCharge)who.charge);
    return KHeapGuardedTake(size, (KHeapCharge)who.charge, false);
}

/* The guard of a pool block, its canaries held: a pointer no pool region holds,
 * or a block whose guards are destroyed, stops the machine with the pointer and
 * `why` — the call that met it. The guard is reached through its Pull Map
 * alias. */
static heap_guard_t *PoolGuardChecked(void *ptr, const char *why)
{
    /* Ask the page map BEFORE dereferencing the guard header. A garbage
     * pointer (an overwritten local, a stale handle) used to reach the canary
     * loads below, read whatever happened to live at ptr-0x20, and die in a
     * SILENT cli;hlt behind debug_printf — which compiles to nothing in a
     * normal build. The machine froze without a word; naming the pointer
     * and halting loudly is the whole difference between a diagnosis and a
     * morning of guessing. panic() prints, dumps and stops every core. */
    heap_guard_t *guard = (heap_guard_t *)((uint8_t *)ptr - sizeof(heap_guard_t));

    if (!KHeapPoolOwns((uint8_t *)guard - sizeof(mem_block_t)))
        panic("[HEAP] %s(%p): not a heap pointer — no slab page, no large piece, "
              "no pool region holds it: freed twice, never allocated, or an "
              "overwritten local\n", why, ptr);

    /* A block handed out through the boot identity window and given back after
     * the Pull Map took over is reached through its Pull Map alias, as its
     * region's free list is: blocks of one region are ordered by address. */
    ptr   = vmm_phys_to_virt(vmm_virt_to_phys_direct(ptr));
    guard = (heap_guard_t *)((uint8_t *)ptr - sizeof(heap_guard_t));

    if (guard->canary_start != HEAP_CANARY_MAGIC) {
        /* Slab-lens on the pointer's page: a destroyed "canary" of 0 with a
         * page whose first words parse as a plausible SlabPage is the
         * signature of an impostor page handed out by the slab free-list
         * (see slab.c recruitment gates) — print the page's claimed
         * geometry so the corpse names its allocator. */
        const SlabPage *lens = (const SlabPage *)((uintptr_t)ptr & ~(uintptr_t)0xFFF);
        panic("[HEAP] %s(%p): start canary destroyed "
              "(expected 0x%llx, got 0x%llx); page lens: obj_size=%u "
              "slots=%u free=%u head=%u — impostor slab page, a neighbour "
              "overflow, or not a block start\n",
              why, ptr, HEAP_CANARY_MAGIC, guard->canary_start,
              lens->obj_size, lens->total_slots, lens->free_count, lens->free_head);
    }

    uint64_t *end_canary = (uint64_t *)((uint8_t *)guard + guard->canary_end_offset);
    if (*end_canary != HEAP_CANARY_MAGIC)
        panic("[HEAP] %s(%p): end canary destroyed (size=%zu, "
              "expected 0x%llx, got 0x%llx) — buffer overflow in this block\n",
              why, ptr, guard->size, HEAP_CANARY_MAGIC, *end_canary);
    return guard;
}

/* The pool block made to hold `size` bytes where it lies: true when its
 * payload — what the pool gave, which may be more than was asked — holds them
 * with its guards, its end canary moved; false, nothing changed, otherwise. */
static bool PoolResize(heap_guard_t *guard, size_t size)
{
    const mem_block_t *block = (const mem_block_t *)((const uint8_t *)guard - sizeof(mem_block_t));
    if (block->size < sizeof(heap_guard_t) + sizeof(uint64_t) ||
        size > block->size - sizeof(heap_guard_t) - sizeof(uint64_t))
        return false;
    guard->size              = size;
    guard->canary_end_offset = sizeof(heap_guard_t) + size;
    *(uint64_t *)((uint8_t *)guard + guard->canary_end_offset) = HEAP_CANARY_MAGIC;
    return true;
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    /* Where the block lies, when what it holds takes `size`; else the bytes of
     * it that are its owner's, carried into a block of their own. */
    size_t held;
    if (slab_owns(ptr)) {
        held = SlabObjectSize(ptr);
        if (size <= held)
            return ptr;
    } else if (KHeapLargeOwns(ptr)) {
        held = KHeapLargeSize(ptr);
        if (KHeapLargeResize(ptr, size))
            return ptr;
    } else {
        heap_guard_t *guard = PoolGuardChecked(ptr, "krealloc");
        held = guard->size;
        if (PoolResize(guard, size))
            return ptr;
    }

    void *fresh = kmalloc(size);
    if (!fresh)
        return NULL;
    memcpy(fresh, ptr, held < size ? held : size);
    kfree(ptr);
    return fresh;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    if (slab_owns(ptr))
    {
        slab_free(ptr);
        return;
    }

    if (KHeapLargeOwns(ptr))
    {
        KHeapLargeGive(ptr);
        return;
    }

    KHeapPoolGive(PoolGuardChecked(ptr, "kfree"));
}
