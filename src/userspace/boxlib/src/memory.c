// Prevent variadic malloc macro from expanding internal calls
#ifdef malloc
#undef malloc
#endif

#include "box/timeouts.h"
#include "box/memory.h"
#include "box/sync.h"
#include "box/string.h"
#include "box/core/notify.h"
#include "box/core/cabin.h"
#include "box/core/heap_blocks.h"  /* the global heap's blocks */
#include "box/core/manifest.h"
#include "box/core/strand_self.h"  /* strand_info_or_null / strand_self — pool selector */
#include "box/print.h"
#include "box/debug.h"             /* kdbg_print — a registry that refuses says so */
#include "strand_pool_abi.h"       /* StrandPool layout (shared with kernel death-stamp) */

/* Large pages for large blocks.
 *
 * A block of 2 MiB or more the heap grows by is laid over whole 2 MiB pages
 * none of whose bytes it has touched (heap_blocks.c grow_huge), and the kernel
 * is asked to back them at once with one PDE per chunk rather than hundreds
 * of 4 KiB demand faults. Smaller blocks stay on the 4 KiB demand-paging path.
 *
 * The pre-fault is SYSTEM_OP_HEAP_PREFAULT (boxos_decks.h), answered by
 * SysHeapPrefault in bay_ops.c. */
#include "boxos_decks.h"

// ---------------------------------------------------------------------------
// Thread-safe heap allocator with tag support and diagnostics: per-strand
// magazines over one global heap, whose blocks are found and merged in a time
// that does not depend on how many it holds (box/core/heap_blocks.h)
// ---------------------------------------------------------------------------

// ---- tag registry (all access under heap_lock) ----------------------------
//
// A name is kept on the heap at its own length and compared whole, so two tags
// are two tags however much of a prefix they share. The registry used to be 64
// names of 32 bytes compared over their first 31: past 64 tags every further
// tag allocated UNTAGGED, and two names alike for 31 characters became one id
// — both in silence, so heap_count_tag and the tag dump simply stopped seeing
// half of what was live and said nothing about it.
//
// The table of names is the id space itself (HEAP_TAG_CAP entries, which is
// every id block_t.tag can hold) and it is taken from the heap on the first
// tagged allocation, so a program that never tags anything carries nothing.
// Names are never released: an id that has been stamped into a live block must
// still have a name when the block is dumped.
static char   **heap_tag_names = NULL;
static uint8_t  heap_tag_count = 0;

/* What the registry could not do, for the CALLER to say once the lock is
 * dropped. kdbg is a syscall; its answer comes back through this strand's
 * ResultRing, and a foreign record standing ahead of that answer is kept in a
 * stash that grows from this very heap — said under heap_lock, that is the
 * lock taken twice and a spin that never ends (the same reason prefault_huge
 * is called outside it). */
typedef enum {
    TAG_SAY_NOTHING = 0,
    TAG_SAY_IDS_EXHAUSTED,
    TAG_SAY_NO_ROOM_FOR_NAME,
} TagSay;

// ---- internal state (all access under heap_lock) --------------------------

static uspin_t   heap_lock    = USPIN_INIT;

// ---- diagnostics ----------------------------------------------------------

/* The main strand's / shared-fallback heap error cell. Internal linkage: the
 * public API is heap_get_last_error() (per-strand), not this symbol. */
static error_t  heap_last_error = OK;

static uint32_t stat_malloc_calls = 0;

/* Depth of "this allocation is the heap's own bookkeeping, not the program's".
 *
 * `malloc_calls` answers the question a caller actually asks — how many times
 * did MY program go to the global allocator — and the tag registry is not that.
 * It is storage the allocator needed in order to answer, taken out of the same
 * heap because that is the only heap there is, and counting it would make the
 * statistic mean "calls plus whatever housekeeping happened to be due".
 *
 * That is not a philosophical point. cxxtest phase41 A5 measures the tagged
 * path by differencing this counter across exactly M pairs and requires the
 * answer to be M: it is checking that the tagged path takes the global lock
 * twice per pair and not a third time. The first tagged allocation in a program
 * builds the registry — a table and a copied name — and those two landed inside
 * the measured window and made the answer M+2.
 *
 * A depth rather than a flag because the registry's own allocation can, in
 * principle, be the one that grows the heap. Only ever touched under
 * heap_lock, which is also the only place the counter moves. */
