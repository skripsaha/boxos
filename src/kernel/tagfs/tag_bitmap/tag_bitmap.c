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

    spin_unlock(&idx->lock);
}

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

    uint32_t result_size = (idx->max_file_id + 8) / 8;

    uint8_t* result_bitmap = kmalloc(result_size);
    if (!result_bitmap) {
        spin_unlock(&idx->lock);
        return -1;
    }
    memset(result_bitmap, 0, result_size);

    bool first = true;

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
            for (uint32_t b = words * 8; b < copy_len; b++) {
                result_bitmap[b] &= bm->bits[b];
            }
            for (uint32_t b = copy_len; b < result_size; b++) {
                result_bitmap[b] = 0;
            }
        }
    }

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
            for (uint32_t b = words * 8; b < result_size; b++) {
                result_bitmap[b] &= temp[b];
            }
        }

        kfree(temp);
    }

    if (first) {
        kfree(result_bitmap);
        spin_unlock(&idx->lock);
        return 0;
    }

    uint32_t count = 0;
    for (uint32_t fid = 1; fid <= idx->max_file_id && count < max_results; fid++) {
        uint32_t byte_idx = fid / 8;
        uint32_t bit_idx  = fid % 8;
        if (byte_idx < result_size && (result_bitmap[byte_idx] & (1 << bit_idx))) {
            out_file_ids[count++] = fid;
        }
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
