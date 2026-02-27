#include "tag_index.h"
#include "klib.h"

uint32_t tag_hash(const char* tag_string) {
    if (!tag_string) {
        return 0;
    }

    uint32_t hash = 5381;
    while (*tag_string) {
        hash = ((hash << 5) + hash) + (uint8_t)(*tag_string);
        tag_string++;
    }
    return hash % 256;
}

TagIndex* tag_index_create(void) {
    TagIndex* index = kmalloc(sizeof(TagIndex));
    if (!index) {
        return NULL;
    }

    memset(index, 0, sizeof(TagIndex));
    return index;
}

void tag_index_destroy(TagIndex* index) {
    if (!index) {
        return;
    }

    tag_index_clear(index);
    kfree(index);
}

void tag_index_clear(TagIndex* index) {
    if (!index) {
        return;
    }

    for (uint32_t i = 0; i < 256; i++) {
        TagIndexEntry* entry = index->buckets[i];
        while (entry) {
            TagIndexEntry* next = entry->next;
            if (entry->file_ids) {
                kfree(entry->file_ids);
            }
            kfree(entry);
            entry = next;
        }
        index->buckets[i] = NULL;
    }

    index->total_tags = 0;
}

int tag_index_add_file(TagIndex* index, const char* tag_string, uint32_t file_id) {
    if (!index || !tag_string) {
        return -1;
    }

    uint32_t hash = tag_hash(tag_string);
    TagIndexEntry* entry = index->buckets[hash];

    while (entry) {
        if (strcmp(entry->tag_string, tag_string) == 0) {
            for (uint32_t i = 0; i < entry->file_count; i++) {
                if (entry->file_ids[i] == file_id) {
                    return 0;
                }
            }

            uint32_t* new_ids = kmalloc(sizeof(uint32_t) * (entry->file_count + 1));
            if (!new_ids) {
                return -1;
            }

            memcpy(new_ids, entry->file_ids, sizeof(uint32_t) * entry->file_count);
            new_ids[entry->file_count] = file_id;

            kfree(entry->file_ids);
            entry->file_ids = new_ids;
            entry->file_count++;
            return 0;
        }
        entry = entry->next;
    }

    TagIndexEntry* new_entry = kmalloc(sizeof(TagIndexEntry));
    if (!new_entry) {
        return -1;
    }

    memset(new_entry, 0, sizeof(TagIndexEntry));
    strncpy(new_entry->tag_string, tag_string, 63);
    new_entry->tag_string[63] = '\0';

    new_entry->file_ids = kmalloc(sizeof(uint32_t));
    if (!new_entry->file_ids) {
        kfree(new_entry);
        return -1;
    }

    new_entry->file_ids[0] = file_id;
    new_entry->file_count = 1;
    new_entry->next = index->buckets[hash];
    index->buckets[hash] = new_entry;
    index->total_tags++;

    return 0;
}

int tag_index_remove_file(TagIndex* index, const char* tag_string, uint32_t file_id) {
    if (!index || !tag_string) {
        return -1;
    }

    uint32_t hash = tag_hash(tag_string);
    TagIndexEntry* entry = index->buckets[hash];
    TagIndexEntry* prev = NULL;

    while (entry) {
        if (strcmp(entry->tag_string, tag_string) == 0) {
            uint32_t found_index = UINT32_MAX;
            for (uint32_t i = 0; i < entry->file_count; i++) {
                if (entry->file_ids[i] == file_id) {
                    found_index = i;
                    break;
                }
            }

            if (found_index == UINT32_MAX) {
                return -1;
            }

            if (entry->file_count == 1) {
                if (prev) {
                    prev->next = entry->next;
                } else {
                    index->buckets[hash] = entry->next;
                }
                kfree(entry->file_ids);
                kfree(entry);
                index->total_tags--;
                return 0;
            }

            uint32_t* new_ids = kmalloc(sizeof(uint32_t) * (entry->file_count - 1));
            if (!new_ids) {
                return -1;
            }

            uint32_t dst_idx = 0;
            for (uint32_t i = 0; i < entry->file_count; i++) {
                if (i != found_index) {
                    new_ids[dst_idx++] = entry->file_ids[i];
                }
            }

            kfree(entry->file_ids);
            entry->file_ids = new_ids;
            entry->file_count--;
            return 0;
        }

        prev = entry;
        entry = entry->next;
    }

    return -1;
}

int tag_index_query(TagIndex* index, const char* tag_string, uint32_t** file_ids, uint32_t* count) {
    if (!index || !tag_string || !file_ids || !count) {
        return -1;
    }

    *file_ids = NULL;
    *count = 0;

    uint32_t hash = tag_hash(tag_string);
    TagIndexEntry* entry = index->buckets[hash];

    while (entry) {
        if (strcmp(entry->tag_string, tag_string) == 0) {
            *file_ids = entry->file_ids;
            *count = entry->file_count;
            return 0;
        }
        entry = entry->next;
    }

    return -1;
}

int tag_index_rebuild(TagIndex* index, TagFSMetadata* metadata_cache, uint32_t total_files) {
    if (!index || !metadata_cache) {
        return -1;
    }

    tag_index_clear(index);

    for (uint32_t file_id = 0; file_id < total_files; file_id++) {
        TagFSMetadata* meta = &metadata_cache[file_id];

        if (!(meta->flags & TAGFS_FILE_ACTIVE)) {
            continue;
        }

        for (uint32_t tag_idx = 0; tag_idx < meta->tag_count && tag_idx < TAGFS_MAX_TAGS_PER_FILE; tag_idx++) {
            TagFSTag* tag = &meta->tags[tag_idx];
            char tag_string[64];
            tagfs_format_tag(tag_string, tag->key, tag->value);

            if (tag_index_add_file(index, tag_string, file_id) != 0) {
                debug_printf("[TagIndex] Warning: Failed to add file %u with tag '%s'\n", file_id, tag_string);
            }
        }
    }

    debug_printf("[TagIndex] Rebuilt index with %u unique tags\n", index->total_tags);
    return 0;
}
