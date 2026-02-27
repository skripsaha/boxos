#include "tag_bitmap.h"
#include "klib.h"

extern void tagfs_format_tag(char* dest, const char* key, const char* value);
extern uint32_t tag_hash(const char* tag_string);

TagBitmapIndex* tag_bitmap_create(void) {
    TagBitmapIndex* index = kmalloc(sizeof(TagBitmapIndex));
    if (!index) {
        return NULL;
    }

    memset(index, 0, sizeof(TagBitmapIndex));
    return index;
}

void tag_bitmap_destroy(TagBitmapIndex* index) {
    if (!index) {
        return;
    }

    tag_bitmap_clear(index);
    kfree(index);
}

void tag_bitmap_clear(TagBitmapIndex* index) {
    if (!index) {
        return;
    }

    for (uint32_t i = 0; i < 256; i++) {
        TagBitmapEntry* entry = index->buckets[i];
        while (entry) {
            TagBitmapEntry* next = entry->next;
            kfree(entry);
            entry = next;
        }
        index->buckets[i] = NULL;
    }

    for (uint32_t file_id = 0; file_id < TAGFS_MAX_FILES; file_id++) {
        if (index->file_tags[file_id].tag_strings) {
            for (uint32_t i = 0; i < index->file_tags[file_id].count; i++) {
                if (index->file_tags[file_id].tag_strings[i]) {
                    kfree(index->file_tags[file_id].tag_strings[i]);
                }
            }
            kfree(index->file_tags[file_id].tag_strings);
            index->file_tags[file_id].tag_strings = NULL;
        }
        index->file_tags[file_id].count = 0;
    }

    index->total_tags = 0;
}

static TagBitmapEntry* find_or_create_entry(TagBitmapIndex* index, const char* tag_string) {
    uint32_t hash = tag_hash(tag_string);
    TagBitmapEntry* entry = index->buckets[hash];

    while (entry) {
        if (strcmp(entry->tag_string, tag_string) == 0) {
            return entry;
        }
        entry = entry->next;
    }

    TagBitmapEntry* new_entry = kmalloc(sizeof(TagBitmapEntry));
    if (!new_entry) {
        return NULL;
    }

    memset(new_entry, 0, sizeof(TagBitmapEntry));
    strncpy(new_entry->tag_string, tag_string, 63);
    new_entry->tag_string[63] = '\0';
    new_entry->next = index->buckets[hash];
    index->buckets[hash] = new_entry;
    index->total_tags++;

    return new_entry;
}

int tag_bitmap_add_file(TagBitmapIndex* index, const char* tag_string, uint32_t file_id) {
    if (!index || !tag_string || file_id >= TAGFS_MAX_FILES) {
        return -1;
    }

    TagBitmapEntry* entry = find_or_create_entry(index, tag_string);
    if (!entry) {
        return -1;
    }

    uint32_t byte_idx = file_id / 8;
    uint32_t bit_idx = file_id % 8;

    if (!(entry->bitmap[byte_idx] & (1 << bit_idx))) {
        entry->bitmap[byte_idx] |= (1 << bit_idx);
        entry->file_count++;

        uint32_t tag_idx = index->file_tags[file_id].count;
        char** new_tag_strings = kmalloc(sizeof(char*) * (tag_idx + 1));
        if (!new_tag_strings) {
            entry->bitmap[byte_idx] &= ~(1 << bit_idx);
            entry->file_count--;
            return -1;
        }

        if (index->file_tags[file_id].tag_strings) {
            memcpy(new_tag_strings, index->file_tags[file_id].tag_strings, sizeof(char*) * tag_idx);
            kfree(index->file_tags[file_id].tag_strings);
        }

        new_tag_strings[tag_idx] = kmalloc(64);
        if (!new_tag_strings[tag_idx]) {
            kfree(new_tag_strings);
            entry->bitmap[byte_idx] &= ~(1 << bit_idx);
            entry->file_count--;
            return -1;
        }

        strncpy(new_tag_strings[tag_idx], tag_string, 63);
        new_tag_strings[tag_idx][63] = '\0';

        index->file_tags[file_id].tag_strings = new_tag_strings;
        index->file_tags[file_id].count++;
    }

    return 0;
}

int tag_bitmap_remove_file(TagBitmapIndex* index, uint32_t file_id) {
    if (!index || file_id >= TAGFS_MAX_FILES) {
        return -1;
    }

    if (!index->file_tags[file_id].tag_strings || index->file_tags[file_id].count == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < index->file_tags[file_id].count; i++) {
        const char* tag_string = index->file_tags[file_id].tag_strings[i];
        uint32_t hash = tag_hash(tag_string);
        TagBitmapEntry* entry = index->buckets[hash];

        while (entry) {
            if (strcmp(entry->tag_string, tag_string) == 0) {
                uint32_t byte_idx = file_id / 8;
                uint32_t bit_idx = file_id % 8;

                if (entry->bitmap[byte_idx] & (1 << bit_idx)) {
                    entry->bitmap[byte_idx] &= ~(1 << bit_idx);
                    entry->file_count--;
                }
                break;
            }
            entry = entry->next;
        }

        kfree(index->file_tags[file_id].tag_strings[i]);
    }

    kfree(index->file_tags[file_id].tag_strings);
    index->file_tags[file_id].tag_strings = NULL;
    index->file_tags[file_id].count = 0;

    return 0;
}