static uint32_t stat_housekeeping_depth = 0;
static uint32_t stat_free_calls   = 0;

// ---- helpers (called with lock held) --------------------------------------

/* A block of the global heap for the program — counted, unless it is the
 * heap's own bookkeeping — or for the heap itself. A block that wants 2 MB
 * backing is only RESERVED: its range comes back in *huge_va / *huge_len for
 * the caller to pre-fault once the lock is dropped (alloc_backed). */
static void* alloc_locked(size_t size, uint8_t tag_id, error_t *errcell,
                          uintptr_t *huge_va, size_t *huge_len) {
    if (stat_housekeeping_depth == 0) stat_malloc_calls++;
    return heap_blocks_take(size, tag_id, errcell, huge_va, huge_len);
}

/* Look up or insert a tag name. Returns the id, or HEAP_TAG_NONE when there is
 * no id left to give or no room to remember the name — and then `say` carries
 * which of the two it was, for the caller to speak outside the lock.
 * Must be called with heap_lock held. */
static uint8_t get_or_create_tag_locked(const char *name, TagSay *say) {
    *say = TAG_SAY_NOTHING;
    if (!name || !name[0]) return HEAP_TAG_NONE;

    for (uint8_t i = 0; i < heap_tag_count; i++) {
        if (strcmp(heap_tag_names[i], name) == 0)
            return i;
    }

    if (heap_tag_count >= HEAP_TAG_CAP) {
        *say = TAG_SAY_IDS_EXHAUSTED;
        return HEAP_TAG_NONE;
    }

    error_t sink = 0;

    /* The registry is the allocator's own, so the two allocations below are
     * not the program's and are not counted as such — see
     * stat_housekeeping_depth. They still take memory from this heap, and
     * heap_stats' BYTE figures still show them, because the bytes really are
     * gone; it is the CALL COUNT that would be a lie. */
    stat_housekeeping_depth++;

    if (!heap_tag_names) {
        size_t table = (size_t)HEAP_TAG_CAP * sizeof(char *);
        heap_tag_names = (char **)alloc_locked(table, HEAP_TAG_NONE, &sink,
                                               NULL, NULL);
        if (!heap_tag_names) {
            stat_housekeeping_depth--;
            *say = TAG_SAY_NO_ROOM_FOR_NAME; return HEAP_TAG_NONE;
        }
        memset(heap_tag_names, 0, table);
    }

    size_t len  = strlen(name);
    char  *kept = (char *)alloc_locked(len + 1, HEAP_TAG_NONE, &sink, NULL, NULL);
    stat_housekeeping_depth--;
    if (!kept) { *say = TAG_SAY_NO_ROOM_FOR_NAME; return HEAP_TAG_NONE; }
    memcpy(kept, name, len + 1);

    uint8_t id = heap_tag_count++;
    heap_tag_names[id] = kept;
    return id;
}

/* Say what the registry refused — once per program, and never under the lock.
 *
 * Once, because the refusal is permanent: an id space that is full stays full,
 * and the program would otherwise repeat itself for every allocation it makes
 * from here to its exit. Once is enough for the reader the line is written
 * for, who is looking at a heap dump that is missing blocks and needs to know
 * why. */
static void heap_tag_say(TagSay say, const char *name) {
    static uint8_t said_exhausted = 0;
    static uint8_t said_no_room   = 0;

    if (say == TAG_SAY_IDS_EXHAUSTED) {
        if (__atomic_exchange_n(&said_exhausted, 1, __ATOMIC_RELAXED)) return;
        kdbg_print("[heap] all %u tag ids are in use — a block header carries its tag "
                   "in one byte — so every tag after this one allocates UNTAGGED and "
                   "no dump can see it: %s",
                   (unsigned)HEAP_TAG_CAP, name ? name : "(none)");
    } else if (say == TAG_SAY_NO_ROOM_FOR_NAME) {
        if (__atomic_exchange_n(&said_no_room, 1, __ATOMIC_RELAXED)) return;
        kdbg_print("[heap] no room on this heap to remember a tag name, so the block "
                   "is UNTAGGED and the dump will not show it: %s",
                   name ? name : "(none)");
    }
}

