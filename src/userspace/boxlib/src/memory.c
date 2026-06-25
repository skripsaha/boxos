// Prevent variadic malloc macro from expanding internal calls
#ifdef malloc
#undef malloc
#endif

#include "box/memory.h"
#include "box/sync.h"
#include "box/string.h"
#include "box/core/notify.h"
#include "box/core/cabin.h"
#include "box/core/manifest.h"
#include "box/core/strand_self.h"  /* strand_info_or_null / strand_self — pool selector */
#include "box/print.h"
#include "cabin_layout.h"
#include "strand_pool_abi.h"       /* StrandPool layout (shared with kernel death-stamp) */

/* Implicit 2 MiB heap pages.
 *
 * When a single allocation crosses the 2 MiB threshold we pad the heap
 * up to the next 2 MiB boundary, ask the kernel to pre-back the region
 * with 2 MiB physical pages (one PDE per chunk), and place the
 * allocation at the aligned address. Sub-2 MiB allocations stay on the
 * existing 4 KiB demand-paging path.
 *
 * The pre-fault syscall mirrors src/kernel/core/decks/system/system_deck.h
 * SYSTEM_OP_HEAP_PREFAULT and the handler in bay_ops.c. */
#include "boxos_sizes.h"
#include "boxos_decks.h"

#define HEAP_OP_PREFAULT         0x14u
#define HEAP_HUGE_THRESHOLD      LARGE_PAGE_2M_SIZE
#define HEAP_HUGE_MASK           LARGE_PAGE_2M_MASK

// ---------------------------------------------------------------------------
// Thread-safe first-fit heap allocator with tag support and diagnostics
// ---------------------------------------------------------------------------

#define HEAP_MAGIC      0xA110CA7EU
#define HEAP_ALIGN      16
#define HEAP_MIN_SPLIT  (sizeof(block_t) + HEAP_ALIGN)

typedef struct block {
    size_t        size;       // payload size (excluding header)
    uint32_t      magic;
    uint32_t      free;       // 1 = free, 0 = used
    struct block* next;
    uint8_t       tag;        // HEAP_TAG_NONE (0xFF) = untagged
    uint8_t       _pad[7];   // keep struct at 32 bytes
} block_t;

// Verify struct layout at compile time: 8+4+4+8+1+7 = 32 bytes
typedef char _block_size_check[sizeof(block_t) == 32 ? 1 : -1];

#define BLOCK_HDR_SIZE  ((sizeof(block_t) + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1))

// Verify BLOCK_HDR_SIZE is still 32 (no heap layout change)
typedef char _hdr_size_check[BLOCK_HDR_SIZE == 32 ? 1 : -1];

// ---- tag registry (all access under heap_lock) ----------------------------

static char    heap_tag_names[HEAP_TAG_CAP][HEAP_TAG_NAME_MAX];
static uint8_t heap_tag_count = 0;

// ---- internal state (all access under heap_lock) --------------------------

static umutex_t  heap_lock    = UMUTEX_INIT;
static block_t*  free_list    = NULL;
static uintptr_t heap_base    = 0;
static uintptr_t heap_current = 0;
static uintptr_t heap_max     = 0;
static int       initialized  = 0;

// ---- diagnostics ----------------------------------------------------------

/* The main strand's / shared-fallback heap error cell. Internal linkage: the
 * public API is heap_get_last_error() (per-strand), not this symbol. */
static error_t  heap_last_error = OK;

static uint32_t stat_malloc_calls = 0;
static uint32_t stat_free_calls   = 0;

// ---- helpers (called with lock held) --------------------------------------

static void heap_init_locked(void) {
    CabinInfo* ci = cabin_info();
    if (ci->heap_base != 0 && ci->heap_max_size != 0) {
        heap_base = ci->heap_base;
        heap_max  = heap_base + ci->heap_max_size;
    } else {
        heap_base = CABIN_HEAP_BASE;
        heap_max  = heap_base + CABIN_HEAP_MAX_SIZE;
    }
    heap_current = heap_base;
    free_list    = NULL;
    initialized  = 1;
}

static void* sbrk_locked(size_t increment) {
    if (!initialized) return NULL;

    uintptr_t new_end = heap_current + increment;
    if (new_end > heap_max) return NULL;

    uintptr_t old = heap_current;
    heap_current  = new_end;
    return (void*)old;
}

static size_t align_up(size_t val, size_t align) {
    return (val + align - 1) & ~(align - 1);
}

static void coalesce_locked(void) {
    block_t* curr = free_list;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) break;
        if (curr->free && curr->next &&
            curr->next->magic == HEAP_MAGIC && curr->next->free) {
            curr->size += BLOCK_HDR_SIZE + curr->next->size;
            curr->next  = curr->next->next;
            continue;
        }
        curr = curr->next;
    }
}

// Look up or insert a tag name. Returns HEAP_TAG_NONE if registry full.
// Must be called with heap_lock held.
static uint8_t get_or_create_tag_locked(const char *name) {
    if (!name) return HEAP_TAG_NONE;

    for (uint8_t i = 0; i < heap_tag_count; i++) {
        if (strncmp(heap_tag_names[i], name, HEAP_TAG_NAME_MAX - 1) == 0)
            return i;
    }

    if (heap_tag_count >= HEAP_TAG_CAP) return HEAP_TAG_NONE;

    uint8_t id = heap_tag_count++;
    strncpy(heap_tag_names[id], name, HEAP_TAG_NAME_MAX - 1);
    heap_tag_names[id][HEAP_TAG_NAME_MAX - 1] = '\0';
    return id;
}

