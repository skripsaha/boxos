#include "tag_bitmap.h"
#include "klib.h"

// ---------------------------------------------------------------------------
// Internal helpers (no locking)
// ---------------------------------------------------------------------------

static void ensure_bitmap_capacity(TagBitmapIndex* idx, uint16_t tag_id) {
    if ((uint32_t)tag_id < idx->bitmap_capacity) {
        return;
    }

    uint32_t new_cap = idx->bitmap_capacity ? idx->bitmap_capacity : 64;
    while (new_cap <= (uint32_t)tag_id) {
        new_cap *= 2;
    }

    TagBitmap** new_bitmaps = kmalloc(sizeof(TagBitmap*) * new_cap);
    if (!new_bitmaps) {
        debug_printf("[TagBitmap] ensure_bitmap_capacity: kmalloc failed\n");
        return;
    }

    if (idx->bitmaps) {
        memcpy(new_bitmaps, idx->bitmaps, sizeof(TagBitmap*) * idx->bitmap_capacity);
        kfree(idx->bitmaps);
    }
    memset(new_bitmaps + idx->bitmap_capacity, 0,
           sizeof(TagBitmap*) * (new_cap - idx->bitmap_capacity));

    idx->bitmaps = new_bitmaps;
    idx->bitmap_capacity = new_cap;
}

static void ensure_file_capacity(TagBitmapIndex* idx, uint32_t file_id) {
    if (file_id < idx->file_capacity) {
        return;
    }

    uint32_t new_cap = idx->file_capacity ? idx->file_capacity : 64;
    while (new_cap <= file_id) {
        new_cap *= 2;
    }

    TagIdList* new_list = kmalloc(sizeof(TagIdList) * new_cap);
    if (!new_list) {
        debug_printf("[TagBitmap] ensure_file_capacity: kmalloc failed\n");
        return;
    }

    if (idx->file_to_tags) {
        memcpy(new_list, idx->file_to_tags, sizeof(TagIdList) * idx->file_capacity);
        kfree(idx->file_to_tags);
    }
    memset(new_list + idx->file_capacity, 0,
           sizeof(TagIdList) * (new_cap - idx->file_capacity));

    idx->file_to_tags = new_list;
    idx->file_capacity = new_cap;
}

static void ensure_bitmap_width(TagBitmap* bm, uint32_t new_max) {
    if (bm->bit_count >= new_max + 1) {
        return;
    }

    uint32_t old_size = (bm->bit_count + 7) / 8;
    uint32_t new_size = (new_max + 8) / 8;

    uint8_t* new_bits = kmalloc(new_size);
    if (!new_bits) {
        debug_printf("[TagBitmap] ensure_bitmap_width: kmalloc failed\n");
        return;
    }

    if (bm->bits && old_size > 0) {
        memcpy(new_bits, bm->bits, old_size);
    }
    memset(new_bits + old_size, 0, new_size - old_size);

    if (bm->bits) {
        kfree(bm->bits);
    }
    bm->bits = new_bits;
    bm->bit_count = new_size * 8;
}

static void reverse_index_add(TagBitmapIndex* idx, uint32_t file_id, uint16_t tag_id) {
    TagIdList* list = &idx->file_to_tags[file_id];

    for (uint16_t i = 0; i < list->count; i++) {
        if (list->ids[i] == tag_id) {
            return;
        }
    }

    if (list->count >= list->capacity) {
        uint16_t new_cap = list->capacity ? (uint16_t)(list->capacity * 2) : 4;
        uint16_t* new_ids = kmalloc(sizeof(uint16_t) * new_cap);
        if (!new_ids) {
            debug_printf("[TagBitmap] reverse_index_add: kmalloc failed\n");
            return;
        }
        if (list->ids) {
            memcpy(new_ids, list->ids, sizeof(uint16_t) * list->count);
            kfree(list->ids);
        }
        list->ids = new_ids;
        list->capacity = new_cap;
    }

    list->ids[list->count++] = tag_id;
}

