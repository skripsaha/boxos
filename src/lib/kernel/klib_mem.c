/* klib_mem.c — kernel heap allocator.
 *
 * Two-tier design:
 *   1. Slab — O(1) for allocations <= SLAB_LARGE_THRESHOLD (2048 bytes).
 *   2. Legacy first-fit pool — for everything larger, plus fallback when
 *      a slab class is empty and PMM cannot grow it.
 *
 * The pool itself lives at `memory_pool` (originally identity-mapped, later
 * rebased through Pull Map by `mem_activate_pull_map`). Each live block
 * carries `KLIB_MAGIC_NUMBER`; every free block carries `KLIB_MAGIC_FREE`.
 * Double-free is detected in O(1) by inspecting the tombstone — without
 * walking the free list — which used to be the dominant cost under
 * sustained IPC pressure (touch_stress, write_concurrent).
 *
 * HEAP_GUARD_ENABLED wraps every legacy allocation in canary words. Slab
 * allocations get their own validation via `slab_owns()`.
 *
 * mem_init also bootstraps the console-print lock by calling
 * klib_print_lock_init() — it lives in klib_print.c but must be wired in
 * one place so the spinlock_t invariants are initialised before the first
 * kprintf-class call. */
#include "klib.h"
#include "slab.h"
#include "pmm.h"
#include "vmm.h"

#define HEAP_CANARY_MAGIC  0xDEADBEEFCAFEBABEULL
#define HEAP_GUARD_ENABLED 1

typedef struct
{
    uint64_t canary_start;
    size_t size;
    const char *caller;
    size_t canary_end_offset;
} heap_guard_t;

static uint8_t      *memory_pool = NULL;
/* PUBLISHED size — what the free list covers and what the range checks accept.
 * Before the Pull Map is up it is only the part the bootloader's identity
 * window can reach; mem_activate_pull_map raises it to memory_pool_full. */
static size_t        memory_pool_size = 0;
static size_t        memory_pool_full = 0;   /* size actually allocated at boot */
static mem_block_t  *free_list = NULL;
static spinlock_t    heap_lock = {0};

/* Pool occupancy, maintained under heap_lock.  The pool is sized once at boot,
 * so how close it runs to full is the difference between a healthy system and
 * every caller's error-unwind path firing at once.  A high-water mark is the
 * only way to see that coming: instantaneous stats always read fine, because
 * the peak has already drained by the time anyone asks.
 *
 * Reported on each DOUBLING of the peak from HEAP_PEAK_REPORT_FLOOR up, not at
 * fractions of the pool: a fraction of a large pool is never crossed by a
 * healthy system, so the reporter would go silent exactly on the machines with
 * the most room to misjudge.  Doubling gives at most log2(pool/floor) lines —
 * eight for a 64 MiB pool — and each one is a real change of magnitude. */
#define HEAP_PEAK_REPORT_FLOOR (256u * 1024u)
static size_t g_heap_live  = 0;
static size_t g_heap_peak  = 0;
static size_t g_heap_max_1 = 0;   /* largest single request ever served */
static size_t g_heap_peak_reported = 0;

/* Declared in klib_print.c — initialises g_kprintf_lock once. */
void klib_print_lock_init(void);

void mem_activate_pull_map(void)
{
    /* Rebase the pool head and every free-list link from identity-virtual
     * to Pull Map-virtual.  identity == phys before activation, so the
     * conversion is one vmm_phys_to_virt per pointer. */
    uintptr_t pool_phys = (uintptr_t)memory_pool;
    memory_pool = (uint8_t *)vmm_phys_to_virt(pool_phys);

    if (free_list)
    {
        free_list = (mem_block_t *)vmm_phys_to_virt((uintptr_t)free_list);

        mem_block_t *block = free_list;
        while (block)
        {
            if (block->next)
                block->next = (mem_block_t *)vmm_phys_to_virt((uintptr_t)block->next);
            block = block->next;
        }
    }

    slab_activate_pull_map();

    /* Publish the tail. Every physical byte is addressable now, so the part of
     * the pool the identity window could not reach joins the free list. The
     * pool is ONE contiguous block, so this is an append — and a merge when the
     * highest free block ends exactly where the tail begins — not a second
     * region: the free list stays address-ordered and the range checks stay a
     * single interval. Runs single-threaded during vmm_init, before the APs
     * boot, so no lock is taken. */
    if (memory_pool_full > memory_pool_size)
    {
        uint8_t *tail  = memory_pool + memory_pool_size;
        size_t   added = memory_pool_full - memory_pool_size;
        memset(tail, 0, added);

        mem_block_t *blk = (mem_block_t *)tail;
        blk->size  = added - sizeof(mem_block_t);
        blk->next  = NULL;
        blk->magic = KLIB_MAGIC_FREE;

        memory_pool_size = memory_pool_full;

        mem_block_t *last = free_list;
        while (last && last->next)
            last = last->next;

        if (!last)
            free_list = blk;
        else if ((uint8_t *)last + sizeof(mem_block_t) + last->size == tail)
            last->size += sizeof(mem_block_t) + added;
        else
            last->next = blk;

        kprintf("[KLIB] kernel heap now %zu KB (tail of %zu KB published)\n",
                memory_pool_size / 1024, added / 1024);
    }
}