/* Pre-fault a 2 MB-aligned, 2 MB-sized region. The kernel maps every
 * 2 MB chunk inside [va_base, va_base+size_2m) with one PDE leaf,
 * falling back to 4 KB pages transparently if PMM fragmentation
 * prevents a chunk-level allocation. Returns 0 on success.
 *
 * Called OUTSIDE heap_lock — it is a syscall, and its reply comes through
 * this strand's ResultRing. A foreign record ahead of that reply is kept in
 * a stash that grows from this very heap (box/core/stash.h): under the lock
 * that is the lock taken twice, a spin that never ends. */
static int prefault_huge(uintptr_t va_base, uint64_t size_2m_aligned) {
    uint8_t params[16];
    uint64_t va64 = (uint64_t)va_base;
    memcpy(params,     &va64,           sizeof(uint64_t));
    memcpy(params + 8, &size_2m_aligned, sizeof(uint64_t));
    return MfCall1(DECK_SYSTEM, SYSTEM_OP_HEAP_PREFAULT,
                   params, sizeof(params),
                   NULL, 0, NULL, 0, NULL,
                   BOX_ANSWER_GUARANTEED, NULL);
}

/* The live blocks of the global heap, counted in memory order. heap_lock HELD. */
static uint32_t live_blocks_locked(void) {
    uint32_t live = 0;
    for (block_t *b = heap_blocks_first(); b && b->magic == HEAP_MAGIC; b = heap_blocks_next(b))
        if (b->state == BLOCK_USED) live++;
    return live;
}

// ---------------------------------------------------------------------------
// StrandPool — per-strand magazine cache over the one global-locked heap
//
// Each strand owns one StrandPool (a private LIFO of free blocks per size
// class). The fast path pops/pushes its own pool with NO lock — the same
// single-writer-per-strand model as the per-strand result stash. A cached block
// stays USED (the global heap still sees it LIVE); the magazine link is stored
// in the block's payload first 8 bytes. The ONE global heap remains the source
// of truth: refill takes N blocks from it, flush gives them back.
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

/* Defensive clamp on the high-water mark: pool_claim only ever sets it to
 * i+1 for an i bounded by STRAND_POOL_SLAB_MAX, so this should be a no-op —
 * but the reclaim scan walks g_pool_slab[] by this bound, so clamping here
 * means a corrupted or otherwise-unexpected hwm can never carry that scan
 * past the array's real end. */
static inline uint32_t pool_slab_hwm_clamped(void) {
    uint32_t hwm = g_pool_slab_hwm;
    return hwm > STRAND_POOL_SLAB_MAX ? STRAND_POOL_SLAB_MAX : hwm;
}

static inline unsigned high_bit(size_t v) {
    return 63u - (unsigned)__builtin_clzll((unsigned long long)v);
}

/* Floor map: smallest class whose size >= n (so a served block is always at
 * least as large as requested) — 16, 32, 48, 64 by sixteens, then the powers
 * of two to 8192. n > 8192 → bypass to the locked global path. */
static unsigned StrandPoolSizeToClass(size_t n) {
    if (n <= 64) return n <= 16 ? 0u : (unsigned)((n - 1) >> 4);
    if (n > STRAND_POOL_MAX_CLASS) return STRAND_POOL_NO_CLASS;
    return high_bit(n - 1) - 2u;
}

/* Largest class whose size <= a freed block's actual size (s > 8192 → bypass).
 * Pairs with the floor alloc map: a block grown for class c has size >=
 * ClassSize[c], so it maps back to c or higher — never below its served class. */
static unsigned StrandPoolClassFromBlockSize(size_t s) {
    if (s < 16 || s > STRAND_POOL_MAX_CLASS) return STRAND_POOL_NO_CLASS;
    if (s < 128) return s >= 64 ? 3u : (unsigned)(s >> 4) - 1u;
    return high_bit(s) - 3u;
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
                  BOX_ANSWER_GUARANTEED, NULL);
}

