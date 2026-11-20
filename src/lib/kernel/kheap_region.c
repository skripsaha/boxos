/* kheap_region.c — where the kernel heap's memory comes from, and goes back to.
 *
 * ‼ THE HEAP IS AS BIG AS THE MACHINE LETS IT BE, NOT AS ONE BLOCK OF THE
 * BUDDY.
 *
 * It used to be one pool, sized once at boot as a fraction of RAM and clamped
 * to the largest block the page allocator hands out — 16 MiB on a machine with
 * a gigabyte, 64 MiB on any machine at all — and everything the kernel was
 * ever asked to hold had to fit in it. Now the pool is a list of REGIONS: the
 * boot region, which is all the addressing there is before the Pull Map, and
 * then whole runs of pages taken from the page allocator whenever no region
 * has room. Each is reached through the Pull Map, which maps every page of the
 * machine from boot on, so growing takes no new mapping and no shootdown: it
 * is a page-allocator call under the buddy's own lock, and that is safe
 * wherever kmalloc is — under process_lock, under an index's IRQ-off lock.
 *
 * A region that empties goes back to the page allocator, except one kept in
 * hand, so an allocation that crosses the edge back and forth does not take
 * and return a region each time. A region grows the pool geometrically — as
 * big as the pool already is, up to the buddy's largest block — so a pool
 * that grew to a size reached it in a handful of regions. Blocks never merge
 * across regions, and the oldest region is asked first, so the newest are the
 * ones that drain and go back.
 *
 * An allocation of KHEAP_LARGE_MIN_PAGES pages or more is a LARGE PIECE: its
 * own exact run of pages from the page allocator, given back to it on kfree.
 *
 * The RESERVE (kheap.h) is regions of its own that only the kernel's own paths
 * are served from. It grows by exactly what it was short and never gives a
 * page back.
 *
 * Which allocator owns a pointer is a question only the allocator may answer
 * (see the slab's page registry), so two page maps answer it here: the pages
 * of every pool region, and the first page of every large piece.
 */
#include "kheap_internal.h"
#include "pmm.h"
#include "vmm.h"

// ----------------------------------------------------------------------------
// Page maps
// ----------------------------------------------------------------------------

typedef struct {
    uint8_t  *bits;
    uintptr_t phys;
    size_t    pages;     /* bits: every physical page up to the end of RAM */
} KHeapPageMap;

static KHeapPageMap g_region_pages;   /* the page belongs to a pool region */
static KHeapPageMap g_large_heads;    /* a large piece begins on the page */

/* Taken at mem_init, when the identity window is the only addressing there
 * is; the page allocator hands out its lowest pages first, which the window
 * reaches. One bit per page: 32 KiB per GiB of RAM, each. */