int tag_bitmap_query(TagBitmapIndex* index, const char* query_tags[],
                     uint32_t tag_count, uint32_t* file_ids, uint32_t max_files) {
    if (!index || !query_tags || tag_count == 0 || !file_ids) {
        return -1;
    }

    uint8_t result_bitmap[TAGFS_BITMAP_SIZE];
    bool first_tag = true;

    for (uint32_t tag_idx = 0; tag_idx < tag_count; tag_idx++) {
        size_t qlen = strlen(query_tags[tag_idx]);
        bool is_wildcard = (qlen > 4 &&
            strcmp(query_tags[tag_idx] + qlen - 4, ":...") == 0);

        bool found = false;

        if (is_wildcard) {
            size_t prefix_len = qlen - 3;
            uint8_t wildcard_bitmap[TAGFS_BITMAP_SIZE];
            memset(wildcard_bitmap, 0, TAGFS_BITMAP_SIZE);
            bool any_match = false;

            for (uint32_t b = 0; b < 256; b++) {
                TagBitmapEntry* e = index->buckets[b];
                while (e) {
                    if (strncmp(e->tag_string, query_tags[tag_idx], prefix_len) == 0) {
                        for (uint32_t i = 0; i < TAGFS_BITMAP_SIZE; i++) {
                            wildcard_bitmap[i] |= e->bitmap[i];
                        }
                        any_match = true;
                    }
                    e = e->next;
                }
            }

            if (any_match) {
                if (first_tag) {
                    memcpy(result_bitmap, wildcard_bitmap, TAGFS_BITMAP_SIZE);
                    first_tag = false;
                } else {
                    for (uint32_t i = 0; i < TAGFS_BITMAP_SIZE; i++) {
                        result_bitmap[i] &= wildcard_bitmap[i];
                    }
                }
                found = true;
            }
        } else {
            uint32_t hash = tag_hash(query_tags[tag_idx]);
            TagBitmapEntry* entry = index->buckets[hash];

            while (entry) {
                if (strcmp(entry->tag_string, query_tags[tag_idx]) == 0) {
                    if (first_tag) {
                        memcpy(result_bitmap, entry->bitmap, TAGFS_BITMAP_SIZE);
                        first_tag = false;
                    } else {
                        for (uint32_t i = 0; i < TAGFS_BITMAP_SIZE; i++) {
                            result_bitmap[i] &= entry->bitmap[i];
                        }
                    }
                    found = true;
                    break;
                }
                entry = entry->next;
            }
        }

        if (!found) {
            return 0;
        }
    }

    if (first_tag) {
        return 0;
    }

    uint32_t result_count = 0;
    for (uint32_t file_id = 1; file_id < TAGFS_MAX_FILES && result_count < max_files; file_id++) {
        uint32_t byte_idx = file_id / 8;
        uint32_t bit_idx = file_id % 8;

        if (result_bitmap[byte_idx] & (1 << bit_idx)) {
            file_ids[result_count++] = file_id;
        }
    }

    return result_count;
}

bool tag_bitmap_has_tag(TagBitmapIndex* index, const char* tag_string, uint32_t file_id) {
    if (!index || !tag_string || file_id >= TAGFS_MAX_FILES) {
        return false;
    }

    uint32_t hash = tag_hash(tag_string);
    TagBitmapEntry* entry = index->buckets[hash];

    while (entry) {
        if (strcmp(entry->tag_string, tag_string) == 0) {
            uint32_t byte_idx = file_id / 8;
            uint32_t bit_idx = file_id % 8;
            return (entry->bitmap[byte_idx] & (1 << bit_idx)) != 0;
        }
        entry = entry->next;
    }

    return false;
}

int tag_bitmap_rebuild(TagBitmapIndex* index, TagFSMetadata* metadata_cache, uint32_t total_files) {
    if (!index || !metadata_cache) {
        return -1;
    }

    tag_bitmap_clear(index);

    for (uint32_t i = 0; i < total_files; i++) {
        TagFSMetadata* meta = &metadata_cache[i];

        if (!(meta->flags & TAGFS_FILE_ACTIVE)) {
            continue;
        }

        if (meta->file_id == 0) {
            debug_printf("[TagBitmap] Warning: Skipping file at index %u (file_id=0)\n", i);
            continue;
        }

        uint32_t file_id = meta->file_id;

        if (i < 3) {
            debug_printf("[TagBitmap] Rebuild: index=%u file_id=%u filename='%s' tags=%u\n",
                        i, file_id, meta->filename, meta->tag_count);
        }

        for (uint32_t tag_idx = 0; tag_idx < meta->tag_count && tag_idx < TAGFS_MAX_TAGS_PER_FILE; tag_idx++) {
            TagFSTag* tag = &meta->tags[tag_idx];
            char tag_string[64];
            tagfs_format_tag(tag_string, tag->key, tag->value);

            if (tag_bitmap_add_file(index, tag_string, file_id) != 0) {
                debug_printf("[TagBitmap] Warning: Failed to add file %u with tag '%s'\n", file_id, tag_string);
            }
        }
    }

    debug_printf("[TagBitmap] Rebuilt index with %u unique tags\n", index->total_tags);
    return 0;
}