/* Give up to `drop` cached blocks of class c back to the global heap. heap_lock
 * HELD. The walk is hard-bounded by Counts[c] and validates each block's magic
 * BEFORE following its payload link, so a corrupted link can never fault under
 * the lock or loop. */
static void pool_flush_class_locked(StrandPool *pool, unsigned c, unsigned drop) {
    unsigned guard = pool->Counts[c];
    while (drop-- && guard-- && pool->Heads[c]) {
        void *payload = pool->Heads[c];
        block_t *block = block_of(payload);
        if (block->magic != HEAP_MAGIC) break;
        pool->Heads[c] = *(void **)payload;
        pool->Counts[c]--;
        if (block->state == BLOCK_USED) (void)heap_blocks_give(block);
    }
}

/* Reclaim every ORPHANED slab slot: give its cached blocks back to the global
 * heap, then mark the slot FREE at the next generation. Called only on the slow
 * path, with heap_lock HELD. Bounded by the slab size. */
static void reclaim_one_orphan_slot_locked(StrandPool *p, uint32_t word) {
    for (unsigned c = 0; c < STRAND_POOL_CLASS_COUNT; c++) {
        pool_flush_class_locked(p, c, (unsigned)-1);
        p->Heads[c]  = NULL;
        p->Counts[c] = 0;
    }
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
    for (unsigned i = 0; i < pool_slab_hwm_clamped(); i++) {
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

    uspin_lock(&heap_lock);
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
            for (unsigned j = 0; j < pool_slab_hwm_clamped(); j++) {
                StrandPool *p2 = &g_pool_slab[j];
                uint32_t word2 = __atomic_load_n(&p2->GenState, __ATOMIC_ACQUIRE);
                if (STRANDPOOL_STATE(word2) != STRANDPOOL_ORPHANED) continue;
                reclaim_one_orphan_slot_locked(p2, word2);
            }
            __atomic_store_n(&g_strandpool_orphan_pending, 0, __ATOMIC_RELEASE);
        }
    }
    uspin_unlock(&heap_lock);

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
        void *payload = alloc_locked(StrandPoolClassSize[c], HEAP_TAG_NONE, errcell, NULL, NULL);
        if (!payload) break;   /* heap exhausted — serve whatever we got */
        *(void **)payload = pool->Heads[c];
        pool->Heads[c]    = payload;
        pool->Counts[c]++;
    }
}

/* Orderly flush of the calling strand's whole pool at strand/cabin exit. Gives
 * every magazine back to the global heap, then bumps the slot's generation to
 * FREE — the bump makes the kernel death-stamp CAS miss, so no unbind syscall
 * is needed. Idempotent: a strand with no pool is a no-op. */
void strand_pool_flush_self(void) {
    StrandInfo *si   = strand_info_or_null();
    StrandPool *pool = si ? (StrandPool *)(uintptr_t)si->strand_pool_ptr : &g_main_pool;
    if (!pool) return;
    if (si && si->strand_pool_ptr == 0) return;
    if (!si && __atomic_load_n(&g_main_pool.GenState, __ATOMIC_ACQUIRE) == 0) return;

    uspin_lock(&heap_lock);
    for (unsigned c = 0; c < STRAND_POOL_CLASS_COUNT; c++)
        pool_flush_class_locked(pool, c, (unsigned)-1);
    uint32_t word = __atomic_load_n(&pool->GenState, __ATOMIC_RELAXED);
    pool->OwnerPid = 0;
    __atomic_store_n(&pool->GenState,
                     STRANDPOOL_PACK(STRANDPOOL_GEN(word) + 1, STRANDPOOL_FREE),
                     __ATOMIC_RELEASE);
    uspin_unlock(&heap_lock);

    if (si) si->strand_pool_ptr = 0;
}

/* Self-test of the crash-orphan reclaim MECHANISM (not a live fault). Stages a
 * spare slab slot exactly as a crashed strand would leave it — real heap blocks
 * cached in its magazines (USED, magazine-linked) with the slot marked
 * ORPHANED — then runs the same reclaim_orphans_scan_locked the slow path uses.
 * Returns 1 if every staged block came back to the global heap and the slot is
 * FREE again; 0 on any inconsistency. `n_blocks` is clamped to a class cap. */
