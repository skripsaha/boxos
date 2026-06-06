/*
 * MemTag — Bitmap Inverted Index Implementation
 *
 * Sparse per-tag bitmaps (lazy alloc on first use). Geometric growth on
 * region_id overflow. AND/OR/AND-NOT bitmap set algebra. 16-slot LRU
 * query cache invalidated by generation counter.
 *
 * Mirrors TagFS tag_bitmap.c with these adaptations:
 *   - region_id is uint32_t (vs TagFS uint32_t file_id) — same width
 *   - no file_to_tags shadow (MemRegion.tag_ids[] already serves that role)
 *   - inline FNV-1a cache hash (matches TagFS convention)
 */

#include "tag_bitmap.h"

/* ─── Bit helpers ─────────────────────────────────────────────────────── */

static inline void BitsSet(uint8_t *bits, uint32_t idx) {
    bits[idx >> 3] |= (uint8_t)(1u << (idx & 7));
}

static inline void BitsClear(uint8_t *bits, uint32_t idx) {
    bits[idx >> 3] &= (uint8_t)~(1u << (idx & 7));
}

static inline bool BitsTest(const uint8_t *bits, uint32_t idx) {
    return (bits[idx >> 3] >> (idx & 7)) & 1u;
}

static inline size_t BitsBytes(uint32_t bit_count) {
    return (bit_count + 7) / 8;
}

/* ─── FNV-1a 64-bit (matches TagFS cache hash) ───────────────────────── */