/* Pre-fault a 2 MB-aligned, 2 MB-sized region. The kernel maps every
 * 2 MB chunk inside [va_base, va_base+size_2m) with one PDE leaf,
 * falling back to 4 KB pages transparently if PMM fragmentation
 * prevents a chunk-level allocation. Returns 0 on success. */
static int prefault_huge_locked(uintptr_t va_base, uint64_t size_2m_aligned) {
    uint8_t params[16];
    uint64_t va64 = (uint64_t)va_base;
    memcpy(params,     &va64,           sizeof(uint64_t));
    memcpy(params + 8, &size_2m_aligned, sizeof(uint64_t));
    return MfCall1(DECK_SYSTEM, HEAP_OP_PREFAULT,
                   params, sizeof(params),
                   NULL, 0, NULL, 0, NULL,
                   30000, NULL);
}

// Core allocation logic. tag_id must already be resolved.
// Called with heap_lock held. Returns payload pointer or NULL.
static void* alloc_locked(size_t size, uint8_t tag_id, error_t *errcell) {
    if (!initialized) heap_init_locked();

    stat_malloc_calls++;

    size_t orig_size = size;
    size = align_up(size, HEAP_ALIGN);
    if (size < orig_size) {
        *errcell = ERR_NO_MEMORY;
        return NULL;
    }

    // First-fit search
    block_t* curr = free_list;
    block_t* prev = NULL;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) {
            *errcell = ERR_CORRUPTED;
            break;
        }
        if (curr->free && curr->size >= size) {
            if (curr->size >= size + HEAP_MIN_SPLIT) {
                block_t* split = (block_t*)((uint8_t*)curr + BLOCK_HDR_SIZE + size);
                split->size  = curr->size - size - BLOCK_HDR_SIZE;
                split->magic = HEAP_MAGIC;
                split->free  = 1;
                split->tag   = HEAP_TAG_NONE;
                split->next  = curr->next;
                curr->size   = size;
                curr->next   = split;
            }
            curr->free = 0;
            curr->tag  = tag_id;
            *errcell = OK;
            return (void*)((uint8_t*)curr + BLOCK_HDR_SIZE);
        }
        prev = curr;
        curr = curr->next;
    }

    // Grow the heap
    size_t total = BLOCK_HDR_SIZE + size;
    if (total < size) {
        *errcell = ERR_NO_MEMORY;
        return NULL;
    }

    /* Implicit 2 MB-page growth — when this single allocation needs at
     * least 2 MB of backing, ask the kernel to pre-back with 2 MB pages
     * rather than spending hundreds of demand-fault syscalls. The block
     * starts at a 2 MB boundary so the whole header+payload sits inside
     * one PDE leaf (or spans multiple cleanly-aligned PDEs). Any gap
     * between heap_current and the aligned base becomes a free padding
     * block recycled by future small allocations. */
    if (total >= HEAP_HUGE_THRESHOLD) {
        uintptr_t cur      = heap_current;
        uintptr_t aligned  = (cur + HEAP_HUGE_MASK) & ~HEAP_HUGE_MASK;
        size_t    pad_size = aligned - cur;
        size_t    huge_total   = (total + HEAP_HUGE_MASK) & ~HEAP_HUGE_MASK;
        size_t    grow_total   = pad_size + huge_total;

        if (heap_current + grow_total > heap_max) {
            *errcell = ERR_HEAP_EXHAUSTED;
            return NULL;
        }

        /* Insert padding free block (skip if the gap is smaller than a
         * usable header+payload — then we simply waste those bytes of
         * VA; physical RAM was never allocated for them). */
        if (pad_size >= BLOCK_HDR_SIZE + HEAP_ALIGN) {
            block_t *pad = (block_t *)cur;
            pad->size  = pad_size - BLOCK_HDR_SIZE;
            pad->magic = HEAP_MAGIC;
            pad->free  = 1;
            pad->tag   = HEAP_TAG_NONE;
            pad->next  = NULL;
            if (prev) {
                prev->next = pad;
            } else {
                free_list = pad;
            }
            prev = pad;
        }

        if (prefault_huge_locked(aligned, (uint64_t)huge_total) != 0) {
            /* Kernel could not back the region (PMM exhausted). Don't
             * roll back the padding block — it's a legitimate free
             * region that future small allocations can use. */
            *errcell = ERR_NO_MEMORY;
            return NULL;
        }

        heap_current = aligned + huge_total;

        block_t *block = (block_t *)aligned;
        block->size  = huge_total - BLOCK_HDR_SIZE;
        block->magic = HEAP_MAGIC;
        block->free  = 0;
        block->tag   = tag_id;
        block->next  = NULL;

        if (prev) {
            prev->next = block;
        } else {
            free_list = block;
        }

        *errcell = OK;
        return (void *)((uint8_t *)block + BLOCK_HDR_SIZE);
    }

    /* Sub-2 MB growth — existing 4 KB demand-paged path. */
    void* mem = sbrk_locked(total);
    if (!mem) {
        *errcell = ERR_HEAP_EXHAUSTED;
        return NULL;
    }

    block_t* block = (block_t*)mem;
    block->size  = size;
    block->magic = HEAP_MAGIC;
    block->free  = 0;
    block->tag   = tag_id;
    block->next  = NULL;

    if (prev) {
        prev->next = block;
    } else {
        free_list = block;
    }

    *errcell = OK;
    return (void*)((uint8_t*)block + BLOCK_HDR_SIZE);
}