void mem_init(void)
{
    /* Initialise heap_lock + the console lock in klib_print.c. BSS-zero
     * happens to mean "unlocked" for the current spinlock_t layout, but
     * a deliberate spinlock_init keeps the invariant explicit. */
    spinlock_init(&heap_lock);
    klib_print_lock_init();

    size_t total_pages = pmm_total_pages();
    size_t total_ram   = total_pages * VMM_PAGE_SIZE;

    /* Heap sizing: KLIB_HEAP_RAM_PERCENT of total RAM, clamped to
     * [KLIB_HEAP_MIN_SIZE, dynamic_max]. Bounds defined in klib.h. */
    size_t heap_size = (total_ram * KLIB_HEAP_RAM_PERCENT) / 100;

    if (heap_size < KLIB_HEAP_MIN_SIZE)
        heap_size = KLIB_HEAP_MIN_SIZE;

    size_t dynamic_max = total_ram / KLIB_HEAP_RAM_CAP_DIVISOR;
    if (dynamic_max < KLIB_HEAP_MAX_SIZE)
        dynamic_max = KLIB_HEAP_MAX_SIZE;

    if (heap_size > dynamic_max)
        heap_size = dynamic_max;

    heap_size = ALIGN_UP(heap_size, VMM_PAGE_SIZE);

    /* Take the whole pool NOW, as ONE contiguous block, and publish only the
     * part the bootloader's identity window can reach. The tail joins the free
     * list in mem_activate_pull_map. The pool used to be clamped to the window
     * outright, with a message promising it "will be N MB after VMM init" —
     * nothing ever raised it, so a 2 MiB heap was all any machine ever got.
     *
     * pmm_alloc, not pmm_alloc_zero: zeroing the whole pool would touch bytes
     * past the identity window. Each half is zeroed when it is published.
     *
     * The pool is one block, so clamp to the largest block the PMM can serve
     * BEFORE asking. Learning that limit from a refusal works, but the refusal
     * logs PMM_FAIL, and a scary line in every boot log is exactly the noise
     * that sends the next investigation down the wrong path.
     *
     * Halving still covers what remains: the pre-Pull-Map window may not hold
     * the ideal size. A machine that cannot spare it must still boot with a
     * smaller heap, and the size it really got is printed rather than assumed. */
    size_t max_bytes = pmm_max_alloc_pages() * VMM_PAGE_SIZE;
    if (heap_size > max_bytes)
        heap_size = max_bytes;

    void  *pool = NULL;
    for (;;)
    {
        pool = pmm_alloc(heap_size / VMM_PAGE_SIZE);
        if (pool)
            break;
        if (heap_size <= KLIB_HEAP_MIN_SIZE)
            panic("Failed to allocate kernel memory pool from PMM!");
        heap_size /= 2;
        if (heap_size < KLIB_HEAP_MIN_SIZE)
            heap_size = KLIB_HEAP_MIN_SIZE;
    }

    size_t early = heap_size;
    if (early > KLIB_HEAP_BOOTLOADER_SAFE_SIZE)
        early = KLIB_HEAP_BOOTLOADER_SAFE_SIZE;

    memory_pool      = (uint8_t *)pool;
    memory_pool_full = heap_size;
    memory_pool_size = early;
    memset(memory_pool, 0, early);

    free_list = (mem_block_t *)memory_pool;
    free_list->size = early - sizeof(mem_block_t);
    free_list->next = NULL;
    free_list->magic = KLIB_MAGIC_FREE;

    kprintf("[KLIB] Total RAM: %zu MB, kernel heap: %zu KB (%zu KB live until Pull Map)\n",
            total_ram / (1024 * 1024), heap_size / 1024, early / 1024);

    slab_init();
}

/* Published pool size. Callers that size a growable structure against the
 * heap need the real number, not a constant that outlives the sizing policy. */
size_t mem_heap_size(void)
{
    return memory_pool_full ? memory_pool_full : memory_pool_size;
}