static void PageMapInit(KHeapPageMap *map, const char *what)
{
    map->pages = (size_t)(pmm_get_mem_end() / VMM_PAGE_SIZE) + 1;
    size_t bytes = (map->pages + 7) / 8;
    size_t pages = (bytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    void  *phys  = pmm_alloc_zero(pages);
    if (!phys)
        panic("[KLIB] cannot allocate the page map of %s (%zu pages)", what, pages);
    map->phys = (uintptr_t)phys;
    map->bits = (uint8_t *)vmm_phys_to_virt(map->phys);
}

static void PageMapMark(KHeapPageMap *map, uintptr_t phys, size_t pages, bool on)
{
    for (size_t i = 0; i < pages; i++) {
        size_t idx = phys / VMM_PAGE_SIZE + i;
        if (idx >= map->pages)
            return;
        uint8_t bit = (uint8_t)(1u << (idx & 7u));
        if (on)
            __atomic_fetch_or(&map->bits[idx >> 3], bit, __ATOMIC_RELEASE);
        else
            __atomic_fetch_and(&map->bits[idx >> 3], (uint8_t)~bit, __ATOMIC_RELEASE);
    }
}

static bool PageMapTest(const KHeapPageMap *map, uintptr_t phys)
{
    size_t idx = phys / VMM_PAGE_SIZE;
    if (idx >= map->pages)
        return false;
    return (__atomic_load_n(&map->bits[idx >> 3], __ATOMIC_ACQUIRE) >> (idx & 7u)) & 1u;
}

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------

static spinlock_t   g_heap_lock;
static KHeapRegion *g_regions;        /* ordinary: the boot region, then the grown, oldest first */
static KHeapRegion *g_reserve;        /* the reserve's, oldest first */
static KHeapRegion *g_in_hand;        /* the one empty grown region kept */
static bool         g_heap_grows;     /* the Pull Map reaches every page the allocator gives */
static size_t       g_boot_bytes;

static uint32_t g_region_count, g_reserve_count, g_large_count;
static size_t   g_region_bytes, g_reserve_bytes, g_large_bytes;
static size_t   g_pool_live, g_reserve_live;

/* What the heap holds for somebody outside the slab, at its highest. An
 * instantaneous figure always reads fine, because the peak has drained by the
 * time anyone asks, so the peak is said on each DOUBLING from
 * HEAP_PEAK_REPORT_FLOOR up: at most log2(peak/floor) lines, each a real
 * change of magnitude, on the smallest machine and on the largest alike. */
#define HEAP_PEAK_REPORT_FLOOR (256u * 1024u)
static size_t g_heap_peak, g_heap_max_1, g_heap_peak_reported;

static inline size_t LiveLocked(void)
{
    return g_pool_live + g_reserve_live + g_large_bytes;
}

/* After a rise, under the lock: whether a new doubling of the peak is due,
 * and the figures to say it with once the lock is let go. */
static bool PeakRose(size_t asked, size_t *peak, size_t *largest)
{
    if (asked > g_heap_max_1)
        g_heap_max_1 = asked;
    size_t live = LiveLocked();
    if (live <= g_heap_peak)
        return false;
    g_heap_peak = live;
    size_t next = g_heap_peak_reported ? g_heap_peak_reported * 2 : HEAP_PEAK_REPORT_FLOOR;
    if (g_heap_peak < next)
        return false;
    g_heap_peak_reported = next;
    *peak    = g_heap_peak;
    *largest = g_heap_max_1;
    return true;
}

static void SayPeak(size_t peak, size_t largest)
{
    kprintf("[KLIB] heap high-water %zu KB, largest request %zu B\n", peak / 1024, largest);
}

// ----------------------------------------------------------------------------
// Regions
// ----------------------------------------------------------------------------

/* Where a free block keeps its place on its list: the first bytes of its
 * payload, every payload being at least 32 bytes. */
typedef struct {
    uint32_t next;     /* the next block of its list, as a distance from the region's start; 0 none */
    uint32_t prev;     /* the one before it; 0 when it is the first */
} FreeLinks;

static inline mem_block_t *BlockAt(KHeapRegion *region, uint32_t at)
{
    return (mem_block_t *)((uint8_t *)region + at);
}

static inline uint32_t BlockOffset(KHeapRegion *region, mem_block_t *block)
{
    return (uint32_t)((uint8_t *)block - (uint8_t *)region);
}

static inline FreeLinks *LinksOf(mem_block_t *block)
{
    return (FreeLinks *)((uint8_t *)block + sizeof(mem_block_t));
}

/* The block just above `block` in its region; NULL past the last. */
static inline mem_block_t *BlockAbove(KHeapRegion *region, mem_block_t *block)
{
    uint8_t *above = (uint8_t *)block + sizeof(mem_block_t) + block->size;
    return above < (uint8_t *)region + region->bytes ? (mem_block_t *)above : NULL;
}

static inline uint32_t HighBit(uint32_t v)
{
    return 31u - (uint32_t)__builtin_clz(v);
}

/* The list a free block of `size` payload bytes is kept on: its class, the
 * power of two of its 32-byte units, and its step within the class. */
static void ListOf(size_t size, uint32_t *cls, uint32_t *step)
{
    uint32_t units = (uint32_t)(size / KLIB_BLOCK_ALIGNMENT);
    uint32_t c     = HighBit(units);
    *cls  = c;
    *step = c < KHEAP_SIZE_STEP_BITS ? units - (1u << c)
                                     : (units >> (c - KHEAP_SIZE_STEP_BITS)) & (KHEAP_SIZE_STEPS - 1u);
}

static void ListPush(KHeapRegion *region, mem_block_t *block)
{
    uint32_t cls, step;
    ListOf(block->size, &cls, &step);
    uint32_t   at    = BlockOffset(region, block);
    uint32_t   first = region->lists[cls][step];
    FreeLinks *links = LinksOf(block);
    links->next = first;
    links->prev = 0;
    if (first) LinksOf(BlockAt(region, first))->prev = at;
    region->lists[cls][step] = at;
    region->steps[cls]      |= (uint8_t)(1u << step);
    region->classes         |= 1u << cls;
}

static void ListTake(KHeapRegion *region, mem_block_t *block)
{
    uint32_t cls, step;
    ListOf(block->size, &cls, &step);
    FreeLinks *links = LinksOf(block);
    if (links->prev) LinksOf(BlockAt(region, links->prev))->next = links->next;
    else             region->lists[cls][step] = links->next;
    if (links->next) LinksOf(BlockAt(region, links->next))->prev = links->prev;
    if (!region->lists[cls][step]) {
        region->steps[cls] &= (uint8_t)~(1u << step);
        if (!region->steps[cls]) region->classes &= ~(1u << cls);
    }
}

/* A free block of the region, as kmalloc found it: corruption said where it
 * is found, not an owner later. */
static mem_block_t *FreeChecked(KHeapRegion *region, uint32_t at)
{
    mem_block_t *block = BlockAt(region, at);
    if (at < KHEAP_REGION_HEAD || at >= region->bytes)
        panic("kmalloc corruption: free block 0x%p out of its region [0x%p - 0x%p]",
              block, (uint8_t *)region + KHEAP_REGION_HEAD, (uint8_t *)region + region->bytes);
    if (block->magic != KLIB_MAGIC_FREE)
        panic("Memory corruption detected in kmalloc (magic=0x%x, expected FREE)!", block->magic);
    return block;
}

static KHeapRegion *RegionLay(uintptr_t phys, size_t bytes, uint8_t kind)
{
    KHeapRegion *region = (KHeapRegion *)vmm_phys_to_virt(phys);
    mem_block_t *all    = (mem_block_t *)((uint8_t *)region + KHEAP_REGION_HEAD);

    if (bytes / KLIB_BLOCK_ALIGNMENT >= (1ull << KHEAP_SIZE_CLASSES))
        panic("[HEAP] a region of %zu bytes is past what its size classes hold", bytes);
    memset(region, 0, KHEAP_REGION_HEAD);
    region->magic = KHEAP_REGION_MAGIC;
    region->kind  = kind;
    region->bytes = bytes;

    all->size   = bytes - KHEAP_REGION_HEAD - sizeof(mem_block_t);
    all->below  = 0;
    all->back   = KHEAP_REGION_HEAD;
    all->magic  = KLIB_MAGIC_FREE;
    all->charge = KHEAP_CHARGE_KERNEL;
    ListPush(region, all);

    PageMapMark(&g_region_pages, phys, bytes / VMM_PAGE_SIZE, true);
    return region;
}

/* A free block of the region of at least `size` payload bytes, off its list:
 * the first of the first list whose every block fits — the list `size` rounds
 * up to, or any after it, found by the bitmaps — else the first that fits on
 * the list `size` itself belongs to, whose blocks are within an eighth of it.
 * NULL when the region has none. */
static mem_block_t *RegionFind(KHeapRegion *region, size_t size)
{
    uint32_t units = (uint32_t)(size / KLIB_BLOCK_ALIGNMENT);
    uint32_t c     = HighBit(units);
    uint32_t up    = c < KHEAP_SIZE_STEP_BITS ? units : units + (1u << (c - KHEAP_SIZE_STEP_BITS)) - 1u;
    uint32_t cls, step;
    ListOf((size_t)up * KLIB_BLOCK_ALIGNMENT, &cls, &step);
    if (cls < KHEAP_SIZE_CLASSES) {
        uint32_t steps = region->steps[cls] & (uint32_t)(0xFFu << step);
        if (!steps) {
            uint32_t classes = cls + 1u < 32u ? region->classes & ~((2u << cls) - 1u) : 0u;
            if (classes) {
                cls   = (uint32_t)__builtin_ctz(classes);
                steps = region->steps[cls];
            }
        }
        if (steps) {
            mem_block_t *block = FreeChecked(region, region->lists[cls][__builtin_ctz(steps)]);
            ListTake(region, block);
            return block;
        }
    }
    ListOf(size, &cls, &step);
    for (uint32_t at = region->lists[cls][step]; at;) {
        mem_block_t *block = FreeChecked(region, at);
        if (block->size >= size) {
            ListTake(region, block);
            return block;
        }
        at = LinksOf(block)->next;
    }
    return NULL;
}

/* A block of `size` payload bytes carved from the region — what is left past
 * it, when it would hold a block of its own, given back to the lists — and
 * counted live. NULL when the region has no room. The caller holds the lock;
 * `size` is a multiple of KLIB_BLOCK_ALIGNMENT. */
static mem_block_t *RegionCarve(KHeapRegion *region, size_t size)
{
    mem_block_t *block = RegionFind(region, size);
    if (!block)
        return NULL;
    if (block->size > size + sizeof(mem_block_t) + KLIB_BLOCK_ALIGNMENT) {
        mem_block_t *rest = (mem_block_t *)((uint8_t *)block + sizeof(mem_block_t) + size);
        rest->size   = block->size - size - sizeof(mem_block_t);
        rest->below  = sizeof(mem_block_t) + size;
        rest->back   = (uint64_t)((uint8_t *)rest - (uint8_t *)region);
        rest->magic  = KLIB_MAGIC_FREE;
        rest->charge = KHEAP_CHARGE_KERNEL;
        block->size  = size;
        mem_block_t *above = BlockAbove(region, rest);
        if (above) above->below = sizeof(mem_block_t) + rest->size;
        ListPush(region, rest);
    }
    block->magic  = KLIB_MAGIC_NUMBER;
    region->live += block->size + sizeof(mem_block_t);
    return block;
}

/* A free block back onto its region's lists, merged with a free neighbour
 * below and above. The caller holds the lock and has marked it free. */
static void RegionRelease(KHeapRegion *region, mem_block_t *block)
{
    mem_block_t *above = BlockAbove(region, block);
    if (above && above->magic == KLIB_MAGIC_FREE) {
        ListTake(region, above);
        block->size += sizeof(mem_block_t) + above->size;
    }
    if (block->below) {
        mem_block_t *below = (mem_block_t *)((uint8_t *)block - block->below);
        if (below->magic == KLIB_MAGIC_FREE) {
            ListTake(region, below);
            below->size += sizeof(mem_block_t) + block->size;
            block = below;
        }
    }
    above = BlockAbove(region, block);
    if (above) above->below = (uint64_t)((uint8_t *)above - (uint8_t *)block);
    ListPush(region, block);
}

static void RegionAppend(KHeapRegion **list, KHeapRegion *region)
{
    while (*list)
        list = &(*list)->next;
    *list = region;
}

static void RegionUnlink(KHeapRegion **list, KHeapRegion *region)
{
    while (*list && *list != region)
        list = &(*list)->next;
    if (*list)
        *list = region->next;
}

/* A run of pages for a region that must hold a block of `size`: for the
 * ordinary pool as big as the pool already is, up to the buddy's largest
 * block and never less than the block needs, halving toward that need when
 * the machine has no run that long; for the reserve exactly what it was
 * short. 0 when the machine has no run even for the block alone. */
static uintptr_t RegionRunFor(size_t size, bool reserve, size_t *out_bytes)
{
    size_t need = ALIGN_UP(KHEAP_REGION_HEAD + sizeof(mem_block_t) + size, VMM_PAGE_SIZE);
    size_t ask  = need;

    if (!reserve) {
        size_t largest = pmm_max_alloc_pages() * VMM_PAGE_SIZE;
        size_t pool    = __atomic_load_n(&g_region_bytes, __ATOMIC_RELAXED);
        ask = MAX(need, MIN(pool, largest));
    }
    for (;;) {
        void *run = PmmTakeExact(ask / VMM_PAGE_SIZE);
        if (run) {
            *out_bytes = ask;
            return (uintptr_t)run;
        }
        if (ask == need)
            return 0;
        ask = MAX(need, ALIGN_UP(ask / 2, VMM_PAGE_SIZE));
    }
}

static void SayMachineShort(size_t asked, size_t pages, const char *what)
{
    kprintf("[KLIB] ERROR: kmalloc failed for %zu bytes — the machine has no run of "
            "%zu page(s) for %s: %zu KB free in the page allocator, and the heap "
            "holds %zu KB in %u region(s) and %zu KB in %u large piece(s)\n",
            asked, pages, what, PmmFreePagesNow() * (VMM_PAGE_SIZE / 1024),
            g_region_bytes / 1024, g_region_count, g_large_bytes / 1024, g_large_count);
}

/* A block taken, counted, under the lock: charged to `charge` — the block as
 * carved, which kfree gives back, not the bytes asked — unless it is the
 * reserve's, and the region kept in hand is in hand no longer. */
static void *BlockTaken(KHeapRegion *region, mem_block_t *block, KHeapCharge charge,
                        size_t asked, size_t *peak, size_t *largest, bool *say_peak)
{
    size_t took = block->size + sizeof(mem_block_t);

    if (region->kind == KHEAP_REGION_RESERVE) {
        block->charge   = KHEAP_CHARGE_KERNEL;
        g_reserve_live += took;
    } else {
        block->charge   = (uint8_t)charge;
        g_pool_live    += took;
        KHeapChargeAdd(charge, took);
    }
    if (region == g_in_hand)
        g_in_hand = NULL;
    *say_peak = PeakRose(asked, peak, largest);
    return (uint8_t *)block + sizeof(mem_block_t);
}

void *KHeapPoolTake(size_t bytes, KHeapCharge charge, bool reserve)
{
    size_t size = ALIGN_UP(bytes, KLIB_BLOCK_ALIGNMENT);
    size_t peak = 0, largest = 0;
    bool   say_peak = false;
    void  *payload  = NULL;

    spin_lock(&g_heap_lock);
    for (KHeapRegion *r = reserve ? g_reserve : g_regions; r && !payload; r = r->next) {
        mem_block_t *block = RegionCarve(r, size);
        if (block)
            payload = BlockTaken(r, block, charge, bytes, &peak, &largest, &say_peak);
    }
    bool grows = g_heap_grows;
    spin_unlock(&g_heap_lock);

    if (payload) {
        if (say_peak)
            SayPeak(peak, largest);
        return payload;
    }

    if (!grows) {
        kprintf("[KLIB] ERROR: kmalloc failed for %zu bytes — before the Pull Map the "
                "heap is its %zu KB boot region alone, and that has no room\n",
                bytes, g_boot_bytes / 1024);
        return NULL;
    }

    size_t    region_bytes = 0;
    uintptr_t run          = RegionRunFor(size, reserve, &region_bytes);
    if (!run) {
        size_t pages = ALIGN_UP(KHEAP_REGION_HEAD + sizeof(mem_block_t) + size,
                                VMM_PAGE_SIZE) / VMM_PAGE_SIZE;
        if (reserve)
            kprintf("[KLIB] the reserve for the kernel's own paths is %zu bytes short and "
                    "the machine has no run of %zu page(s) to grow it by (%zu KB free) — "
                    "that path is served, if at all, by the heap every other path uses\n",
                    bytes, pages, PmmFreePagesNow() * (VMM_PAGE_SIZE / 1024));
        else
            SayMachineShort(bytes, pages, "a region to hold it");
        return NULL;
    }

    KHeapRegion *region = RegionLay(run, region_bytes,
                                    reserve ? KHEAP_REGION_RESERVE : KHEAP_REGION_GROWN);

    spin_lock(&g_heap_lock);
    if (reserve) {
        RegionAppend(&g_reserve, region);
        g_reserve_count++;
        g_reserve_bytes += region_bytes;
    } else {
        RegionAppend(&g_regions, region);
        g_region_count++;
        g_region_bytes += region_bytes;
    }
    mem_block_t *block = RegionCarve(region, size);    /* a fresh region holds it: laid for it */
    payload = BlockTaken(region, block, charge, bytes, &peak, &largest, &say_peak);
    size_t   total = reserve ? g_reserve_bytes : g_region_bytes;
    uint32_t count = reserve ? g_reserve_count : g_region_count;
    spin_unlock(&g_heap_lock);

    if (reserve)
        kprintf("[KLIB] the reserve for the kernel's own paths was %zu bytes short and "
                "grows by %zu KB, to %zu KB in %u region(s) — what those paths have "
                "held at once\n", bytes, region_bytes / 1024, total / 1024, count);
    else
        kprintf("[KLIB] the kernel heap takes a region of %zu KB from the page allocator: "
                "%zu KB in %u region(s) now\n", region_bytes / 1024, total / 1024, count);
    if (say_peak)
        SayPeak(peak, largest);
    return payload;
}

bool KHeapPoolOwns(const void *addr)
{
    return PageMapTest(&g_region_pages, vmm_virt_to_phys_direct((void *)addr));
}

void KHeapPoolGive(void *payload)
{
    mem_block_t *block  = (mem_block_t *)((uint8_t *)payload - sizeof(mem_block_t));
    KHeapRegion *region = (KHeapRegion *)((uint8_t *)block - block->back);

    /* The block is on a region's page — the caller asked the page map — but
     * its `back` is a word in memory like any other: the region it names is
     * asked the same question before a byte of it is read. */
    if (block->back < KHEAP_REGION_HEAD || !KHeapPoolOwns(region) ||
        region->magic != KHEAP_REGION_MAGIC || block->back >= region->bytes)
        panic("[HEAP] kfree(%p): its block names no region (back=0x%llx) — not a block "
              "start, or a neighbour overflowed into its header",
              payload, (unsigned long long)block->back);

    KHeapRegion *back_to_machine = NULL;
    size_t       back_bytes      = 0;

    spin_lock(&g_heap_lock);
    /* O(1) double-free detection via magic tombstone — and the tombstone laid
     * under the lock: a neighbour given back on another core merges with a
     * block whose magic says FREE, and takes it off its list, so a block is
     * FREE only once it is on one. */
    if (block->magic == KLIB_MAGIC_FREE)
        panic("Double free detected (magic=FREE)!");
    if (block->magic != KLIB_MAGIC_NUMBER)
        panic("Invalid free: bad magic number!");
    block->magic = KLIB_MAGIC_FREE;

    KHeapCharge charge = (KHeapCharge)block->charge;
    size_t      took   = block->size + sizeof(mem_block_t);
    uint8_t     kind   = region->kind;
    region->live -= took;
    if (kind == KHEAP_REGION_RESERVE)
        g_reserve_live -= took;
    else
        g_pool_live -= took;
    RegionRelease(region, block);

    if (kind == KHEAP_REGION_GROWN && region->live == 0) {
        if (!g_in_hand) {
            g_in_hand = region;
        } else if (g_in_hand != region) {
            RegionUnlink(&g_regions, region);
            g_region_count--;
            g_region_bytes -= region->bytes;
            back_bytes      = region->bytes;
            back_to_machine = region;
        }
    }
    spin_unlock(&g_heap_lock);

    if (kind != KHEAP_REGION_RESERVE)
        KHeapChargeSub(charge, took);
    if (back_to_machine) {
        uintptr_t phys = vmm_virt_to_phys_direct(back_to_machine);
        PageMapMark(&g_region_pages, phys, back_bytes / VMM_PAGE_SIZE, false);
        PmmGiveExact((void *)phys, back_bytes / VMM_PAGE_SIZE);
    }
}

// ----------------------------------------------------------------------------
// Large pieces
// ----------------------------------------------------------------------------

static size_t LargePages(size_t size)
{
    return (sizeof(KHeapLargeHead) + size + sizeof(uint64_t) + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
}

/* Before the Pull Map a large request is carved from the boot region, as
 * everything was: the page allocator's runs are not all in reach yet. */
bool KHeapIsLarge(size_t size)
{
    return __atomic_load_n(&g_heap_grows, __ATOMIC_ACQUIRE) &&
           LargePages(size) >= KHEAP_LARGE_MIN_PAGES;
}

void *KHeapLargeTake(size_t size, KHeapCharge charge)
{
    size_t pages = LargePages(size);
    void  *run   = PmmTakeExact(pages);
    if (!run) {
        SayMachineShort(size, pages, "it");
        return NULL;
    }

    KHeapLargeHead *head = (KHeapLargeHead *)vmm_phys_to_virt((uintptr_t)run);
    head->magic  = KHEAP_LARGE_MAGIC;
    head->pages  = pages;
    head->size   = size;
    head->charge = charge;
    head->canary = HEAP_CANARY_MAGIC;
    *(uint64_t *)((uint8_t *)(head + 1) + size) = HEAP_CANARY_MAGIC;
    PageMapMark(&g_large_heads, (uintptr_t)run, 1, true);

    size_t peak = 0, largest = 0;
    spin_lock(&g_heap_lock);
    g_large_count++;
    g_large_bytes += pages * VMM_PAGE_SIZE;
    bool say_peak = PeakRose(size, &peak, &largest);
    spin_unlock(&g_heap_lock);

    KHeapChargeAdd(charge, pages * VMM_PAGE_SIZE);
    if (say_peak)
        SayPeak(peak, largest);
    return head + 1;
}

bool KHeapLargeOwns(const void *ptr)
{
    uintptr_t head = (uintptr_t)ptr - sizeof(KHeapLargeHead);
    if (head % VMM_PAGE_SIZE)
        return false;
    return PageMapTest(&g_large_heads, vmm_virt_to_phys_direct((void *)head));
}

/* A large piece's head, its guards held: a destroyed one stops the machine
 * with the piece named and `why` — the call that met it. */
static KHeapLargeHead *LargeHeadChecked(void *ptr, const char *why)
{
    KHeapLargeHead *head = (KHeapLargeHead *)ptr - 1;

    if (head->magic != KHEAP_LARGE_MAGIC || head->canary != HEAP_CANARY_MAGIC)
        panic("[HEAP] %s(%p): a large piece's head is destroyed (magic 0x%llx, "
              "canary 0x%llx) — an underflow of the piece, or a block of another "
              "allocator", why, ptr, (unsigned long long)head->magic,
              (unsigned long long)head->canary);
    if (head->pages != LargePages(head->size))
        panic("[HEAP] %s(%p): a large piece says %zu pages for %zu bytes — its head "
              "was overwritten", why, ptr, head->pages, head->size);
    uint64_t end = *(uint64_t *)((uint8_t *)ptr + head->size);
    if (end != HEAP_CANARY_MAGIC)
        panic("[HEAP] %s(%p): end canary destroyed (size=%zu, expected 0x%llx, got "
              "0x%llx) — buffer overflow in this block\n",
              why, ptr, head->size, HEAP_CANARY_MAGIC, (unsigned long long)end);
    return head;
}

size_t KHeapLargeSize(void *ptr)
{
    return LargeHeadChecked(ptr, "krealloc")->size;
}

bool KHeapLargeResize(void *ptr, size_t size)
{
    KHeapLargeHead *head = LargeHeadChecked(ptr, "krealloc");
    if (LargePages(size) != head->pages)
        return false;
    head->size = size;
    *(uint64_t *)((uint8_t *)ptr + size) = HEAP_CANARY_MAGIC;
    return true;
}

void KHeapLargeGive(void *ptr)
{
    KHeapLargeHead *head = LargeHeadChecked(ptr, "kfree");

    size_t      pages  = head->pages;
    KHeapCharge charge = (KHeapCharge)head->charge;
    uintptr_t   phys   = vmm_virt_to_phys_direct(head);

    head->magic = 0;
    PageMapMark(&g_large_heads, phys, 1, false);

    spin_lock(&g_heap_lock);
    g_large_count--;
    g_large_bytes -= pages * VMM_PAGE_SIZE;
    spin_unlock(&g_heap_lock);

    KHeapChargeSub(charge, pages * VMM_PAGE_SIZE);
    PmmGiveExact((void *)phys, pages);
}

// ----------------------------------------------------------------------------
// Boot, the Pull Map, and what the heap says about itself
// ----------------------------------------------------------------------------

void KHeapRegionsInit(void *boot, size_t bytes)
{
    spinlock_init(&g_heap_lock);
    PageMapInit(&g_region_pages, "the pool's regions");
    PageMapInit(&g_large_heads, "the large pieces");

    g_boot_bytes   = bytes;
    g_regions      = RegionLay((uintptr_t)boot, bytes, KHEAP_REGION_BOOT);
    g_region_count = 1;
    g_region_bytes = bytes;
}

/* Every pointer the heap keeps moves from the identity window to the Pull
 * Map; its blocks and their lists keep only distances. Called by vmm_init after the page
 * allocator is rebased — growth asks it for pages — single-threaded, before
 * the APs boot, so no lock is taken. From here on the heap grows. */
void KHeapRegionsPullMap(void)
{
    g_region_pages.bits = (uint8_t *)vmm_phys_to_virt(g_region_pages.phys);
    g_large_heads.bits  = (uint8_t *)vmm_phys_to_virt(g_large_heads.phys);

    g_regions = (KHeapRegion *)vmm_phys_to_virt((uintptr_t)g_regions);
    __atomic_store_n(&g_heap_grows, true, __ATOMIC_RELEASE);
}

size_t KHeapLiveOutsideSlab(void)
{
    spin_lock(&g_heap_lock);
    size_t live = LiveLocked();
    spin_unlock(&g_heap_lock);
    return live;
}

/* What the ordinary regions hold free — memory any class could still be
 * served from. Read without the lock: a policy figure, a block behind at
 * worst. */
size_t KHeapRegionSlack(void)
{
    size_t bytes = __atomic_load_n(&g_region_bytes, __ATOMIC_RELAXED);
    size_t live  = __atomic_load_n(&g_pool_live, __ATOMIC_RELAXED);
    return bytes > live ? bytes - live : 0;
}

void KHeapShapeNow(KHeapShape *out)
{
    spin_lock(&g_heap_lock);
    out->regions         = g_region_count;
    out->region_bytes    = g_region_bytes;
    out->pool_live       = g_pool_live;
    out->large           = g_large_count;
    out->large_bytes     = g_large_bytes;
    out->reserve_regions = g_reserve_count;
    out->reserve_bytes   = g_reserve_bytes;
    out->reserve_live    = g_reserve_live;
    spin_unlock(&g_heap_lock);
}