// ---------------------------------------------------------------------------
// StrandPool — per-strand magazine cache over the one global-locked heap
//
// Each strand owns one StrandPool (a private LIFO of free blocks per size
// class). The fast path pops/pushes its own pool with NO lock — the same
// single-writer-per-strand model as the per-strand result stash. A cached block
// stays free=0 (the global heap still sees it LIVE); the magazine link is stored
// in the block's payload first 8 bytes. The ONE global heap remains the source
// of truth: refill calls alloc_locked N×, flush sets free=1 + one coalesce.
// ---------------------------------------------------------------------------

static const size_t   StrandPoolClassSize[STRAND_POOL_CLASS_COUNT] =
    { 16, 32, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
static const uint16_t StrandPoolCapForClass[STRAND_POOL_CLASS_COUNT] =
    {  8,  8,  8,  8,   8,   8,   4,    4,    2,    2,    1 };

#define STRAND_POOL_REFILL_BATCH  4u
#define STRAND_POOL_NO_CLASS      STRAND_POOL_CLASS_COUNT  /* "bypass the cache" */

/* Cabin-persistent pool storage. The slab survives a strand's StrandInfo unmap
 * on crash, so an orphaned slot's cached blocks are still reachable for reclaim.
 * The slab IS the registry (linear scan, slow-path only — no linked list). The
 * main strand uses a separate pool that can never be crash-orphaned. All slab
 * fields are zero (FREE) at BSS init. */
static StrandPool g_pool_slab[STRAND_POOL_SLAB_MAX];
static StrandPool g_main_pool;

/* Per-strand heap last-error cells (Ф23c). The boxlib heap is shared by every
 * strand in the cabin, but "the cause of MY last heap op" must NOT be: under
 * spawned strands a single global cell is a race — one strand's success would
 * clobber another strand's failure before it is read. Each strand owns the cell
 * parallel to its StrandPool slab slot (same index → single-writer, no lock, no
 * race); the main strand and any strand that could not claim a slot share
 * heap_last_error. This stays entirely in userspace and never touches the
 * kernel-shared StrandPool ABI (strand_pool_abi.h pins it to GenState only). */
static error_t g_pool_last_error[STRAND_POOL_SLAB_MAX];

/* The error cell owned by the strand whose pool is `pool`: main/NULL/uncached
 * → the process-global cell; a claimed slab slot → its parallel per-strand
 * cell. `pool - g_pool_slab` is the slot index (pool always points into the
 * slab once the &g_main_pool case is excluded). */
static inline error_t *heap_err_cell_for(StrandPool *pool) {
    if (pool == NULL || pool == &g_main_pool) return &heap_last_error;
    return &g_pool_last_error[pool - g_pool_slab];
}

/* The calling strand's last heap error (Ф23c). Read-only: it never claims a
 * pool slot, so a pure query has no allocation side effect. A spawned strand
 * that has touched the cached heap reads its own per-strand cell. NOTE the
 * value is meaningful only AFTER this strand's first heap op: before that a
 * spawned strand has no cell of its own and reads the bootstrap/main cell, and
 * a strand that could not claim a slot (slab full) shares the main cell for as
 * long as it runs — the same bounded degradation under which it also allocates
 * uncached through the locked global path. */
error_t heap_get_last_error(void) {
    StrandInfo *si = strand_info_or_null();
    if (si && si->strand_pool_ptr != 0)
        return *heap_err_cell_for((StrandPool *)(uintptr_t)si->strand_pool_ptr);
    return heap_last_error;
}

/* Dirty flag: the kernel sets this to 1 (RELEASE) after a successful
 * LIVE→ORPHANED CAS in process_destroy. 0/1 only — NOT a counter, so
 * it can never underflow. CLEAR-BEFORE-SCAN: we zero it before walking
 * so a concurrent kernel set during the scan leaves the flag at 1 and
 * the NEXT slow-path call catches the new orphan. The slot stays
 * ORPHANED in the slab, so no orphan is ever lost. */
static volatile uint32_t g_strandpool_orphan_pending = 0;

/* High-water mark: highest claimed slot index + 1. Updated under heap_lock
 * in pool_claim. Bounds the reclaim scan to slots that were ever used. */
static uint32_t g_pool_slab_hwm = 0;

/* Floor map: smallest class whose size >= n (so a served block is always at
 * least as large as requested). n > 8192 → bypass to the locked global path. */
static unsigned StrandPoolSizeToClass(size_t n) {
    for (unsigned c = 0; c < STRAND_POOL_CLASS_COUNT; c++) {
        if (StrandPoolClassSize[c] >= n) return c;
    }
    return STRAND_POOL_NO_CLASS;
}

/* Largest class whose size <= a freed block's actual size (s > 8192 → bypass).
 * Pairs with the floor alloc map: a block grown for class c has size >=
 * ClassSize[c], so it maps back to c or higher — never below its served class. */
static unsigned StrandPoolClassFromBlockSize(size_t s) {
    if (s > STRAND_POOL_MAX_CLASS) return STRAND_POOL_NO_CLASS;
    unsigned cls = STRAND_POOL_NO_CLASS;
    for (unsigned c = 0; c < STRAND_POOL_CLASS_COUNT; c++) {
        if (StrandPoolClassSize[c] <= s) cls = c; else break;
    }
    return cls;
}

/* Tell the kernel which slot this strand bound, so process_destroy can stamp it
 * ORPHANED if the strand crashes without flushing. Called AFTER the slot is
 * written (page present → resolvable) and OUTSIDE heap_lock (it is an IPC call).
 * Main strand never binds (its pool can't be crash-orphaned). */
static void strand_pool_bind_kernel(StrandPool *pool, uint32_t gen) {
    uint8_t params[20];
    uint64_t va         = (uint64_t)(uintptr_t)pool;
    uint64_t pending_va = (uint64_t)(uintptr_t)&g_strandpool_orphan_pending;
    memcpy(params,      &va,         sizeof(uint64_t));
    memcpy(params + 8,  &gen,        sizeof(uint32_t));
    memcpy(params + 12, &pending_va, sizeof(uint64_t));
    (void)MfCall1(DECK_SYSTEM, SYSTEM_OP_STRAND_POOL_BIND,
                  params, sizeof(params),
                  NULL, 0, NULL, 0, NULL,
                  30000, NULL);
}

/* Reclaim every ORPHANED slab slot: flush its cached blocks back to the global
 * heap (free=1), coalesce once, then mark the slot FREE at the next generation.
 * Called only on the slow path, with heap_lock HELD. Bounded by the slab size. */
static void reclaim_one_orphan_slot_locked(StrandPool *p, uint32_t word) {
    for (unsigned c = 0; c < STRAND_POOL_CLASS_COUNT; c++) {
        unsigned guard = p->Counts[c];
        void *node = p->Heads[c];
        while (node && guard--) {
            block_t *block = (block_t *)((uint8_t *)node - BLOCK_HDR_SIZE);
            if (block->magic != HEAP_MAGIC) break;
            void *next = *(void **)node;
            if (!block->free) {
                block->free = 1;
                block->tag  = HEAP_TAG_NONE;
            }
            node = next;
        }
        p->Heads[c]  = NULL;
        p->Counts[c] = 0;
    }
    coalesce_locked();
    p->OwnerPid = 0;
    __atomic_store_n(&p->GenState,
                     STRANDPOOL_PACK(STRANDPOOL_GEN(word) + 1, STRANDPOOL_FREE),
                     __ATOMIC_RELEASE);
}

/* Event-driven orphan sweep: the kernel sets g_strandpool_orphan_pending only on
 * a real crash-stamp, so the common (no-crash) case skips the scan entirely. The
 * scan is bounded by the high-water mark, not the full slab. heap_lock HELD. */
static void reclaim_orphans_scan_locked(void) {
    if (__atomic_load_n(&g_strandpool_orphan_pending, __ATOMIC_ACQUIRE) == 0) return;
    __atomic_store_n(&g_strandpool_orphan_pending, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < g_pool_slab_hwm; i++) {
        StrandPool *p = &g_pool_slab[i];
        uint32_t word = __atomic_load_n(&p->GenState, __ATOMIC_ACQUIRE);
        if (STRANDPOOL_STATE(word) != STRANDPOOL_ORPHANED) continue;
        reclaim_one_orphan_slot_locked(p, word);
    }
}

/* Claim a slab slot for the calling strand. Spawned strands take a g_pool_slab
 * slot and bind it to the kernel; the main strand takes g_main_pool (no bind).
 * Returns NULL when the slab is full even after reclaiming orphans — the strand
 * then runs uncached through the locked global path (correctness preserved). */
static StrandPool *pool_claim(StrandInfo *si) {
    if (!si) {
        /* Main strand: its pool is process-lifetime, never crash-orphaned. This
         * branch is reached ONLY by the main strand (the sole si==NULL context),
         * so it is single-writer — no CAS is needed to claim g_main_pool. */
        uint32_t word = __atomic_load_n(&g_main_pool.GenState, __ATOMIC_ACQUIRE);
        if (STRANDPOOL_STATE(word) == STRANDPOOL_FREE) {
            g_main_pool.OwnerPid = strand_self();
            __atomic_store_n(&g_main_pool.GenState,
                             STRANDPOOL_PACK(STRANDPOOL_GEN(word) + 1, STRANDPOOL_LIVE),
                             __ATOMIC_RELEASE);
        }
        return &g_main_pool;
    }

    StrandPool *claimed = NULL;
    uint32_t    claimed_gen = 0;

    umutex_lock(&heap_lock);
    for (int pass = 0; pass < 2 && !claimed; pass++) {
        for (unsigned i = 0; i < STRAND_POOL_SLAB_MAX; i++) {
            StrandPool *p = &g_pool_slab[i];
            uint32_t word = __atomic_load_n(&p->GenState, __ATOMIC_RELAXED);
            if (STRANDPOOL_STATE(word) != STRANDPOOL_FREE) continue;
            uint32_t next = STRANDPOOL_PACK(STRANDPOOL_GEN(word) + 1, STRANDPOOL_LIVE);
            if (__atomic_compare_exchange_n(&p->GenState, &word, next, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                p->OwnerPid = strand_self();
                g_pool_last_error[i] = OK;  /* fresh slot starts with no error */
                claimed     = p;
                claimed_gen = STRANDPOOL_GEN(next);
                if (i + 1 > g_pool_slab_hwm) g_pool_slab_hwm = i + 1;
                break;
            }
        }
        if (!claimed && pass == 0) {
            /* Slab full: force an unconditional walk [0, hwm) regardless of
             * the dirty flag — robustness net for the vanishingly-unlikely
             * case where a kernel flag-set was missed. */
            for (unsigned j = 0; j < g_pool_slab_hwm; j++) {
                StrandPool *p2 = &g_pool_slab[j];
                uint32_t word2 = __atomic_load_n(&p2->GenState, __ATOMIC_ACQUIRE);
                if (STRANDPOOL_STATE(word2) != STRANDPOOL_ORPHANED) continue;
                reclaim_one_orphan_slot_locked(p2, word2);
            }
            __atomic_store_n(&g_strandpool_orphan_pending, 0, __ATOMIC_RELEASE);
        }
    }
    umutex_unlock(&heap_lock);

    if (claimed) {
        /* Cache the pointer for the lock-free fast path, THEN bind to the kernel
         * (the write guarantees the page is present so the kernel walk resolves). */
        si->strand_pool_ptr = (uint64_t)(uintptr_t)claimed;
        strand_pool_bind_kernel(claimed, claimed_gen);
    }
    return claimed;
}

/* The calling strand's pool, claiming one lazily on first use. Returns NULL only
 * when a spawned strand cannot get a slot — its caller then uses the locked path.
 * Single-writer per strand → no lock on this read. */
static StrandPool *pool_self(void) {
    StrandInfo *si = strand_info_or_null();
    if (!si) {
        if (__atomic_load_n(&g_main_pool.GenState, __ATOMIC_ACQUIRE) == 0)
            return pool_claim(NULL);
        return &g_main_pool;
    }
    if (si->strand_pool_ptr != 0)
        return (StrandPool *)(uintptr_t)si->strand_pool_ptr;
    return pool_claim(si);
}

/* Refill one class: pull STRAND_POOL_REFILL_BATCH blocks of ClassSize[c] from
 * the global heap and push them onto the magazine. heap_lock HELD. */
static void pool_refill_locked(StrandPool *pool, unsigned c) {
    error_t *errcell = heap_err_cell_for(pool);
    for (unsigned k = 0; k < STRAND_POOL_REFILL_BATCH; k++) {
        void *payload = alloc_locked(StrandPoolClassSize[c], HEAP_TAG_NONE, errcell);
        if (!payload) break;   /* heap exhausted — serve whatever we got */
        *(void **)payload = pool->Heads[c];
        pool->Heads[c]    = payload;
        pool->Counts[c]++;
    }
}

/* Flush up to `drop` blocks of class c back to the global heap (free=1). One
 * coalesce afterward is the caller's job. heap_lock HELD. The walk is hard-bounded
 * by Counts[c] and validates each block's magic BEFORE following its payload link,
 * so a corrupted link can never fault under the lock or loop. */
static void pool_flush_class_locked(StrandPool *pool, unsigned c, unsigned drop) {
    unsigned guard = pool->Counts[c];
    while (drop-- && guard-- && pool->Heads[c]) {
        void *payload = pool->Heads[c];
        block_t *block = (block_t *)((uint8_t *)payload - BLOCK_HDR_SIZE);
        if (block->magic != HEAP_MAGIC) break;
        pool->Heads[c] = *(void **)payload;
        pool->Counts[c]--;
        if (!block->free) {
            block->free = 1;
            block->tag  = HEAP_TAG_NONE;
        }
    }
}

/* Orderly flush of the calling strand's whole pool at strand/cabin exit. Drains
 * every magazine to the global heap, coalesces once, then bumps the slot's
 * generation to FREE — the bump makes the kernel death-stamp CAS miss, so no
 * unbind syscall is needed. Idempotent: a strand with no pool is a no-op. */
void strand_pool_flush_self(void) {
    StrandInfo *si   = strand_info_or_null();
    StrandPool *pool = si ? (StrandPool *)(uintptr_t)si->strand_pool_ptr : &g_main_pool;
    if (!pool) return;
    if (si && si->strand_pool_ptr == 0) return;
    if (!si && __atomic_load_n(&g_main_pool.GenState, __ATOMIC_ACQUIRE) == 0) return;

    umutex_lock(&heap_lock);
    for (unsigned c = 0; c < STRAND_POOL_CLASS_COUNT; c++)
        pool_flush_class_locked(pool, c, (unsigned)-1);
    coalesce_locked();
    uint32_t word = __atomic_load_n(&pool->GenState, __ATOMIC_RELAXED);
    pool->OwnerPid = 0;
    __atomic_store_n(&pool->GenState,
                     STRANDPOOL_PACK(STRANDPOOL_GEN(word) + 1, STRANDPOOL_FREE),
                     __ATOMIC_RELEASE);
    umutex_unlock(&heap_lock);

    if (si) si->strand_pool_ptr = 0;
}

/* Self-test of the crash-orphan reclaim MECHANISM (not a live fault). Stages a
 * spare slab slot exactly as a crashed strand would leave it — real heap blocks
 * cached in its magazines (free=0, magazine-linked) with the slot marked
 * ORPHANED — then runs the same reclaim_orphans_scan_locked the slow path uses.
 * Returns 1 if every staged block came back to the global heap and the slot is
 * FREE again; 0 on any inconsistency. `n_blocks` is clamped to a class cap. */
int strand_pool_test_orphan_reclaim(unsigned n_blocks) {
    const unsigned c = 3;  /* the 64-byte class */
    if (n_blocks == 0 || n_blocks > StrandPoolCapForClass[c]) return 0;

    umutex_lock(&heap_lock);

    /* Find a FREE spare slot near the top of the slab (away from live claims). */
    StrandPool *slot = NULL;
    for (int i = (int)STRAND_POOL_SLAB_MAX - 1; i >= 0; i--) {
        if (STRANDPOOL_STATE(g_pool_slab[i].GenState) == STRANDPOOL_FREE) {
            slot = &g_pool_slab[i];
            break;
        }
    }
    if (!slot) { umutex_unlock(&heap_lock); return 0; }

    uint32_t gen = STRANDPOOL_GEN(slot->GenState);

    /* Cache real blocks the way the fast path does: alloc from the global heap
     * (free=0) and link through the payload. */
    error_t staging_err = OK;
    for (unsigned k = 0; k < n_blocks; k++) {
        void *payload = alloc_locked(StrandPoolClassSize[c], HEAP_TAG_NONE, &staging_err);
        if (!payload) break;
        *(void **)payload = slot->Heads[c];
        slot->Heads[c]    = payload;
        slot->Counts[c]++;
    }
    unsigned staged = slot->Counts[c];

    /* Count live blocks before reclaim, then stamp ORPHANED and reclaim. */
    uint32_t live_before = 0;
    for (block_t *b = free_list; b && b->magic == HEAP_MAGIC; b = b->next)
        if (!b->free) live_before++;

    slot->OwnerPid = 0;
    __atomic_store_n(&slot->GenState,
                     STRANDPOOL_PACK(gen, STRANDPOOL_ORPHANED), __ATOMIC_RELEASE);

    /* Update HWM so the flag-gated scan covers this test slot. */
    unsigned slot_idx = (unsigned)(slot - g_pool_slab);
    if (slot_idx + 1 > g_pool_slab_hwm) g_pool_slab_hwm = slot_idx + 1;
    __atomic_store_n(&g_strandpool_orphan_pending, 1, __ATOMIC_RELEASE);
    reclaim_orphans_scan_locked();

    uint32_t live_after = 0;
    for (block_t *b = free_list; b && b->magic == HEAP_MAGIC; b = b->next)
        if (!b->free) live_after++;

    int ok = (STRANDPOOL_STATE(slot->GenState) == STRANDPOOL_FREE) &&
             (slot->Heads[c] == NULL) && (slot->Counts[c] == 0) &&
             (staged > 0) && (live_after + staged == live_before);

    umutex_unlock(&heap_lock);
    return ok;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void* _malloc_impl(size_t size) {
    if (size == 0) return NULL;

    /* Resolve this strand's pool (claiming one lazily) and its error cell up
     * front: the cause of this call lands in the calling strand's OWN cell (or,
     * only when the slab is full and no slot could be claimed, the shared
     * fallback — see the banner above), so slot-holding strands never clobber
     * each other. */
    StrandPool *pool    = pool_self();
    error_t    *errcell = heap_err_cell_for(pool);

    unsigned c = StrandPoolSizeToClass(size);
    if (c == STRAND_POOL_NO_CLASS || !pool) {
        /* Oversized request, or no slab slot available — serve from the locked
         * global heap. alloc_locked records the cause in this strand's cell. */
        umutex_lock(&heap_lock);
        void* ptr = alloc_locked(size, HEAP_TAG_NONE, errcell);
        umutex_unlock(&heap_lock);
        return ptr;
    }

    if (!pool->Heads[c]) {
        umutex_lock(&heap_lock);
        reclaim_orphans_scan_locked();
        pool_refill_locked(pool, c);
        umutex_unlock(&heap_lock);
        if (!pool->Heads[c]) {
            *errcell = ERR_HEAP_EXHAUSTED;
            return NULL;
        }
    }

    void *payload  = pool->Heads[c];
    pool->Heads[c] = *(void **)payload;
    pool->Counts[c]--;
    *errcell = OK;
    return payload;
}

void* malloc_tagged(size_t size, const char *tag) {
    if (size == 0) return NULL;

    /* pool_self() (which may claim a slot) is called BEFORE the heap lock —
     * pool_claim takes the lock itself, so resolving it here avoids re-entry. */
    error_t *errcell = heap_err_cell_for(pool_self());
    umutex_lock(&heap_lock);
    uint8_t tag_id = get_or_create_tag_locked(tag);
    void* ptr = alloc_locked(size, tag_id, errcell);
    umutex_unlock(&heap_lock);
    return ptr;
}

/* Return a block to the global heap under the lock — the slow path shared by
 * free()'s global cases (corrupt/double-free/tagged/oversized/overflow). */
static void free_global(void* ptr) {
    error_t *errcell = heap_err_cell_for(pool_self());
    umutex_lock(&heap_lock);

    stat_free_calls++;

    block_t* block = (block_t*)((uint8_t*)ptr - BLOCK_HDR_SIZE);

    if (block->magic != HEAP_MAGIC) {
        *errcell = ERR_CORRUPTED;
        umutex_unlock(&heap_lock);
        return;
    }

    if (block->free) {
        *errcell = ERR_INVALID_ADDRESS;
        umutex_unlock(&heap_lock);
        return;
    }

    block->free = 1;
    block->tag  = HEAP_TAG_NONE;
    coalesce_locked();

    *errcell = OK;
    umutex_unlock(&heap_lock);
}

void free(void* ptr) {
    if (!ptr) return;

    block_t* block = (block_t*)((uint8_t*)ptr - BLOCK_HDR_SIZE);

    /* A wild/corrupt pointer is caught here (magic is write-once); a genuinely
     * free block (real double-free of a flushed block) and any tagged block go
     * to the locked global path that owns those semantics. */
    if (block->magic != HEAP_MAGIC) { free_global(ptr); return; }
    if (block->free)                { free_global(ptr); return; }
    if (block->tag != HEAP_TAG_NONE){ free_global(ptr); return; }

    unsigned c = StrandPoolClassFromBlockSize(block->size);
    if (c == STRAND_POOL_NO_CLASS) { free_global(ptr); return; }

    StrandPool *pool = pool_self();
    if (!pool) { free_global(ptr); return; }
    error_t *errcell = heap_err_cell_for(pool);

    /* Double-free of a still-cached block: scan this magazine (<= cap pointers).
     * The global free() above cannot catch it because a cached block is free=0.
     * Bound the scan by Counts[c] and validate each node's magic BEFORE following
     * its payload link, so a corrupted cached link cannot fault or loop here. */
    unsigned guard = pool->Counts[c];
    for (void *node = pool->Heads[c]; node && guard--; ) {
        if (node == ptr) {
            *errcell = ERR_INVALID_ADDRESS;
            return;
        }
        block_t *b = (block_t *)((uint8_t *)node - BLOCK_HDR_SIZE);
        if (b->magic != HEAP_MAGIC) break;
        node = *(void **)node;
    }

    if (pool->Counts[c] >= StrandPoolCapForClass[c]) {
        umutex_lock(&heap_lock);
        reclaim_orphans_scan_locked();
        pool_flush_class_locked(pool, c, STRAND_POOL_REFILL_BATCH);
        coalesce_locked();
        umutex_unlock(&heap_lock);
    }

    *(void **)ptr  = pool->Heads[c];
    pool->Heads[c] = ptr;
    pool->Counts[c]++;
    *errcell = OK;
}

void* calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;

    size_t total = nmemb * size;
    if (total / nmemb != size) {
        *heap_err_cell_for(pool_self()) = ERR_NO_MEMORY;
        return NULL;
    }

    void* ptr = _malloc_impl(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return _malloc_impl(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    error_t *errcell = heap_err_cell_for(pool_self());
    umutex_lock(&heap_lock);

    block_t* block = (block_t*)((uint8_t*)ptr - BLOCK_HDR_SIZE);
    if (block->magic != HEAP_MAGIC) {
        *errcell = ERR_CORRUPTED;
        umutex_unlock(&heap_lock);
        return NULL;
    }

    size_t aligned = align_up(size, HEAP_ALIGN);

    // Shrink in-place
    if (block->size >= aligned) {
        if (block->size >= aligned + HEAP_MIN_SPLIT) {
            block_t* split = (block_t*)((uint8_t*)block + BLOCK_HDR_SIZE + aligned);
            split->size  = block->size - aligned - BLOCK_HDR_SIZE;
            split->magic = HEAP_MAGIC;
            split->free  = 1;
            split->tag   = HEAP_TAG_NONE;
            split->next  = block->next;
            block->size  = aligned;
            block->next  = split;
        }
        *errcell = OK;
        umutex_unlock(&heap_lock);
        return ptr;
    }

    // Try to absorb the next block if it's free
    if (block->next && block->next->magic == HEAP_MAGIC && block->next->free) {
        size_t combined = block->size + BLOCK_HDR_SIZE + block->next->size;
        if (combined >= aligned) {
            block->size = combined;
            block->next = block->next->next;
            if (block->size >= aligned + HEAP_MIN_SPLIT) {
                block_t* split = (block_t*)((uint8_t*)block + BLOCK_HDR_SIZE + aligned);
                split->size  = block->size - aligned - BLOCK_HDR_SIZE;
                split->magic = HEAP_MAGIC;
                split->free  = 1;
                split->tag   = HEAP_TAG_NONE;
                split->next  = block->next;
                block->size  = aligned;
                block->next  = split;
            }
            *errcell = OK;
            umutex_unlock(&heap_lock);
            return ptr;
        }
    }

    // Must relocate — save old tag and size, then drop lock
    size_t old_size = block->size;
    uint8_t old_tag = block->tag;
    umutex_unlock(&heap_lock);

    void* new_ptr = _malloc_impl(size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, old_size);
    free(ptr);

    // Restore tag on the new block
    if (old_tag != HEAP_TAG_NONE) {
        umutex_lock(&heap_lock);
        block_t* new_block = (block_t*)((uint8_t*)new_ptr - BLOCK_HDR_SIZE);
        if (new_block->magic == HEAP_MAGIC) {
            new_block->tag = old_tag;
        }
        umutex_unlock(&heap_lock);
    }

    return new_ptr;
}

// ---------------------------------------------------------------------------
// Tag registry query API
// ---------------------------------------------------------------------------

uint8_t heap_register_tag(const char *name) {
    umutex_lock(&heap_lock);
    uint8_t id = get_or_create_tag_locked(name);
    umutex_unlock(&heap_lock);
    return id;
}

uint8_t heap_lookup_tag(const char *name) {
    if (!name) return HEAP_TAG_NONE;

    umutex_lock(&heap_lock);
    uint8_t result = HEAP_TAG_NONE;
    for (uint8_t i = 0; i < heap_tag_count; i++) {
        if (strncmp(heap_tag_names[i], name, HEAP_TAG_NAME_MAX - 1) == 0) {
            result = i;
            break;
        }
    }
    umutex_unlock(&heap_lock);
    return result;
}

const char *heap_tag_name(uint8_t id) {
    if (id == HEAP_TAG_NONE || id >= HEAP_TAG_CAP) return NULL;

    umutex_lock(&heap_lock);
    const char *name = (id < heap_tag_count) ? heap_tag_names[id] : NULL;
    umutex_unlock(&heap_lock);
    return name;
}

size_t heap_count_tag(const char *tag) {
    if (!tag) return 0;

    umutex_lock(&heap_lock);

    uint8_t tag_id = HEAP_TAG_NONE;
    for (uint8_t i = 0; i < heap_tag_count; i++) {
        if (strncmp(heap_tag_names[i], tag, HEAP_TAG_NAME_MAX - 1) == 0) {
            tag_id = i;
            break;
        }
    }

    size_t count = 0;
    if (tag_id != HEAP_TAG_NONE) {
        block_t* curr = free_list;
        while (curr) {
            if (curr->magic != HEAP_MAGIC) break;
            if (!curr->free && curr->tag == tag_id) count++;
            curr = curr->next;
        }
    }

    umutex_unlock(&heap_lock);
    return count;
}

void heap_iterate_tag(const char *tag, HeapTagCallback cb, void *userdata) {
    if (!tag || !cb) return;

    umutex_lock(&heap_lock);

    uint8_t tag_id = HEAP_TAG_NONE;
    for (uint8_t i = 0; i < heap_tag_count; i++) {
        if (strncmp(heap_tag_names[i], tag, HEAP_TAG_NAME_MAX - 1) == 0) {
            tag_id = i;
            break;
        }
    }

    if (tag_id != HEAP_TAG_NONE) {
        block_t* curr = free_list;
        while (curr) {
            if (curr->magic != HEAP_MAGIC) break;
            if (!curr->free && curr->tag == tag_id) {
                void* payload = (void*)((uint8_t*)curr + BLOCK_HDR_SIZE);
                cb(payload, curr->size, heap_tag_names[tag_id], userdata);
            }
            curr = curr->next;
        }
    }

    umutex_unlock(&heap_lock);
}

void heap_iterate_all_tagged(HeapTagCallback cb, void *userdata) {
    if (!cb) return;

    umutex_lock(&heap_lock);

    block_t* curr = free_list;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) break;
        if (!curr->free && curr->tag != HEAP_TAG_NONE) {
            void* payload = (void*)((uint8_t*)curr + BLOCK_HDR_SIZE);
            const char *name = (curr->tag < heap_tag_count)
                               ? heap_tag_names[curr->tag]
                               : NULL;
            cb(payload, curr->size, name, userdata);
        }
        curr = curr->next;
    }

    umutex_unlock(&heap_lock);
}

void heap_dump_tags(void) {
    umutex_lock(&heap_lock);

    printf("[heap] tagged live blocks:\n");

    block_t* curr = free_list;
    bool found_any = false;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) {
            printf("[heap]   (corrupted block at 0x%x)\n", (uint32_t)(uintptr_t)curr);
            break;
        }
        if (!curr->free && curr->tag != HEAP_TAG_NONE) {
            const char *name = (curr->tag < heap_tag_count)
                               ? heap_tag_names[curr->tag]
                               : "(unknown)";
            void* payload = (void*)((uint8_t*)curr + BLOCK_HDR_SIZE);
            printf("[heap]   ptr=0x%x size=%u tag=%s\n",
                   (uint32_t)(uintptr_t)payload,
                   (uint32_t)curr->size,
                   name);
            found_any = true;
        }
        curr = curr->next;
    }

    if (!found_any) {
        printf("[heap]   (none)\n");
    }

    umutex_unlock(&heap_lock);
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void heap_get_stats(heap_stats_t *out) {
    if (!out) return;

    umutex_lock(&heap_lock);

    out->total_allocated = 0;
    out->total_free      = 0;
    out->alloc_count     = 0;
    out->free_count      = 0;
    out->malloc_calls    = stat_malloc_calls;
    out->free_calls      = stat_free_calls;
    out->heap_used       = (initialized) ? (heap_current - heap_base) : 0;

    block_t* curr = free_list;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) break;
        if (curr->free) {
            out->total_free += curr->size;
            out->free_count++;
        } else {
            out->total_allocated += curr->size;
            out->alloc_count++;
        }
        curr = curr->next;
    }

    umutex_unlock(&heap_lock);
}