static void *kmalloc_internal(size_t size)
{
    if (size == 0)
        return NULL;

    size = (size + KLIB_BLOCK_ALIGNMENT - 1) & ~(KLIB_BLOCK_ALIGNMENT - 1);

    spin_lock(&heap_lock);

    mem_block_t *curr = free_list, *prev = NULL;
    void *result = NULL;

    while (curr)
    {
        if (curr->magic != KLIB_MAGIC_FREE)
            panic("Memory corruption detected in kmalloc (magic=0x%x, expected FREE)!",
                  curr->magic);

        if ((uintptr_t)curr < (uintptr_t)memory_pool ||
            (uintptr_t)curr >= (uintptr_t)memory_pool + memory_pool_size)
            panic("kmalloc corruption: free_list entry 0x%p out of heap range [0x%p - 0x%p]",
                  curr, memory_pool, (void *)((uintptr_t)memory_pool + memory_pool_size));

        if (curr->size >= size)
        {
            if (curr->size > size + sizeof(mem_block_t) + KLIB_BLOCK_ALIGNMENT)
            {
                mem_block_t *new_block = (mem_block_t *)((char *)curr + sizeof(mem_block_t) + size);
                new_block->size  = curr->size - size - sizeof(mem_block_t);
                new_block->next  = curr->next;
                new_block->magic = KLIB_MAGIC_FREE;
                curr->size = size;
                curr->next = new_block;
            }

            if (prev) prev->next = curr->next;
            else      free_list  = curr->next;

            curr->magic = KLIB_MAGIC_NUMBER;
            result = (void *)((char *)curr + sizeof(mem_block_t));

            if ((uintptr_t)result < (uintptr_t)memory_pool ||
                (uintptr_t)result >= (uintptr_t)memory_pool + memory_pool_size)
                panic("kmalloc corruption: returned pointer 0x%p out of heap range [0x%p - 0x%p]",
                      result, memory_pool, (void *)((uintptr_t)memory_pool + memory_pool_size));

            break;
        }

        prev = curr;

        if (curr->next != NULL)
        {
            if ((uintptr_t)curr->next < (uintptr_t)memory_pool ||
                (uintptr_t)curr->next >= (uintptr_t)memory_pool + memory_pool_size)
                panic("kmalloc corruption: free_list->next 0x%p out of heap range [0x%p - 0x%p]",
                      curr->next, memory_pool, (void *)((uintptr_t)memory_pool + memory_pool_size));
        }

        curr = curr->next;
    }

    size_t live = 0, peak = 0, largest_free = 0, max_1 = 0;
    unsigned step = 0;

    if (result)
    {
        /* Account the BLOCK, not the request: when the tail remainder is too
         * small to split off, curr->size stays larger than `size` and the
         * block carries that slack until it is freed. kfree_internal gives
         * back curr->size, so charging `size` here would drift the counter
         * down on every non-splitting allocation and understate the peak. */
        g_heap_live += curr->size + sizeof(mem_block_t);
        if (size > g_heap_max_1)
            g_heap_max_1 = size;
        if (g_heap_live > g_heap_peak)
        {
            g_heap_peak = g_heap_live;
            size_t next = g_heap_peak_reported ? g_heap_peak_reported * 2
                                               : HEAP_PEAK_REPORT_FLOOR;
            if (g_heap_peak >= next)
            {
                g_heap_peak_reported = next;
                step  = 1;
                peak  = g_heap_peak;
                max_1 = g_heap_max_1;
            }
        }
    }
    else
    {
        live = g_heap_live;
        peak = g_heap_peak;
        for (mem_block_t *b = free_list; b; b = b->next)
            if (b->size > largest_free)
                largest_free = b->size;
    }

    spin_unlock(&heap_lock);

    /* kprintf, not debug_printf: a NULL from here is how every caller's
     * error-unwind path begins, and in a release build debug_printf is
     * compiled out — heap exhaustion used to leave no trace at all, so the
     * unwind that followed looked like a spontaneous failure.  Printing
     * happens after the unlock: kprintf takes the console lock, and heap
     * before console is not an ordering this kernel takes anywhere else. */
    if (!result)
        kprintf("[KLIB] ERROR: kmalloc failed for %zu bytes — pool %zu KB, live %zu KB, "
                "peak %zu KB, largest free %zu B\n",
                size, memory_pool_size / 1024, live / 1024, peak / 1024, largest_free);
    else if (step)
        kprintf("[KLIB] heap high-water %zu KB of %zu KB, largest request %zu B\n",
                peak / 1024, memory_pool_size / 1024, max_1);

    return result;
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    /* Slab path: O(1) for small allocations. */
    if (size <= SLAB_LARGE_THRESHOLD)
    {
        void *ptr = slab_alloc(size);
        if (ptr)
            return ptr;
        /* Fall through if slab exhausted — legacy pool handles it. */
    }

#if HEAP_GUARD_ENABLED
    size_t total_size = sizeof(heap_guard_t) + size + sizeof(uint64_t);
    heap_guard_t *guard = (heap_guard_t *)kmalloc_internal(total_size);

    if (!guard)
        return NULL;

    guard->canary_start      = HEAP_CANARY_MAGIC;
    guard->size              = size;
    guard->caller            = "kmalloc";
    guard->canary_end_offset = sizeof(heap_guard_t) + size;

    uint64_t *end_canary = (uint64_t *)((uint8_t *)guard + guard->canary_end_offset);
    *end_canary = HEAP_CANARY_MAGIC;

    return (void *)((uint8_t *)guard + sizeof(heap_guard_t));
#else
    return kmalloc_internal(size);
#endif
}