static uint64_t Fnv1a64(const void *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ─── Bitmap growth ──────────────────────────────────────────────────── */

static error_t BitmapEnsureCapacity(MemTagBitmap *bm, uint32_t needed_bit) {
    if (needed_bit < bm->bit_count) return OK;
    uint32_t new_count = bm->bit_count ? bm->bit_count : 64;
    while (new_count <= needed_bit) {
        uint32_t doubled = new_count * 2;
        if (doubled <= new_count) return ERR_NO_MEMORY;  /* overflow */
        new_count = doubled;
    }
    size_t old_bytes = BitsBytes(bm->bit_count);
    size_t new_bytes = BitsBytes(new_count);
    uint8_t *new_bits = (uint8_t *)kmalloc(new_bytes);
    if (!new_bits) return ERR_NO_MEMORY;
    memset(new_bits, 0, new_bytes);
    if (bm->bits && old_bytes > 0) {
        memcpy(new_bits, bm->bits, old_bytes);
        kfree(bm->bits);
    }
    bm->bits      = new_bits;
    bm->bit_count = new_count;
    return OK;
}

static error_t IndexEnsureTagSlot(MemTagBitmapIndex *idx, uint16_t tag_id) {
    if (tag_id >= idx->bitmap_cap) {
        uint32_t new_cap = idx->bitmap_cap;
        while (new_cap <= tag_id) {
            uint32_t doubled = new_cap * 2;
            if (doubled <= new_cap) return ERR_NO_MEMORY;
            new_cap = doubled;
        }
        MemTagBitmap **new_arr =
            (MemTagBitmap **)kmalloc(sizeof(MemTagBitmap *) * new_cap);
        if (!new_arr) return ERR_NO_MEMORY;
        memcpy(new_arr, idx->bitmaps, sizeof(MemTagBitmap *) * idx->bitmap_cap);
        memset(new_arr + idx->bitmap_cap, 0,
               sizeof(MemTagBitmap *) * (new_cap - idx->bitmap_cap));
        kfree(idx->bitmaps);
        idx->bitmaps    = new_arr;
        idx->bitmap_cap = new_cap;
    }
    if (!idx->bitmaps[tag_id]) {
        MemTagBitmap *bm = (MemTagBitmap *)kmalloc(sizeof(MemTagBitmap));
        if (!bm) return ERR_NO_MEMORY;
        bm->bits = NULL;
        bm->bit_count = 0;
        bm->set_count = 0;
        idx->bitmaps[tag_id] = bm;
    }
    return OK;
}

/* ─── Cache (caller holds idx->lock) ─────────────────────────────────── */

static void CacheClearEntry(MemTagQueryCacheEntry *e) {
    if (e->result_ids) { kfree(e->result_ids); e->result_ids = NULL; }
    if (e->tag_key)    { kfree(e->tag_key);    e->tag_key    = NULL; }
    e->result_count = 0;
    e->tag_count    = 0;
    e->used         = 0;
    e->gen_snapshot = 0;
    e->key_hash     = 0;
    e->query_type   = 0;
    e->last_used    = 0;
}

static void InvalidateCacheUnlocked(MemTagBitmapIndex *idx) {
    for (int i = 0; i < MEMTAG_QUERY_CACHE_SLOTS; i++)
        CacheClearEntry(&idx->cache[i]);
}

/* Stable-sort uint16 ascending (tag_ids may not be sorted at API boundary). */
static void SortTags(uint16_t *a, uint16_t n) {
    for (uint16_t i = 1; i < n; i++) {
        uint16_t key = a[i];
        int j = (int)i - 1;
        while (j >= 0 && a[j] > key) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

static uint64_t CacheHash(const uint16_t *sorted_tags, uint16_t count,
                           uint8_t query_type) {
    /* hash header (count + type) then tag_ids — same idea as TagFS */
    uint64_t h = 0xcbf29ce484222325ULL;
    uint8_t  hdr[3] = { (uint8_t)(count & 0xFF), (uint8_t)(count >> 8), query_type };
    for (size_t i = 0; i < sizeof(hdr); i++) {
        h ^= hdr[i];
        h *= 0x100000001b3ULL;
    }
    h ^= Fnv1a64(sorted_tags, count * sizeof(uint16_t));
    return h;
}

/* Look up cache. On hit, writes up to max into out, updates LRU, returns
 * actual result count. On miss, returns SIZE_MAX (sentinel). */
#define MEMTAG_CACHE_MISS  ((size_t)-1)

static size_t CacheLookup(MemTagBitmapIndex *idx, uint64_t hash,
                           const uint16_t *sorted_tags, uint16_t count,
                           uint8_t query_type,
                           uint32_t *out, size_t max) {
    for (int i = 0; i < MEMTAG_QUERY_CACHE_SLOTS; i++) {
        MemTagQueryCacheEntry *e = &idx->cache[i];
        if (!e->used) continue;
        if (e->gen_snapshot != idx->generation) continue;
        if (e->key_hash != hash) continue;
        if (e->tag_count != count) continue;
        if (e->query_type != query_type) continue;
        if (count > 0 && memcmp(e->tag_key, sorted_tags,
                                 count * sizeof(uint16_t)) != 0) continue;
        /* Hit */
        size_t n = e->result_count < max ? e->result_count : max;
        if (n > 0) memcpy(out, e->result_ids, n * sizeof(uint32_t));
        e->last_used = ++idx->cache_counter;
        idx->cache_hits++;
        return e->result_count;
    }
    idx->cache_misses++;
    return MEMTAG_CACHE_MISS;
}

static void CacheStore(MemTagBitmapIndex *idx, uint64_t hash,
                        const uint16_t *sorted_tags, uint16_t count,
                        uint8_t query_type,
                        const uint32_t *result, uint32_t result_count) {
    /* Find empty slot or LRU victim */
    int victim = 0;
    uint64_t oldest = ~0ULL;
    for (int i = 0; i < MEMTAG_QUERY_CACHE_SLOTS; i++) {
        if (!idx->cache[i].used) { victim = i; break; }
        if (idx->cache[i].last_used < oldest) {
            oldest = idx->cache[i].last_used;
            victim = i;
        }
    }
    MemTagQueryCacheEntry *e = &idx->cache[victim];
    CacheClearEntry(e);

    if (count > 0) {
        e->tag_key = (uint16_t *)kmalloc(sizeof(uint16_t) * count);
        if (!e->tag_key) return;
        memcpy(e->tag_key, sorted_tags, sizeof(uint16_t) * count);
    }

    if (result_count > 0) {
        e->result_ids = (uint32_t *)kmalloc(sizeof(uint32_t) * result_count);
        if (!e->result_ids) {
            if (e->tag_key) { kfree(e->tag_key); e->tag_key = NULL; }
            return;
        }
        memcpy(e->result_ids, result, sizeof(uint32_t) * result_count);
    }

    e->result_count = result_count;
    e->tag_count    = count;
    e->key_hash     = hash;
    e->query_type   = query_type;
    e->gen_snapshot = idx->generation;
    e->last_used    = ++idx->cache_counter;
    e->used         = 1;
}

/* ─── Public API ──────────────────────────────────────────────────────── */

error_t MemTagBitmapInit(MemTagBitmapIndex *idx,
                          uint32_t initial_tag_cap, uint32_t initial_region_cap) {
    if (!idx) return ERR_INVALID_ARGUMENT;
    if (!initial_tag_cap)    initial_tag_cap    = MEMTAG_BITMAP_INITIAL_TAGS;
    if (!initial_region_cap) initial_region_cap = MEMTAG_BITMAP_INITIAL_REGS;

    idx->bitmaps = (MemTagBitmap **)kmalloc(sizeof(MemTagBitmap *) * initial_tag_cap);
    if (!idx->bitmaps) return ERR_NO_MEMORY;
    memset(idx->bitmaps, 0, sizeof(MemTagBitmap *) * initial_tag_cap);

    idx->bitmap_cap     = initial_tag_cap;
    idx->region_cap     = initial_region_cap;
    idx->max_region_id  = 0;
    idx->generation     = 0;
    idx->cache_counter  = 0;
    idx->cache_hits     = 0;
    idx->cache_misses   = 0;
    memset(idx->cache, 0, sizeof(idx->cache));

    spinlock_init(&idx->lock);
    debug_printf("[MEMTAG/BMP] Init: tag_cap=%u region_cap=%u cache_slots=%u\n",
                 initial_tag_cap, initial_region_cap, MEMTAG_QUERY_CACHE_SLOTS);
    return OK;
}

void MemTagBitmapShutdown(MemTagBitmapIndex *idx) {
    if (!idx) return;
    for (uint32_t i = 0; i < idx->bitmap_cap; i++) {
        if (idx->bitmaps[i]) {
            if (idx->bitmaps[i]->bits) kfree(idx->bitmaps[i]->bits);
            kfree(idx->bitmaps[i]);
        }
    }
    kfree(idx->bitmaps);
    InvalidateCacheUnlocked(idx);
    idx->bitmaps    = NULL;
    idx->bitmap_cap = 0;
}

error_t MemTagBitmapSet(MemTagBitmapIndex *idx,
                         uint16_t tag_id, uint32_t region_id) {
    if (!idx) return ERR_INVALID_ARGUMENT;

    spin_lock(&idx->lock);
    error_t e = IndexEnsureTagSlot(idx, tag_id);
    if (e != OK) { spin_unlock(&idx->lock); return e; }

    MemTagBitmap *bm = idx->bitmaps[tag_id];
    e = BitmapEnsureCapacity(bm, region_id);
    if (e != OK) { spin_unlock(&idx->lock); return e; }

    if (!BitsTest(bm->bits, region_id)) {
        BitsSet(bm->bits, region_id);
        bm->set_count++;
    }
    if (region_id > idx->max_region_id) idx->max_region_id = region_id;
    idx->generation++;
    spin_unlock(&idx->lock);
    return OK;
}

error_t MemTagBitmapClear(MemTagBitmapIndex *idx,
                           uint16_t tag_id, uint32_t region_id) {
    if (!idx) return ERR_INVALID_ARGUMENT;

    spin_lock(&idx->lock);
    if (tag_id >= idx->bitmap_cap || !idx->bitmaps[tag_id]) {
        spin_unlock(&idx->lock);
        return OK;  /* never set */
    }
    MemTagBitmap *bm = idx->bitmaps[tag_id];
    if (region_id < bm->bit_count && BitsTest(bm->bits, region_id)) {
        BitsClear(bm->bits, region_id);
        if (bm->set_count > 0) bm->set_count--;
    }
    idx->generation++;
    spin_unlock(&idx->lock);
    return OK;
}

void MemTagBitmapRemoveRegion(MemTagBitmapIndex *idx, uint32_t region_id) {
    if (!idx) return;
    spin_lock(&idx->lock);
    for (uint32_t t = 0; t < idx->bitmap_cap; t++) {
        MemTagBitmap *bm = idx->bitmaps[t];
        if (!bm || region_id >= bm->bit_count) continue;
        if (BitsTest(bm->bits, region_id)) {
            BitsClear(bm->bits, region_id);
            if (bm->set_count > 0) bm->set_count--;
        }
    }
    idx->generation++;
    spin_unlock(&idx->lock);
}

bool MemTagBitmapHas(MemTagBitmapIndex *idx,
                      uint16_t tag_id, uint32_t region_id) {
    if (!idx) return false;
    spin_lock(&idx->lock);
    bool result = false;
    if (tag_id < idx->bitmap_cap && idx->bitmaps[tag_id]) {
        MemTagBitmap *bm = idx->bitmaps[tag_id];
        if (region_id < bm->bit_count)
            result = BitsTest(bm->bits, region_id);
    }
    spin_unlock(&idx->lock);
    return result;
}

/* ─── Query engines ──────────────────────────────────────────────────── */

/* Scan workspace bitmap and emit set bits as region_ids into out[]. */
static size_t EmitFromBitmap(const uint8_t *bits, uint32_t bit_count,
                              uint32_t *out, size_t max) {
    size_t n = 0;
    /* Walk 8 bytes at a time, then trailing bytes */
    uint32_t word_count = bit_count / 64;
    const uint64_t *words = (const uint64_t *)bits;
    for (uint32_t w = 0; w < word_count && n < max; w++) {
        uint64_t v = words[w];
        while (v && n < max) {
            int bit = __builtin_ctzll(v);
            uint32_t region_id = w * 64 + (uint32_t)bit;
            out[n++] = region_id;
            v &= v - 1;
        }
    }
    /* Trailing bits in the last partial word */
    for (uint32_t b = word_count * 64; b < bit_count && n < max; b++) {
        if (BitsTest(bits, b)) out[n++] = b;
    }
    return n;
}

/* AND-fold required[*]; if no required tags, return all-zero workspace
 * (caller must seed differently). Returns workspace bit_count. */
static uint32_t AndFoldRequired(MemTagBitmapIndex *idx,
                                 const uint16_t *required, uint16_t n_req,
                                 uint8_t *workspace, uint32_t workspace_cap_bits) {
    /* Initialize workspace from FIRST required tag's bitmap. */
    if (n_req == 0) {
        memset(workspace, 0, BitsBytes(workspace_cap_bits));
        return 0;
    }

    uint16_t first = required[0];
    if (first >= idx->bitmap_cap || !idx->bitmaps[first]) {
        memset(workspace, 0, BitsBytes(workspace_cap_bits));
        return 0;
    }
    MemTagBitmap *bm0 = idx->bitmaps[first];
    uint32_t use_bits = bm0->bit_count;
    if (use_bits > workspace_cap_bits) use_bits = workspace_cap_bits;
    size_t use_bytes = BitsBytes(use_bits);
    memcpy(workspace, bm0->bits, use_bytes);
    /* zero any trailing bits in the last byte beyond use_bits */
    if (use_bits & 7) {
        uint8_t mask = (uint8_t)((1u << (use_bits & 7)) - 1);
        workspace[use_bytes - 1] &= mask;
    }
    /* zero workspace beyond use_bytes */
    if (BitsBytes(workspace_cap_bits) > use_bytes) {
        memset(workspace + use_bytes, 0,
               BitsBytes(workspace_cap_bits) - use_bytes);
    }

    /* AND in each subsequent required tag */
    for (uint16_t i = 1; i < n_req; i++) {
        uint16_t t = required[i];
        if (t >= idx->bitmap_cap || !idx->bitmaps[t]) {
            memset(workspace, 0, BitsBytes(workspace_cap_bits));
            return 0;
        }
        MemTagBitmap *bm = idx->bitmaps[t];
        size_t cmp_bytes = BitsBytes(use_bits);
        size_t bm_bytes  = BitsBytes(bm->bit_count);
        size_t common    = cmp_bytes < bm_bytes ? cmp_bytes : bm_bytes;
        for (size_t b = 0; b < common; b++) workspace[b] &= bm->bits[b];
        /* zero workspace bits beyond bm's range */
        for (size_t b = common; b < cmp_bytes; b++) workspace[b] = 0;
        if (bm->bit_count < use_bits) use_bits = bm->bit_count;
    }
    return use_bits;
}

/* OR-fold "any" into workspace; intersects with existing workspace.
 * If n_any == 0, no-op (any-clause ignored). */
static void IntersectAny(MemTagBitmapIndex *idx,
                          const uint16_t *any, uint16_t n_any,
                          uint8_t *workspace, uint32_t workspace_bits) {
    if (n_any == 0) return;
    size_t bytes = BitsBytes(workspace_bits);
    uint8_t *any_mask = (uint8_t *)kmalloc(bytes);
    if (!any_mask) return;
    memset(any_mask, 0, bytes);
    for (uint16_t i = 0; i < n_any; i++) {
        uint16_t t = any[i];
        if (t >= idx->bitmap_cap || !idx->bitmaps[t]) continue;
        MemTagBitmap *bm = idx->bitmaps[t];
        size_t bm_bytes = BitsBytes(bm->bit_count);
        size_t common   = bm_bytes < bytes ? bm_bytes : bytes;
        for (size_t b = 0; b < common; b++) any_mask[b] |= bm->bits[b];
    }
    for (size_t b = 0; b < bytes; b++) workspace[b] &= any_mask[b];
    kfree(any_mask);
}

/* AND-NOT excluded tags from workspace. */
static void SubtractExcluded(MemTagBitmapIndex *idx,
                              const uint16_t *excl, uint16_t n_excl,
                              uint8_t *workspace, uint32_t workspace_bits) {
    size_t bytes = BitsBytes(workspace_bits);
    for (uint16_t i = 0; i < n_excl; i++) {
        uint16_t t = excl[i];
        if (t >= idx->bitmap_cap || !idx->bitmaps[t]) continue;
        MemTagBitmap *bm = idx->bitmaps[t];
        size_t bm_bytes = BitsBytes(bm->bit_count);
        size_t common   = bm_bytes < bytes ? bm_bytes : bytes;
        for (size_t b = 0; b < common; b++) workspace[b] &= (uint8_t)~bm->bits[b];
    }
}

static size_t QueryCore(MemTagBitmapIndex *idx,
                         const uint16_t *required, uint16_t n_req,
                         const uint16_t *any,      uint16_t n_any,
                         const uint16_t *excl,     uint16_t n_excl,
                         uint8_t query_type,
                         uint32_t *out, size_t max) {
    if (!idx || !out || !max) return 0;

    /* Build cache key from sorted required+any+excluded tag arrays.
     * For pure AND query we just hash sorted required[]. For OR we hash
     * sorted any[]. For MIXED we hash all three concatenated. */
    uint16_t key_tags_buf[64];
    uint16_t key_n = 0;
    if (query_type == MEMTAG_QUERY_TYPE_AND) {
        for (uint16_t i = 0; i < n_req && key_n < 64; i++)
            key_tags_buf[key_n++] = required[i];
    } else if (query_type == MEMTAG_QUERY_TYPE_OR) {
        for (uint16_t i = 0; i < n_any && key_n < 64; i++)
            key_tags_buf[key_n++] = any[i];
    } else {
        for (uint16_t i = 0; i < n_req && key_n < 64; i++)
            key_tags_buf[key_n++] = required[i];
        for (uint16_t i = 0; i < n_any && key_n < 64; i++)
            key_tags_buf[key_n++] = any[i];
        for (uint16_t i = 0; i < n_excl && key_n < 64; i++)
            key_tags_buf[key_n++] = excl[i];
    }
    SortTags(key_tags_buf, key_n);
    uint64_t hash = CacheHash(key_tags_buf, key_n, query_type);

    spin_lock(&idx->lock);
    size_t cached = CacheLookup(idx, hash, key_tags_buf, key_n, query_type, out, max);
    if (cached != MEMTAG_CACHE_MISS) {
        spin_unlock(&idx->lock);
        return cached;
    }

    /* Cache miss → compute. */
    uint32_t workspace_bits = idx->max_region_id + 1;
    if (workspace_bits < idx->region_cap) workspace_bits = idx->region_cap;
    if (workspace_bits == 0) {
        spin_unlock(&idx->lock);
        return 0;
    }

    size_t ws_bytes = BitsBytes(workspace_bits);
    uint8_t *workspace = (uint8_t *)kmalloc(ws_bytes);
    if (!workspace) {
        spin_unlock(&idx->lock);
        return 0;
    }

    uint32_t effective_bits;
    if (query_type == MEMTAG_QUERY_TYPE_OR) {
        memset(workspace, 0, ws_bytes);
        for (uint16_t i = 0; i < n_any; i++) {
            uint16_t t = any[i];
            if (t >= idx->bitmap_cap || !idx->bitmaps[t]) continue;
            MemTagBitmap *bm = idx->bitmaps[t];
            size_t bm_bytes = BitsBytes(bm->bit_count);
            size_t common   = bm_bytes < ws_bytes ? bm_bytes : ws_bytes;
            for (size_t b = 0; b < common; b++) workspace[b] |= bm->bits[b];
        }
        effective_bits = workspace_bits;
    } else {
        effective_bits = AndFoldRequired(idx, required, n_req,
                                          workspace, workspace_bits);
        if (n_any > 0)
            IntersectAny(idx, any, n_any, workspace, effective_bits);
        if (n_excl > 0)
            SubtractExcluded(idx, excl, n_excl, workspace, effective_bits);
    }

    size_t count = EmitFromBitmap(workspace, effective_bits, out, max);

    /* Store full result in cache (may be larger than `max`; for cache we
     * store what we have — partial truncation acceptable per TagFS pattern). */
    CacheStore(idx, hash, key_tags_buf, key_n, query_type, out, (uint32_t)count);

    kfree(workspace);
    spin_unlock(&idx->lock);
    return count;
}

size_t MemTagBitmapQueryAnd(MemTagBitmapIndex *idx,
                             const uint16_t *tag_ids, uint16_t tag_count,
                             uint32_t *out, size_t max_results) {
    return QueryCore(idx, tag_ids, tag_count, NULL, 0, NULL, 0,
                     MEMTAG_QUERY_TYPE_AND, out, max_results);
}

size_t MemTagBitmapQueryOr(MemTagBitmapIndex *idx,
                            const uint16_t *tag_ids, uint16_t tag_count,
                            uint32_t *out, size_t max_results) {
    return QueryCore(idx, NULL, 0, tag_ids, tag_count, NULL, 0,
                     MEMTAG_QUERY_TYPE_OR, out, max_results);
}

size_t MemTagBitmapQueryMixed(MemTagBitmapIndex *idx,
                               const uint16_t *required, uint16_t n_req,
                               const uint16_t *any,      uint16_t n_any,
                               const uint16_t *excluded, uint16_t n_excl,
                               uint32_t *out, size_t max_results) {
    return QueryCore(idx, required, n_req, any, n_any, excluded, n_excl,
                     MEMTAG_QUERY_TYPE_MIXED, out, max_results);
}

/* ─── Stats ───────────────────────────────────────────────────────────── */

uint64_t MemTagBitmapGeneration(MemTagBitmapIndex *idx) {
    return idx ? idx->generation : 0;
}

uint64_t MemTagBitmapCacheHits(MemTagBitmapIndex *idx) {
    return idx ? idx->cache_hits : 0;
}

uint64_t MemTagBitmapCacheMisses(MemTagBitmapIndex *idx) {
    return idx ? idx->cache_misses : 0;
}

void MemTagBitmapInvalidateCache(MemTagBitmapIndex *idx) {
    if (!idx) return;
    spin_lock(&idx->lock);
    InvalidateCacheUnlocked(idx);
    idx->generation++;
    spin_unlock(&idx->lock);
}

void MemTagBitmapDump(MemTagBitmapIndex *idx) {
    if (!idx) return;
    spin_lock(&idx->lock);
    uint32_t active_tags = 0, total_bits = 0;
    for (uint32_t i = 0; i < idx->bitmap_cap; i++) {
        if (idx->bitmaps[i]) {
            active_tags++;
            total_bits += idx->bitmaps[i]->set_count;
        }
    }
    debug_printf("[MEMTAG/BMP] tag_cap=%u active=%u total_bits=%u gen=%lu hits=%lu misses=%lu\n",
                 idx->bitmap_cap, active_tags, total_bits,
                 (unsigned long)idx->generation,
                 (unsigned long)idx->cache_hits,
                 (unsigned long)idx->cache_misses);
    spin_unlock(&idx->lock);
}