static void reverse_index_remove(TagBitmapIndex* idx, uint32_t file_id, uint16_t tag_id) {
    if (file_id >= idx->file_capacity) {
        return;
    }

    TagIdList* list = &idx->file_to_tags[file_id];
    for (uint16_t i = 0; i < list->count; i++) {
        if (list->ids[i] == tag_id) {
            list->ids[i] = list->ids[list->count - 1];
            list->count--;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

TagBitmapIndex* tag_bitmap_create(uint32_t initial_tag_cap, uint32_t initial_file_cap) {
    TagBitmapIndex* idx = kmalloc(sizeof(TagBitmapIndex));
    if (!idx) {
        return NULL;
    }
    memset(idx, 0, sizeof(TagBitmapIndex));

    idx->bitmaps = kmalloc(sizeof(TagBitmap*) * initial_tag_cap);
    if (!idx->bitmaps) {
        kfree(idx);
        return NULL;
    }
    memset(idx->bitmaps, 0, sizeof(TagBitmap*) * initial_tag_cap);
    idx->bitmap_capacity = initial_tag_cap;

    idx->file_to_tags = kmalloc(sizeof(TagIdList) * initial_file_cap);
    if (!idx->file_to_tags) {
        kfree(idx->bitmaps);
        kfree(idx);
        return NULL;
    }
    memset(idx->file_to_tags, 0, sizeof(TagIdList) * initial_file_cap);
    idx->file_capacity = initial_file_cap;

    idx->max_file_id = 0;
    idx->generation  = 0;
    memset(idx->cache, 0, sizeof(idx->cache));
    spinlock_init(&idx->lock);

    return idx;
}

void tag_bitmap_destroy(TagBitmapIndex* idx) {
    if (!idx) {
        return;
    }

    for (uint32_t i = 0; i < idx->bitmap_capacity; i++) {
        if (idx->bitmaps[i]) {
            if (idx->bitmaps[i]->bits) {
                kfree(idx->bitmaps[i]->bits);
            }
            kfree(idx->bitmaps[i]);
        }
    }
    kfree(idx->bitmaps);

    for (uint32_t i = 0; i < idx->file_capacity; i++) {
        if (idx->file_to_tags[i].ids) {
            kfree(idx->file_to_tags[i].ids);
        }
    }
    kfree(idx->file_to_tags);

    for (int i = 0; i < QUERY_CACHE_SLOTS; i++) {
        if (idx->cache[i].file_ids) kfree(idx->cache[i].file_ids);
        if (idx->cache[i].tag_key)  kfree(idx->cache[i].tag_key);
    }

    kfree(idx);
}

int tag_bitmap_set(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id) {
    if (!idx) {
        return -1;
    }

    spin_lock(&idx->lock);

    ensure_bitmap_capacity(idx, tag_id);
    ensure_file_capacity(idx, file_id);

    if (!idx->bitmaps || (uint32_t)tag_id >= idx->bitmap_capacity) {
        spin_unlock(&idx->lock);
        return -1;
    }

    if (!idx->file_to_tags || file_id >= idx->file_capacity) {
        spin_unlock(&idx->lock);
        return -1;
    }

    if (!idx->bitmaps[tag_id]) {
        TagBitmap* bm = kmalloc(sizeof(TagBitmap));
        if (!bm) {
            spin_unlock(&idx->lock);
            return -1;
        }
        memset(bm, 0, sizeof(TagBitmap));
        idx->bitmaps[tag_id] = bm;
    }

    uint32_t needed_max = (file_id > idx->max_file_id) ? file_id : idx->max_file_id;
    ensure_bitmap_width(idx->bitmaps[tag_id], needed_max);

    if (!idx->bitmaps[tag_id]->bits) {
        spin_unlock(&idx->lock);
        return -1;
    }

    uint32_t byte_idx = file_id / 8;
    uint32_t bit_idx  = file_id % 8;

    if (!(idx->bitmaps[tag_id]->bits[byte_idx] & (1 << bit_idx))) {
        idx->bitmaps[tag_id]->bits[byte_idx] |= (uint8_t)(1 << bit_idx);
        idx->bitmaps[tag_id]->file_count++;
    }

    if (file_id > idx->max_file_id) {
        idx->max_file_id = file_id;
    }

    reverse_index_add(idx, file_id, tag_id);
    idx->generation++;

    spin_unlock(&idx->lock);
    return 0;
}

int tag_bitmap_clear(TagBitmapIndex* idx, uint16_t tag_id, uint32_t file_id) {
    if (!idx) {
        return -1;
    }

    spin_lock(&idx->lock);

    if ((uint32_t)tag_id >= idx->bitmap_capacity || !idx->bitmaps[tag_id]) {
        spin_unlock(&idx->lock);
        return -1;
    }

    TagBitmap* bm = idx->bitmaps[tag_id];
    uint32_t byte_idx = file_id / 8;
    uint32_t bit_idx  = file_id % 8;

    if (byte_idx < (bm->bit_count + 7) / 8 && (bm->bits[byte_idx] & (1 << bit_idx))) {
        bm->bits[byte_idx] &= (uint8_t)(~(1 << bit_idx));
        if (bm->file_count > 0) {
            bm->file_count--;
        }
    }

    reverse_index_remove(idx, file_id, tag_id);
    idx->generation++;

    spin_unlock(&idx->lock);
    return 0;
}

void tag_bitmap_remove_file(TagBitmapIndex* idx, uint32_t file_id) {
    if (!idx || file_id >= idx->file_capacity) {
        return;
    }

    spin_lock(&idx->lock);

    TagIdList* list = &idx->file_to_tags[file_id];

    for (uint16_t i = 0; i < list->count; i++) {
        uint16_t tid = list->ids[i];
        if ((uint32_t)tid < idx->bitmap_capacity && idx->bitmaps[tid]) {
            TagBitmap* bm = idx->bitmaps[tid];
            uint32_t byte_idx = file_id / 8;
            uint32_t bit_idx  = file_id % 8;
            uint32_t bm_bytes = (bm->bit_count + 7) / 8;
            if (byte_idx < bm_bytes && (bm->bits[byte_idx] & (1 << bit_idx))) {
                bm->bits[byte_idx] &= (uint8_t)(~(1 << bit_idx));
                if (bm->file_count > 0) {
                    bm->file_count--;
                }
            }
        }
    }

    if (list->ids) {
        kfree(list->ids);
        list->ids = NULL;
    }
    list->count    = 0;
    list->capacity = 0;
    idx->generation++;

    spin_unlock(&idx->lock);
}

// ---------------------------------------------------------------------------
// Query cache helpers (called under lock)
// ---------------------------------------------------------------------------

static uint32_t query_hash(const uint16_t* tag_ids, uint32_t tag_count,
                           TagKeyGroup** groups, uint32_t group_count) {
    uint32_t h = 0x811c9dc5;  // FNV-1a
    for (uint32_t i = 0; i < tag_count; i++) {
        h ^= tag_ids[i];
        h *= 0x01000193;
    }
    for (uint32_t g = 0; g < group_count; g++) {
        if (!groups[g]) continue;
        for (uint32_t k = 0; k < groups[g]->count; k++) {
            h ^= groups[g]->tag_ids[k] | 0x80000000;
            h *= 0x01000193;
        }
    }
    return h;
}

static QueryCacheEntry* cache_lookup(TagBitmapIndex* idx, uint32_t hash,
                                      const uint16_t* tag_ids, uint32_t tag_count) {
    for (int i = 0; i < QUERY_CACHE_SLOTS; i++) {
        QueryCacheEntry* e = &idx->cache[i];
        if (e->hash == hash && e->generation == idx->generation &&
            e->tag_count == tag_count && e->tag_key && e->file_ids) {
            bool match = true;
            for (uint16_t j = 0; j < tag_count; j++) {
                if (e->tag_key[j] != tag_ids[j]) { match = false; break; }
            }
            if (match) return e;
        }
    }
    return NULL;
}

static void cache_store(TagBitmapIndex* idx, uint32_t hash,
                        const uint16_t* tag_ids, uint32_t tag_count,
                        const uint32_t* file_ids, uint32_t count) {
    // Find empty or oldest slot (simple: use hash % SLOTS)
    int slot = (int)(hash % QUERY_CACHE_SLOTS);
    QueryCacheEntry* e = &idx->cache[slot];

    if (e->file_ids) kfree(e->file_ids);
    if (e->tag_key)  kfree(e->tag_key);

    e->hash       = hash;
    e->generation = idx->generation;
    e->count      = count;
    e->tag_count  = (uint16_t)tag_count;

    e->file_ids = kmalloc(sizeof(uint32_t) * (count > 0 ? count : 1));
    e->tag_key  = kmalloc(sizeof(uint16_t) * (tag_count > 0 ? tag_count : 1));

    if (e->file_ids && count > 0)
        memcpy(e->file_ids, file_ids, sizeof(uint32_t) * count);
    if (e->tag_key && tag_count > 0)
        memcpy(e->tag_key, tag_ids, sizeof(uint16_t) * tag_count);
}

// ---------------------------------------------------------------------------
// Fast bit extraction: use CTZ to skip empty words
// ---------------------------------------------------------------------------

static uint32_t extract_set_bits(const uint8_t* bitmap, uint32_t byte_count,
                                  uint32_t max_file_id,
                                  uint32_t* out_ids, uint32_t max_results) {
    uint32_t count = 0;
    const uint64_t* words = (const uint64_t*)bitmap;
    uint32_t word_count = byte_count / 8;

    // 64-bit word scan with CTZ
    for (uint32_t w = 0; w < word_count && count < max_results; w++) {
        uint64_t word = words[w];
        while (word != 0 && count < max_results) {
            uint32_t bit = (uint32_t)__builtin_ctzll(word);
            uint32_t fid = w * 64 + bit;
            if (fid >= 1 && fid <= max_file_id) {
                out_ids[count++] = fid;
            }
            word &= word - 1;  // clear lowest set bit
        }
    }

    // Handle remaining bytes (if byte_count not aligned to 8)
    for (uint32_t b = word_count * 8; b < byte_count && count < max_results; b++) {
        uint8_t byte = bitmap[b];
        while (byte != 0 && count < max_results) {
            uint32_t bit = (uint32_t)__builtin_ctz((unsigned)byte);
            uint32_t fid = b * 8 + bit;
            if (fid >= 1 && fid <= max_file_id) {
                out_ids[count++] = fid;
            }
            byte &= (uint8_t)(byte - 1);
        }
    }

    return count;
}

// ---------------------------------------------------------------------------
// Check if bitmap is all-zero (early termination)
// ---------------------------------------------------------------------------

static bool bitmap_is_empty(const uint8_t* bitmap, uint32_t byte_count) {
    const uint64_t* words = (const uint64_t*)bitmap;
    uint32_t word_count = byte_count / 8;
    for (uint32_t w = 0; w < word_count; w++) {
        if (words[w] != 0) return false;
    }
    for (uint32_t b = word_count * 8; b < byte_count; b++) {
        if (bitmap[b] != 0) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Optimized query: cache + 64-bit AND + CTZ extraction + early termination
// ---------------------------------------------------------------------------

int tag_bitmap_query(TagBitmapIndex* idx,
                     const uint16_t* tag_ids, uint32_t tag_count,
                     TagKeyGroup** groups, uint32_t group_count,
                     uint32_t* out_file_ids, uint32_t max_results) {
    if (!idx || !out_file_ids) {
        return -1;
    }

    spin_lock(&idx->lock);

    if (idx->max_file_id == 0) {
        spin_unlock(&idx->lock);
        return 0;
    }

    // --- Cache lookup (only for simple tag-only queries) ---
    uint32_t hash = 0;
    if (tag_count > 0 && group_count == 0) {
        hash = query_hash(tag_ids, tag_count, groups, group_count);
        QueryCacheEntry* cached = cache_lookup(idx, hash, tag_ids, tag_count);
        if (cached) {
            uint32_t copy = cached->count < max_results ? cached->count : max_results;
            if (copy > 0) memcpy(out_file_ids, cached->file_ids, sizeof(uint32_t) * copy);
            spin_unlock(&idx->lock);
            return (int)copy;
        }
    }

    // --- Bitmap intersection ---
    uint32_t result_size = (idx->max_file_id + 8) / 8;
    // Align to 8 bytes for safe uint64_t access
    result_size = (result_size + 7) & ~7u;

    uint8_t* result_bitmap = kmalloc(result_size);
    if (!result_bitmap) {
        spin_unlock(&idx->lock);
        return -1;
    }
    memset(result_bitmap, 0, result_size);

    bool first = true;

    // AND all required tags
    for (uint32_t i = 0; i < tag_count; i++) {
        uint16_t tid = tag_ids[i];
        if ((uint32_t)tid >= idx->bitmap_capacity || !idx->bitmaps[tid]) {
            kfree(result_bitmap);
            spin_unlock(&idx->lock);
            return 0;
        }

        TagBitmap* bm = idx->bitmaps[tid];
        uint32_t bm_bytes = (bm->bit_count + 7) / 8;
        uint32_t copy_len = bm_bytes < result_size ? bm_bytes : result_size;

        if (first) {
            memcpy(result_bitmap, bm->bits, copy_len);
            if (copy_len < result_size) {
                memset(result_bitmap + copy_len, 0, result_size - copy_len);
            }
            first = false;
        } else {
            uint64_t* r64 = (uint64_t*)result_bitmap;
            uint64_t* b64 = (uint64_t*)bm->bits;
            uint32_t words = copy_len / 8;
            for (uint32_t w = 0; w < words; w++) {
                r64[w] &= b64[w];
            }
            // AND remaining bytes that don't fill a full 64-bit word
            for (uint32_t b = words * 8; b < copy_len; b++) {
                result_bitmap[b] &= bm->bits[b];
            }
            for (uint32_t b = copy_len; b < result_size; b++) {
                result_bitmap[b] = 0;
            }
            // Early termination: if result is all-zero, no matches possible
            if (bitmap_is_empty(result_bitmap, result_size)) {
                kfree(result_bitmap);
                spin_unlock(&idx->lock);
                return 0;
            }
        }
    }

    // AND with each group (OR within group)
    for (uint32_t g = 0; g < group_count; g++) {
        TagKeyGroup* grp = groups[g];
        if (!grp || grp->count == 0) {
            continue;
        }

        uint8_t* temp = kmalloc(result_size);
        if (!temp) {
            kfree(result_bitmap);
            spin_unlock(&idx->lock);
            return -1;
        }
        memset(temp, 0, result_size);

        for (uint32_t k = 0; k < grp->count; k++) {
            uint16_t tid = grp->tag_ids[k];
            if ((uint32_t)tid >= idx->bitmap_capacity || !idx->bitmaps[tid]) {
                continue;
            }
            TagBitmap* bm = idx->bitmaps[tid];
            uint32_t bm_bytes = (bm->bit_count + 7) / 8;
            uint32_t or_len = bm_bytes < result_size ? bm_bytes : result_size;

            uint64_t* t64 = (uint64_t*)temp;
            uint64_t* b64 = (uint64_t*)bm->bits;
            uint32_t words = or_len / 8;
            for (uint32_t w = 0; w < words; w++) {
                t64[w] |= b64[w];
            }
            // OR remaining bytes that don't fill a full 64-bit word
            for (uint32_t b = words * 8; b < or_len; b++) {
                temp[b] |= bm->bits[b];
            }
        }

        if (first) {
            memcpy(result_bitmap, temp, result_size);
            first = false;
        } else {
            uint64_t* r64 = (uint64_t*)result_bitmap;
            uint64_t* t64 = (uint64_t*)temp;
            uint32_t words = result_size / 8;
            for (uint32_t w = 0; w < words; w++) {
                r64[w] &= t64[w];
            }
        }

        kfree(temp);

        // Early termination after group AND
        if (!first && bitmap_is_empty(result_bitmap, result_size)) {
            kfree(result_bitmap);
            spin_unlock(&idx->lock);
            return 0;
        }
    }

    if (first) {
        kfree(result_bitmap);
        spin_unlock(&idx->lock);
        return 0;
    }

    // --- Fast bit extraction with CTZ ---
    uint32_t count = extract_set_bits(result_bitmap, result_size,
                                       idx->max_file_id,
                                       out_file_ids, max_results);

    // --- Store in cache (simple tag-only queries, non-truncated only) ---
    if (tag_count > 0 && group_count == 0 && count < max_results) {
        cache_store(idx, hash, tag_ids, tag_count, out_file_ids, count);
    }

    kfree(result_bitmap);
    spin_unlock(&idx->lock);
    return (int)count;
}

int tag_bitmap_tags_for_file(TagBitmapIndex* idx, uint32_t file_id,
                              uint16_t* out_ids, uint32_t max_ids) {
    if (!idx || !out_ids) {
        return -1;
    }

    spin_lock(&idx->lock);

    if (file_id >= idx->file_capacity) {
        spin_unlock(&idx->lock);
        return 0;
    }

    TagIdList* list = &idx->file_to_tags[file_id];
    uint32_t copy_count = list->count < max_ids ? list->count : max_ids;
    if (copy_count > 0 && list->ids) {
        memcpy(out_ids, list->ids, sizeof(uint16_t) * copy_count);
    }

    spin_unlock(&idx->lock);
    return (int)copy_count;
}

int tag_bitmap_tag_count_for_file(TagBitmapIndex* idx, uint32_t file_id) {
    if (!idx) return 0;

    spin_lock(&idx->lock);

    if (file_id >= idx->file_capacity) {
        spin_unlock(&idx->lock);
        return 0;
    }

    int count = (int)idx->file_to_tags[file_id].count;
    spin_unlock(&idx->lock);
    return count;
}
