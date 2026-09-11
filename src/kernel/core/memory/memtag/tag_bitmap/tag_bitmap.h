
#ifndef MEMTAG_BITMAP_H
#define MEMTAG_BITMAP_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"

#define MEMTAG_QUERY_CACHE_SLOTS   16
#define MEMTAG_BITMAP_INITIAL_TAGS 64
#define MEMTAG_BITMAP_INITIAL_REGS 1024

typedef struct MemTagBitmap {
    uint8_t  *bits;
    uint32_t  bit_count;
    uint32_t  set_count;
} MemTagBitmap;

typedef struct MemTagQueryCacheEntry {
    uint64_t   key_hash;
    uint64_t   gen_snapshot;
    uint32_t  *result_ids;
    uint32_t   result_count;
    uint16_t  *tag_key;
    uint16_t   tag_count;
    uint8_t    query_type;
    uint8_t    used;
    uint64_t   last_used;
} MemTagQueryCacheEntry;

typedef struct MemTagBitmapIndex {
    MemTagBitmap **bitmaps;
    uint32_t       bitmap_cap;

    uint32_t       max_region_id;
    uint32_t       region_cap;

    volatile uint64_t generation;

    MemTagQueryCacheEntry cache[MEMTAG_QUERY_CACHE_SLOTS];
    volatile uint64_t     cache_counter;
    uint64_t              cache_hits;
    uint64_t              cache_misses;

    spinlock_t            lock;
} MemTagBitmapIndex;


error_t   MemTagBitmapInit(MemTagBitmapIndex *idx,
                            uint32_t initial_tag_cap,
                            uint32_t initial_region_cap);
void      MemTagBitmapShutdown(MemTagBitmapIndex *idx);


error_t   MemTagBitmapSet(MemTagBitmapIndex *idx,
                           uint16_t tag_id, uint32_t region_id);

error_t   MemTagBitmapClear(MemTagBitmapIndex *idx,
                             uint16_t tag_id, uint32_t region_id);

void      MemTagBitmapRemoveRegion(MemTagBitmapIndex *idx, uint32_t region_id);

bool      MemTagBitmapHas(MemTagBitmapIndex *idx,
                           uint16_t tag_id, uint32_t region_id);


#define MEMTAG_QUERY_TYPE_AND    0
#define MEMTAG_QUERY_TYPE_OR     1
#define MEMTAG_QUERY_TYPE_MIXED  2

size_t    MemTagBitmapQueryAnd(MemTagBitmapIndex *idx,
                                const uint16_t *tag_ids, uint16_t tag_count,
                                uint32_t *out, size_t max_results);

size_t    MemTagBitmapQueryOr(MemTagBitmapIndex *idx,
                               const uint16_t *tag_ids, uint16_t tag_count,
                               uint32_t *out, size_t max_results);

size_t    MemTagBitmapQueryMixed(MemTagBitmapIndex *idx,
                                  const uint16_t *required, uint16_t n_req,
                                  const uint16_t *any,      uint16_t n_any,
                                  const uint16_t *excluded, uint16_t n_excl,
                                  uint32_t *out, size_t max_results);


uint64_t  MemTagBitmapGeneration(MemTagBitmapIndex *idx);
uint64_t  MemTagBitmapCacheHits(MemTagBitmapIndex *idx);
uint64_t  MemTagBitmapCacheMisses(MemTagBitmapIndex *idx);
void      MemTagBitmapInvalidateCache(MemTagBitmapIndex *idx);
void      MemTagBitmapDump(MemTagBitmapIndex *idx);

#endif