int strand_pool_test_orphan_reclaim(unsigned n_blocks) {
    const unsigned c = 3;  /* the 64-byte class */
    if (n_blocks == 0 || n_blocks > StrandPoolCapForClass[c]) return 0;

    uspin_lock(&heap_lock);

    /* Find a FREE spare slot near the top of the slab (away from live claims). */
    StrandPool *slot = NULL;
    for (int i = (int)STRAND_POOL_SLAB_MAX - 1; i >= 0; i--) {
        if (STRANDPOOL_STATE(g_pool_slab[i].GenState) == STRANDPOOL_FREE) {
            slot = &g_pool_slab[i];
            break;
        }
    }
    if (!slot) { uspin_unlock(&heap_lock); return 0; }

    uint32_t gen = STRANDPOOL_GEN(slot->GenState);

    /* Cache real blocks the way the fast path does: take them from the global
     * heap (USED) and link through the payload. */
    error_t staging_err = OK;
    for (unsigned k = 0; k < n_blocks; k++) {
        void *payload = alloc_locked(StrandPoolClassSize[c], HEAP_TAG_NONE, &staging_err, NULL, NULL);
        if (!payload) break;
        *(void **)payload = slot->Heads[c];
        slot->Heads[c]    = payload;
        slot->Counts[c]++;
    }
    unsigned staged = slot->Counts[c];

    /* Count live blocks before reclaim, then stamp ORPHANED and reclaim. */
    uint32_t live_before = live_blocks_locked();

    slot->OwnerPid = 0;
    __atomic_store_n(&slot->GenState,
                     STRANDPOOL_PACK(gen, STRANDPOOL_ORPHANED), __ATOMIC_RELEASE);

    /* Update HWM so the flag-gated scan covers this test slot. */
    unsigned slot_idx = (unsigned)(slot - g_pool_slab);
    if (slot_idx + 1 > g_pool_slab_hwm) g_pool_slab_hwm = slot_idx + 1;
    __atomic_store_n(&g_strandpool_orphan_pending, 1, __ATOMIC_RELEASE);
    reclaim_orphans_scan_locked();

    uint32_t live_after = live_blocks_locked();

    int ok = (STRANDPOOL_STATE(slot->GenState) == STRANDPOOL_FREE) &&
             (slot->Heads[c] == NULL) && (slot->Counts[c] == 0) &&
             (staged > 0) && (live_after + staged == live_before);

    uspin_unlock(&heap_lock);
    return ok;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/* A block of the global heap, a large one backed. The large pages are the
 * kernel's help, never a condition: a reservation it does not back at once is
 * given all the same, its pages backed as they are touched — the heap's own
 * way below 2 MB — save where the kernel says why it may not be: the machine
 * has no memory for it (then it is given back, its pages faulting in one by
 * one if a smaller allocation ever takes it, and the caller gets NULL and the
 * cause), or a mapping not the heap's lies in the range (then not a byte of
 * it is touched, the block is FOREIGN for good, and the block is taken again
 * past it). Each is said, outside the lock. */
static void* alloc_backed(size_t size, uint8_t tag_id, error_t *errcell) {
    for (int tries = 0; tries < 4; tries++) {
        uintptr_t huge_va = 0; size_t huge_len = 0;
        uspin_lock(&heap_lock);
        void* ptr = alloc_locked(size, tag_id, errcell, &huge_va, &huge_len);
        uspin_unlock(&heap_lock);
        if (!ptr || huge_len == 0) return ptr;

        const int     refused = prefault_huge(huge_va, (uint64_t)huge_len);
        const error_t why     = refused < 0 ? (error_t)-refused : (error_t)refused;
        if (refused == 0) return ptr;
        if (why == ERR_INVALID_ADDRESS) {
            kdbg_print("[heap] %lu bytes at 0x%lx meet a mapping not the heap's: left untouched, the block "
                       "taken past them", (unsigned long)huge_len, (unsigned long)huge_va);
            uspin_lock(&heap_lock);
            heap_blocks_bar(block_of(ptr));
            uspin_unlock(&heap_lock);
            *errcell = ERR_INVALID_ADDRESS;
            continue;
        }
        if (why == ERR_NO_MEMORY) {
            kdbg_print("[heap] %lu bytes at 0x%lx: the machine has no memory to back them, the allocation "
                       "is refused", (unsigned long)huge_len, (unsigned long)huge_va);
            uspin_lock(&heap_lock);
            (void)heap_blocks_give(block_of(ptr));
            uspin_unlock(&heap_lock);
            *errcell = ERR_NO_MEMORY;
            return NULL;
        }
        kdbg_print("[heap] %lu bytes at 0x%lx were not backed at once (error %u): they are backed as "
                   "they are touched", (unsigned long)huge_len, (unsigned long)huge_va, (unsigned)why);
        return ptr;
    }
    return NULL;
}

void* malloc(size_t size) {
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
         * global heap. The cause lands in this strand's cell. */
        return alloc_backed(size, HEAP_TAG_NONE, errcell);
    }

    if (!pool->Heads[c]) {
        uspin_lock(&heap_lock);
        reclaim_orphans_scan_locked();
        pool_refill_locked(pool, c);
        uspin_unlock(&heap_lock);
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

/* aligned_alloc — [c.malloc] requires that the pointer it returns be released
 * by the ORDINARY free(), and that is the whole reason this lives inside the
 * allocator instead of being a wrapper.
 *
 * The trick every hosted libc-less project reaches for first — over-allocate,
 * return an aligned address inside, stash the real pointer just below it — is
 * exactly what boxcxx's aligned operator new does, and it works there because
 * the aligned DELETE knows to look for the stash. free() does not: it reads the
 * 32 bytes in front of the payload and expects a block header with the heap
 * magic. So this puts a REAL header there: the block taken is cut so that a
 * header lands exactly at (aligned address − 32), what lies in front of it
 * given back, and so is what lies past the size asked (heap_blocks_align).
 * free() then sees an ordinary block, because that is what it is.
 *
 * The over-allocation is size + alignment + one header + HEAP_ALIGN: the last
 * term guarantees the leading remainder is itself a usable block rather than a
 * sliver too small to carry a header, which is the case that would otherwise
 * force a second alignment step and eat into the payload. */
void* aligned_alloc(size_t alignment, size_t size) {
    if (size == 0) return NULL;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return NULL;
    if (alignment <= HEAP_ALIGN) return malloc(size);

    size_t raw = size + alignment + BLOCK_HDR_SIZE + HEAP_ALIGN;
    if (raw < size) return NULL;                      /* overflow */

    StrandPool *pool    = pool_self();
    error_t    *errcell = heap_err_cell_for(pool);

    void* p = alloc_backed(raw, HEAP_TAG_NONE, errcell);
    if (!p) return NULL;

    /* Cut at the aligned address — under the lock again, and only now that
     * the block has its backing. */
    uspin_lock(&heap_lock);
    void *aligned = heap_blocks_align(block_of(p), alignment, size);
    uspin_unlock(&heap_lock);
    if (!aligned) *errcell = ERR_CORRUPTED;
    return aligned;
}

void* malloc_tagged(size_t size, const char *tag) {
    if (size == 0) return NULL;

    /* pool_self() (which may claim a slot) is called BEFORE the heap lock —
     * pool_claim takes the lock itself, so resolving it here avoids re-entry. */
    error_t *errcell = heap_err_cell_for(pool_self());
    TagSay say = TAG_SAY_NOTHING;
    uspin_lock(&heap_lock);
    uint8_t tag_id = get_or_create_tag_locked(tag, &say);
    uspin_unlock(&heap_lock);
    /* Outside the lock: saying it is a syscall. */
    if (say != TAG_SAY_NOTHING) heap_tag_say(say, tag);
    return alloc_backed(size, tag_id, errcell);
}

/* Return a block to the global heap under the lock — the slow path shared by
 * free()'s global cases (corrupt/double-free/tagged/oversized/overflow). */
static void free_global(void* ptr) {
    error_t *errcell = heap_err_cell_for(pool_self());
    uspin_lock(&heap_lock);

    stat_free_calls++;

    block_t* block = block_of(ptr);

    if (block->magic != HEAP_MAGIC) {
        *errcell = ERR_CORRUPTED;
        uspin_unlock(&heap_lock);
        return;
    }

    if (block->state != BLOCK_USED) {
        *errcell = ERR_INVALID_ADDRESS;
        uspin_unlock(&heap_lock);
        return;
    }

    *errcell = heap_blocks_give(block);
    uspin_unlock(&heap_lock);
}

void free(void* ptr) {
    if (!ptr) return;

    block_t* block = block_of(ptr);

    /* A wild/corrupt pointer is caught here (magic is write-once); a block the
     * global heap holds as not USED (real double-free of a flushed block) and
     * any tagged block go to the locked global path that owns those semantics. */
    if (block->magic != HEAP_MAGIC)     { free_global(ptr); return; }
    if (block->state != BLOCK_USED)     { free_global(ptr); return; }
    if (block->tag != HEAP_TAG_NONE)    { free_global(ptr); return; }

    unsigned c = StrandPoolClassFromBlockSize(block->size);
    if (c == STRAND_POOL_NO_CLASS) { free_global(ptr); return; }

    StrandPool *pool = pool_self();
    if (!pool) { free_global(ptr); return; }
    error_t *errcell = heap_err_cell_for(pool);

    /* Double-free of a still-cached block: scan this magazine (<= cap pointers).
     * The global free() above cannot catch it because a cached block is USED.
     * Bound the scan by Counts[c] and validate each node's magic BEFORE following
     * its payload link, so a corrupted cached link cannot fault or loop here. */
    unsigned guard = pool->Counts[c];
    for (void *node = pool->Heads[c]; node && guard--; ) {
        if (node == ptr) {
            *errcell = ERR_INVALID_ADDRESS;
            return;
        }
        if (block_of(node)->magic != HEAP_MAGIC) break;
        node = *(void **)node;
    }

    if (pool->Counts[c] >= StrandPoolCapForClass[c]) {
        uspin_lock(&heap_lock);
        reclaim_orphans_scan_locked();
        pool_flush_class_locked(pool, c, STRAND_POOL_REFILL_BATCH);
        uspin_unlock(&heap_lock);
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

    void* ptr = malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    error_t *errcell = heap_err_cell_for(pool_self());
    uspin_lock(&heap_lock);

    block_t* block = block_of(ptr);
    if (block->magic != HEAP_MAGIC) {
        *errcell = ERR_CORRUPTED;
        uspin_unlock(&heap_lock);
        return NULL;
    }
    if (block->state != BLOCK_USED) {
        *errcell = ERR_INVALID_ADDRESS;
        uspin_unlock(&heap_lock);
        return NULL;
    }

    /* Where it stands: shrunk, or grown into a free block above it or into
     * the heap's end. */
    if (heap_blocks_resize(block, size)) {
        *errcell = OK;
        uspin_unlock(&heap_lock);
        return ptr;
    }

    /* Must move — under its own tag, which a tagged block keeps. */
    size_t  old_size = block->size;
    uint8_t old_tag  = block->tag;
    uspin_unlock(&heap_lock);

    void* new_ptr = old_tag != HEAP_TAG_NONE ? alloc_backed(size, old_tag, errcell) : malloc(size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, old_size < size ? old_size : size);
    free(ptr);
    return new_ptr;
}

// ---------------------------------------------------------------------------
// Tag registry query API
// ---------------------------------------------------------------------------

uint8_t heap_register_tag(const char *name) {
    TagSay say = TAG_SAY_NOTHING;
    uspin_lock(&heap_lock);
    uint8_t id = get_or_create_tag_locked(name, &say);
    uspin_unlock(&heap_lock);
    if (say != TAG_SAY_NOTHING) heap_tag_say(say, name);
    return id;
}

/* The id of a registered tag name, HEAP_TAG_NONE when there is none.
 * heap_lock HELD. */
static uint8_t lookup_tag_locked(const char *name) {
    for (uint8_t i = 0; i < heap_tag_count; i++) {
        if (strcmp(heap_tag_names[i], name) == 0)
            return i;
    }
    return HEAP_TAG_NONE;
}

uint8_t heap_lookup_tag(const char *name) {
    if (!name) return HEAP_TAG_NONE;

    uspin_lock(&heap_lock);
    uint8_t result = lookup_tag_locked(name);
    uspin_unlock(&heap_lock);
    return result;
}

const char *heap_tag_name(uint8_t id) {
    if (id == HEAP_TAG_NONE) return NULL;

    uspin_lock(&heap_lock);
    const char *name = (id < heap_tag_count) ? heap_tag_names[id] : NULL;
    uspin_unlock(&heap_lock);
    return name;
}

size_t heap_count_tag(const char *tag) {
    if (!tag) return 0;

    uspin_lock(&heap_lock);

    uint8_t tag_id = lookup_tag_locked(tag);
    size_t  count  = 0;
    if (tag_id != HEAP_TAG_NONE) {
        for (block_t *b = heap_blocks_first(); b && b->magic == HEAP_MAGIC; b = heap_blocks_next(b))
            if (b->state == BLOCK_USED && b->tag == tag_id) count++;
    }

    uspin_unlock(&heap_lock);
    return count;
}

void heap_iterate_tag(const char *tag, HeapTagCallback cb, void *userdata) {
    if (!tag || !cb) return;

    uspin_lock(&heap_lock);

    uint8_t tag_id = lookup_tag_locked(tag);
    if (tag_id != HEAP_TAG_NONE) {
        for (block_t *b = heap_blocks_first(); b && b->magic == HEAP_MAGIC; b = heap_blocks_next(b))
            if (b->state == BLOCK_USED && b->tag == tag_id)
                cb(payload_of(b), b->size, heap_tag_names[tag_id], userdata);
    }

    uspin_unlock(&heap_lock);
}

void heap_iterate_all_tagged(HeapTagCallback cb, void *userdata) {
    if (!cb) return;

    uspin_lock(&heap_lock);

    for (block_t *b = heap_blocks_first(); b && b->magic == HEAP_MAGIC; b = heap_blocks_next(b)) {
        if (b->state == BLOCK_USED && b->tag != HEAP_TAG_NONE) {
            const char *name = (b->tag < heap_tag_count) ? heap_tag_names[b->tag] : NULL;
            cb(payload_of(b), b->size, name, userdata);
        }
    }

    uspin_unlock(&heap_lock);
}

void heap_dump_tags(void) {
    uspin_lock(&heap_lock);

    printf("[heap] tagged live blocks:\n");

    bool found_any = false;
    for (block_t *b = heap_blocks_first(); b; b = heap_blocks_next(b)) {
        if (b->magic != HEAP_MAGIC) {
            printf("[heap]   (corrupted block at 0x%x)\n", (uint32_t)(uintptr_t)b);
            break;
        }
        if (b->state == BLOCK_USED && b->tag != HEAP_TAG_NONE) {
            const char *name = (b->tag < heap_tag_count) ? heap_tag_names[b->tag] : "(unknown)";
            printf("[heap]   ptr=0x%x size=%u tag=%s\n",
                   (uint32_t)(uintptr_t)payload_of(b),
                   (uint32_t)b->size,
                   name);
            found_any = true;
        }
    }

    if (!found_any) {
        printf("[heap]   (none)\n");
    }

    uspin_unlock(&heap_lock);
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void heap_get_stats(heap_stats_t *out) {
    if (!out) return;

    uspin_lock(&heap_lock);

    out->total_allocated = 0;
    out->total_free      = 0;
    out->alloc_count     = 0;
    out->free_count      = 0;
    out->malloc_calls    = stat_malloc_calls;
    out->free_calls      = stat_free_calls;
    out->heap_used       = heap_blocks_grown();

    for (block_t *b = heap_blocks_first(); b && b->magic == HEAP_MAGIC; b = heap_blocks_next(b)) {
        if (b->state == BLOCK_FREE) {
            out->total_free += b->size;
            out->free_count++;
        } else if (b->state == BLOCK_USED) {
            out->total_allocated += b->size;
            out->alloc_count++;
        }
    }

    uspin_unlock(&heap_lock);
}