static void kfree_internal(void *ptr)
{
    if (!ptr)
        return;

    mem_block_t *block = (mem_block_t *)((char *)ptr - sizeof(mem_block_t));

    if ((uintptr_t)block < (uintptr_t)memory_pool ||
        (uintptr_t)block >= (uintptr_t)memory_pool + memory_pool_size)
        panic("Invalid free: pointer out of range!");

    /* O(1) double-free detection via magic tombstone — see klib.h. */
    if (block->magic == KLIB_MAGIC_FREE)
        panic("Double free detected (magic=FREE)!");
    if (block->magic != KLIB_MAGIC_NUMBER)
        panic("Invalid free: bad magic number!");
    block->magic = KLIB_MAGIC_FREE;

    spin_lock(&heap_lock);

    if (g_heap_live >= block->size + sizeof(mem_block_t))
        g_heap_live -= block->size + sizeof(mem_block_t);

    mem_block_t *curr = free_list, *prev = NULL;
    while (curr && curr < block)
    {
        prev = curr;
        curr = curr->next;
    }

    if (prev && (char *)prev + sizeof(mem_block_t) + prev->size == (char *)block)
    {
        prev->size += sizeof(mem_block_t) + block->size;
        block = prev;
    }
    else
    {
        block->next = curr;
        if (prev) prev->next = block;
        else      free_list  = block;
    }

    if (block->next && (char *)block + sizeof(mem_block_t) + block->size == (char *)block->next)
    {
        block->size += sizeof(mem_block_t) + block->next->size;
        block->next = block->next->next;
    }

    spin_unlock(&heap_lock);
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

#if HEAP_GUARD_ENABLED
    heap_guard_t *guard = (heap_guard_t *)((uint8_t *)ptr - sizeof(heap_guard_t));

    if (guard->canary_start != HEAP_CANARY_MAGIC)
    {
        debug_printf("[HEAP] CORRUPTION: Start canary destroyed at %p\n", ptr);
        debug_printf("[HEAP]   Expected: 0x%llx, Got: 0x%llx\n",
                     HEAP_CANARY_MAGIC, guard->canary_start);
        while (1) asm volatile("cli; hlt");
    }

    uint64_t *end_canary = (uint64_t *)((uint8_t *)guard + guard->canary_end_offset);
    if (*end_canary != HEAP_CANARY_MAGIC)
    {
        debug_printf("[HEAP] CORRUPTION: End canary destroyed at %p (size=%zu)\n",
                     ptr, guard->size);
        debug_printf("[HEAP]   Expected: 0x%llx, Got: 0x%llx\n",
                     HEAP_CANARY_MAGIC, *end_canary);
        debug_printf("[HEAP]   This indicates buffer overflow!\n");
        while (1) asm volatile("cli; hlt");
    }

    kfree_internal(guard);
#else
    kfree_internal(ptr);
#endif
}

void mem_stats(void)
{
    spin_lock(&heap_lock);

    size_t free_blocks = 0;
    size_t free_memory = 0;
    size_t largest_block = 0;

    mem_block_t *curr = free_list;
    while (curr)
    {
        free_blocks++;
        free_memory += curr->size;
        if (curr->size > largest_block)
            largest_block = curr->size;
        curr = curr->next;
    }

    size_t used_memory = memory_pool_size - free_memory - free_blocks * sizeof(mem_block_t);

    kprintf("Memory Statistics:\n");
    kprintf("  Total memory: %zu bytes\n", memory_pool_size);
    kprintf("  Used memory:  %zu bytes\n", used_memory);
    kprintf("  Free memory:  %zu bytes\n", free_memory);
    kprintf("  Free blocks:  %zu\n", free_blocks);
    kprintf("  Largest free: %zu bytes\n", largest_block);

    spin_unlock(&heap_lock);
}
