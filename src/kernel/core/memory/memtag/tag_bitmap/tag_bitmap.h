/*
 * MemTag — Bitmap Inverted Index + Query Cache
 *
 * Mirror of TagFS src/kernel/tagfs/tag_bitmap/. Each interned tag_id owns a
 * bitmap of region_ids that have it. Sparse — bitmap[tag_id] is NULL until
 * a region first claims the tag. Set/clear → flip a single bit + bump
 * generation. Query → bitmap AND-fold across required tags, OR-fold across
 * "any" tags, then AND-NOT across excluded tags.
 *
 * 16-slot LRU query cache (TagFS-parity). Cache invalidated implicitly via
 * generation counter — every set/clear bumps it; cache entries store a
 * generation snapshot and are invalidated when stale.
 */

#ifndef MEMTAG_BITMAP_H
#define MEMTAG_BITMAP_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"

#define MEMTAG_QUERY_CACHE_SLOTS   16
#define MEMTAG_BITMAP_INITIAL_TAGS 64      /* matches tag_registry initial   */
#define MEMTAG_BITMAP_INITIAL_REGS 1024    /* matches region_registry initial */

typedef struct MemTagBitmap {
    uint8_t  *bits;         /* raw bitmap data (byte-packed)          */
    uint32_t  bit_count;    /* allocated bit count (capacity)         */
    uint32_t  set_count;    /* number of bits currently 1             */
} MemTagBitmap;

typedef struct MemTagQueryCacheEntry {
    uint64_t   key_hash;       /* FNV-1a of (sorted tag_ids + type)   */
    uint64_t   gen_snapshot;   /* generation when cached              */
    uint32_t  *result_ids;     /* cached region_ids[]                 */
    uint32_t   result_count;
    uint16_t  *tag_key;        /* copy of sorted tag_ids (validate)   */
    uint16_t   tag_count;
    uint8_t    query_type;     /* 0=AND, 1=OR, 2=MIXED                */
    uint8_t    used;           /* 0=empty slot                        */
    uint64_t   last_used;      /* monotonic counter for LRU eviction  */
} MemTagQueryCacheEntry;

typedef struct MemTagBitmapIndex {
    MemTagBitmap **bitmaps;     /* [tag_id], sparse (NULL=no regions)  */
    uint32_t       bitmap_cap;  /* grows with tag registry             */

    uint32_t       max_region_id;  /* highest region_id seen          */
    uint32_t       region_cap;     /* bitmap bits sized to this       */

    volatile uint64_t generation;

    MemTagQueryCacheEntry cache[MEMTAG_QUERY_CACHE_SLOTS];
    volatile uint64_t     cache_counter;  /* monotonic for LRU stamping */
    uint64_t              cache_hits;
    uint64_t              cache_misses;

    spinlock_t            lock;
} MemTagBitmapIndex;

/* ─── Lifecycle ────────────────────────────────────────────────────────── */

error_t   MemTagBitmapInit(MemTagBitmapIndex *idx,
                            uint32_t initial_tag_cap,
                            uint32_t initial_region_cap);
void      MemTagBitmapShutdown(MemTagBitmapIndex *idx);

/* ─── Bit-level mutation ──────────────────────────────────────────────── */

/* Set (tag, region) bit. Returns OK or NO_MEMORY (bitmap growth failed). */
error_t   MemTagBitmapSet(MemTagBitmapIndex *idx,
                           uint16_t tag_id, uint32_t region_id);

/* Clear (tag, region) bit. Idempotent. */
error_t   MemTagBitmapClear(MemTagBitmapIndex *idx,
                             uint16_t tag_id, uint32_t region_id);

/* Clear region_id from EVERY tag bitmap (region destroy). */
void      MemTagBitmapRemoveRegion(MemTagBitmapIndex *idx, uint32_t region_id);

/* O(1) "does region have tag?" via bitmap bit-test. */
bool      MemTagBitmapHas(MemTagBitmapIndex *idx,
                           uint16_t tag_id, uint32_t region_id);

/* ─── Query ───────────────────────────────────────────────────────────── */

/* Query types — passed to cache for key hashing */
#define MEMTAG_QUERY_TYPE_AND    0
#define MEMTAG_QUERY_TYPE_OR     1
#define MEMTAG_QUERY_TYPE_MIXED  2

/* Bitmap AND of `tag_ids`. Writes up to `max_results` region_ids into out.
 * Returns count. Cache-aware: same query repeats hit the LRU cache. */
size_t    MemTagBitmapQueryAnd(MemTagBitmapIndex *idx,
                                const uint16_t *tag_ids, uint16_t tag_count,
                                uint32_t *out, size_t max_results);

/* Bitmap OR of `tag_ids`. */
size_t    MemTagBitmapQueryOr(MemTagBitmapIndex *idx,
                               const uint16_t *tag_ids, uint16_t tag_count,
                               uint32_t *out, size_t max_results);

/* Full algebra: (AND required) ∧ (OR any) ∧ (NOT OR excluded). */
size_t    MemTagBitmapQueryMixed(MemTagBitmapIndex *idx,
                                  const uint16_t *required, uint16_t n_req,
                                  const uint16_t *any,      uint16_t n_any,
                                  const uint16_t *excluded, uint16_t n_excl,
                                  uint32_t *out, size_t max_results);

/* ─── Stats ───────────────────────────────────────────────────────────── */

uint64_t  MemTagBitmapGeneration(MemTagBitmapIndex *idx);
uint64_t  MemTagBitmapCacheHits(MemTagBitmapIndex *idx);
uint64_t  MemTagBitmapCacheMisses(MemTagBitmapIndex *idx);
void      MemTagBitmapInvalidateCache(MemTagBitmapIndex *idx);
void      MemTagBitmapDump(MemTagBitmapIndex *idx);

#endif /* MEMTAG_BITMAP_H */
