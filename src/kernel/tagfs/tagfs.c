#include "tagfs.h"
#include "metadata_cache.h"
#include "tag_bitmap.h"
#include "journal.h"
#include "klib.h"
#include "ata.h"
#include "rtc.h"

// Verify data start doesn't overlap with journal entries
STATIC_ASSERT(TAGFS_DATA_START > (JOURNAL_ENTRIES_START + JOURNAL_ENTRY_COUNT * JOURNAL_ENTRY_SECTORS),
             "TAGFS data must start after journal entries");

static TagFSState tfs;

static inline void bitmap_set(uint8_t* bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void bitmap_clear(uint8_t* bitmap, uint32_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static inline bool bitmap_test(uint8_t* bitmap, uint32_t bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

// Update compact summary from metadata
static void summary_update(uint32_t file_id, const TagFSMetadata* meta) {
    if (!tfs.file_summaries || file_id == 0 || file_id > TAGFS_MAX_FILES) return;
    uint32_t idx = file_id - 1;
    tfs.file_summaries[idx].file_id = meta->file_id;
    tfs.file_summaries[idx].flags = meta->flags;
    tfs.file_summaries[idx].start_block = meta->start_block;
    tfs.file_summaries[idx].block_count = meta->block_count;
}

TagFSState* tagfs_get_state(void) {
    return &tfs;
}

int tagfs_read_superblock(TagFSSuperblock* sb) {
    if (!sb) {
        return -1;
    }

    uint8_t* sector_buffer = kmalloc(ATA_SECTOR_SIZE);
    if (!sector_buffer) {
        return -1;
    }

    if (ata_read_sectors(1, TAGFS_SUPERBLOCK_SECTOR, 1, sector_buffer) != 0) {
        debug_printf("[TagFS] Failed to read superblock from sector %u\n", TAGFS_SUPERBLOCK_SECTOR);
        kfree(sector_buffer);
        return -1;
    }

    memcpy(sb, sector_buffer, sizeof(TagFSSuperblock));

    if (sb->magic != TAGFS_MAGIC) {
        debug_printf("[TagFS] Primary superblock corrupted (bad magic: 0x%08x)\n", sb->magic);
        debug_printf("[TagFS] Attempting to read backup superblock from sector %u...\n",
                TAGFS_SUPERBLOCK_BACKUP);

        if (ata_read_sectors(1, TAGFS_SUPERBLOCK_BACKUP, 1, sector_buffer) != 0) {
            debug_printf("[TagFS] Backup superblock read failed\n");
            kfree(sector_buffer);
            return -1;
        }

        memcpy(sb, sector_buffer, sizeof(TagFSSuperblock));

        if (sb->magic != TAGFS_MAGIC) {
            debug_printf("[TagFS] CRITICAL: Both primary and backup superblocks corrupted\n");
            debug_printf("[TagFS] Cannot mount filesystem\n");
            kfree(sector_buffer);
            return -1;
        }

        debug_printf("[TagFS] SUCCESS: Backup superblock restored\n");
        debug_printf("[TagFS] Restoring primary superblock from backup...\n");

        if (ata_write_sectors(1, TAGFS_SUPERBLOCK_SECTOR, 1, sector_buffer) != 0) {
            debug_printf("[TagFS] Warning: Could not restore primary superblock\n");
        } else {
            debug_printf("[TagFS] Primary superblock restored from backup\n");
        }
    }

    if (sb->version != TAGFS_VERSION) {
        debug_printf("[TagFS] Unsupported version: %u (expected %u)\n", sb->version, TAGFS_VERSION);
        kfree(sector_buffer);
        return -1;
    }

    kfree(sector_buffer);
    return 0;
}

static int tagfs_write_superblock_raw(const TagFSSuperblock* sb) {
    if (!sb) {
        return -1;
    }

    if (sb->magic != TAGFS_MAGIC || sb->version != TAGFS_VERSION) {
        debug_printf("[TagFS] Invalid superblock parameters\n");
        return -1;
    }

    uint8_t* sector_buffer = kmalloc(ATA_SECTOR_SIZE);
    if (!sector_buffer) {
        return -1;
    }

    memset(sector_buffer, 0, ATA_SECTOR_SIZE);
    memcpy(sector_buffer, sb, sizeof(TagFSSuperblock));

    if (ata_write_sectors(1, TAGFS_SUPERBLOCK_SECTOR, 1, sector_buffer) != 0) {
        debug_printf("[TagFS] Failed to write superblock to sector %u\n", TAGFS_SUPERBLOCK_SECTOR);
        kfree(sector_buffer);
        return -1;
    }

    debug_printf("[TagFS] Writing backup superblock to sector %u\n", TAGFS_SUPERBLOCK_BACKUP);
    if (ata_write_sectors(1, TAGFS_SUPERBLOCK_BACKUP, 1, sector_buffer) != 0) {
        debug_printf("[TagFS] Warning: Failed to write backup superblock (primary is still valid)\n");
    }

    if (ata_flush_cache(1) != 0) {
        debug_printf("[TagFS] Warning: Failed to flush cache after superblock write\n");
    }

    kfree(sector_buffer);
    return 0;
}

int tagfs_write_superblock(const TagFSSuperblock* sb) {
    return tagfs_write_superblock_journaled(sb);
}

int tagfs_read_metadata(uint32_t file_id, TagFSMetadata* metadata) {
    if (!metadata || file_id == 0 || file_id > TAGFS_MAX_FILES) {
        return -1;
    }

    uint32_t metadata_index = file_id - 1;
    uint32_t sector = TAGFS_METADATA_START + metadata_index;

    uint8_t* sector_buffer = kmalloc(ATA_SECTOR_SIZE);
    if (!sector_buffer) {
        return -1;
    }

    if (ata_read_sectors(1, sector, 1, sector_buffer) != 0) {
        debug_printf("[TagFS] Failed to read metadata for file %u from sector %u\n", file_id, sector);
        kfree(sector_buffer);
        return -1;
    }

    memcpy(metadata, sector_buffer, sizeof(TagFSMetadata));

    // magic == 0 is allowed for uninitialized (never-created) metadata slots
    if (metadata->magic != TAGFS_METADATA_MAGIC && metadata->magic != 0) {
        debug_printf("[TagFS] Corrupted metadata for file %u (bad magic: 0x%08x, expected: 0x%08x)\n",
                file_id, metadata->magic, TAGFS_METADATA_MAGIC);
        kfree(sector_buffer);
        return -1;
    }

    kfree(sector_buffer);

    if (metadata->magic == 0) {
        metadata->file_id = 0;
    }

    return 0;
}

static int tagfs_write_metadata_raw(uint32_t file_id, const TagFSMetadata* metadata) {
    if (!metadata || file_id == 0 || file_id > TAGFS_MAX_FILES) {
        return -1;
    }

    uint32_t metadata_index = file_id - 1;
    uint32_t sector = TAGFS_METADATA_START + metadata_index;

    uint8_t* sector_buffer = kmalloc(ATA_SECTOR_SIZE);
    if (!sector_buffer) {
        return -1;
    }

    // work on a local copy to preserve const and ensure magic is set
    TagFSMetadata meta_copy = *metadata;
    meta_copy.magic = TAGFS_METADATA_MAGIC;

    memset(sector_buffer, 0, ATA_SECTOR_SIZE);
    memcpy(sector_buffer, &meta_copy, sizeof(TagFSMetadata));

    int result = ata_write_sectors(1, sector, 1, sector_buffer);
    kfree(sector_buffer);

    if (result != 0) {
        debug_printf("[TagFS] Failed to write metadata for file %u to sector %u\n", file_id, sector);
        return -1;
    }

    if (ata_flush_cache(1) != 0) {
        debug_printf("[TagFS] Warning: Failed to flush cache after metadata write (file %u)\n", file_id);
    }

    return 0;
}

// Write-through: writes to disk (journal), then updates LRU cache + summary
int tagfs_write_metadata(uint32_t file_id, const TagFSMetadata* metadata) {
    int result = tagfs_write_metadata_journaled(file_id, metadata);
    if (result == 0) {
        // Keep LRU cache consistent
        if (tfs.mcache) {
            mcache_put(tfs.mcache, file_id, metadata);
        }
        // Keep summary consistent
        summary_update(file_id, metadata);
    }
    return result;
}

// LRU-cached metadata access: O(1) hit, disk load on miss
TagFSMetadata* tagfs_get_metadata(uint32_t file_id) {
    if (!tfs.initialized || file_id == 0 || file_id > TAGFS_MAX_FILES) {
        return NULL;
    }
    return mcache_get(tfs.mcache, file_id);
}

void tagfs_format_tag(char* dest, const char* key, const char* value) {
    if (!dest || !key || !value) {
        return;
    }

    if (value[0] == '\0') {
        ksnprintf(dest, 64, "%s", key);
    } else {
        ksnprintf(dest, 64, "%s:%s", key, value);
    }
}

int tagfs_parse_tag(const char* tag_string, char* key, char* value, uint8_t* type) {
    if (!tag_string || !key || !value || !type) {
        return -1;
    }

    const char* colon = strchr(tag_string, ':');

    if (!colon) {
        size_t len = strlen(tag_string);
        if (len == 0 || len >= 11) {
            return -1;
        }
        *type = TAGFS_TAG_SYSTEM;
        memcpy(key, tag_string, len);
        key[len] = '\0';
        value[0] = '\0';
    } else {
        size_t key_len = colon - tag_string;
        if (key_len == 0 || key_len >= 11) {
            return -1;
        }

        const char* value_start = colon + 1;
        size_t value_len = strlen(value_start);
        if (value_len >= 12) {
            return -1;
        }

        *type = TAGFS_TAG_USER;
        memcpy(key, tag_string, key_len);
        key[key_len] = '\0';

        if (value_len > 0) {
            memcpy(value, value_start, value_len);
        }
        value[value_len] = '\0';
    }

    return 0;
}

int tagfs_init(void) {
    debug_printf("[TagFS] Initializing filesystem...\n");

    if (tfs.initialized) {
        debug_printf("[TagFS] Already initialized\n");
        return 0;
    }

    if (journal_init() != 0) {
        debug_printf("[TagFS] Journal initialization failed\n");
        return -1;
    }

    if (journal_validate_and_replay() != 0) {
        debug_printf("[TagFS] Journal recovery failed\n");
        return -1;
    }

    memset(&tfs, 0, sizeof(TagFSState));
    spinlock_init(&tfs.lock);

    if (tagfs_read_superblock(&tfs.superblock) != 0) {
        debug_printf("[TagFS] Failed to read superblock\n");
        return -1;
    }

    debug_printf("[TagFS] Superblock loaded: version=%u, total_files=%u, free_blocks=%u\n",
            tfs.superblock.version,
            tfs.superblock.total_files,
            tfs.superblock.free_blocks);

    // Allocate compact file summaries (16 bytes per file vs 512 bytes for full metadata)
    tfs.file_summaries = kmalloc(sizeof(TagFSFileSummary) * TAGFS_MAX_FILES);
    if (!tfs.file_summaries) {
        debug_printf("[TagFS] Failed to allocate file summaries\n");
        return -1;
    }
    memset(tfs.file_summaries, 0, sizeof(TagFSFileSummary) * TAGFS_MAX_FILES);

    // Initialize LRU metadata cache (128 entries, ~67 KB)
    tfs.mcache = kmalloc(sizeof(MetadataLRU));
    if (!tfs.mcache) {
        debug_printf("[TagFS] Failed to allocate metadata LRU\n");
        kfree(tfs.file_summaries);
        return -1;
    }
    if (mcache_init(tfs.mcache) != 0) {
        debug_printf("[TagFS] Failed to initialize metadata LRU cache\n");
        kfree(tfs.mcache);
        kfree(tfs.file_summaries);
        return -1;
    }

    // Create tag index before scanning files
    tfs.tag_index = tag_bitmap_create();
    if (!tfs.tag_index) {
        debug_printf("[TagFS] ERROR: Failed to create tag index\n");
        mcache_destroy(tfs.mcache);
        kfree(tfs.mcache);
        kfree(tfs.file_summaries);
        return -1;
    }

    // Scan metadata from disk: build summaries + tag index without keeping all in RAM
    // Uses a single temp buffer instead of a TAGFS_MAX_FILES-sized array
    TagFSMetadata temp_meta;

    for (uint32_t i = 0; i < tfs.superblock.total_files; i++) {
        uint32_t file_id = i + 1;

        if (tagfs_read_metadata(file_id, &temp_meta) != 0) {
            debug_printf("[TagFS] Warning: Failed to read metadata for file %u\n", file_id);
            continue;
        }

        // Save compact summary
        tfs.file_summaries[i].file_id = temp_meta.file_id;
        tfs.file_summaries[i].flags = temp_meta.flags;
        tfs.file_summaries[i].start_block = temp_meta.start_block;
        tfs.file_summaries[i].block_count = temp_meta.block_count;

        if (temp_meta.file_id == 0 || !(temp_meta.flags & TAGFS_FILE_ACTIVE))
            continue;

        // Deduplicate tags
        bool had_duplicates = false;
        for (uint8_t t = 0; t < temp_meta.tag_count; t++) {
            for (uint8_t u = t + 1; u < temp_meta.tag_count; ) {
                if (temp_meta.tags[u].type == temp_meta.tags[t].type &&
                    strcmp(temp_meta.tags[u].key, temp_meta.tags[t].key) == 0 &&
                    strcmp(temp_meta.tags[u].value, temp_meta.tags[t].value) == 0) {
                    for (uint8_t s = u; s + 1 < temp_meta.tag_count; s++) {
                        temp_meta.tags[s] = temp_meta.tags[s + 1];
                    }
                    temp_meta.tag_count--;
                    had_duplicates = true;
                } else {
                    u++;
                }
            }
        }

        if (had_duplicates) {
            debug_printf("[TagFS] Dedup: removed duplicate tags from file %u '%s'\n",
                         file_id, temp_meta.filename);
            tagfs_write_metadata_raw(file_id, &temp_meta);
        }

        // Build tag index entry-by-entry (no full array needed)
        for (uint32_t tag_idx = 0; tag_idx < temp_meta.tag_count && tag_idx < TAGFS_MAX_TAGS_PER_FILE; tag_idx++) {
            char tag_string[64];
            tagfs_format_tag(tag_string, temp_meta.tags[tag_idx].key, temp_meta.tags[tag_idx].value);
            tag_bitmap_add_file(tfs.tag_index, tag_string, file_id);
        }
    }

    debug_printf("[TagFS] File summaries built: %u files scanned (16 bytes/file = %u KB)\n",
            tfs.superblock.total_files,
            (uint32_t)(sizeof(TagFSFileSummary) * TAGFS_MAX_FILES / 1024));

    // Build block bitmap from compact summaries (no full metadata needed)
    uint32_t total_blocks = tfs.superblock.total_blocks;
    uint32_t bitmap_size = (total_blocks + 7) / 8;

    tfs.block_bitmap.bitmap = kmalloc(bitmap_size);
    if (!tfs.block_bitmap.bitmap) {
        debug_printf("[TagFS] ERROR: Failed to allocate block bitmap\n");
        tag_bitmap_destroy(tfs.tag_index);
        mcache_destroy(tfs.mcache);
        kfree(tfs.mcache);
        kfree(tfs.file_summaries);
        return -1;
    }
    tfs.block_bitmap.total_blocks = total_blocks;
    memset(tfs.block_bitmap.bitmap, 0, bitmap_size);

    for (uint32_t i = 0; i < tfs.superblock.max_files; i++) {
        TagFSFileSummary* sum = &tfs.file_summaries[i];
        if (sum->file_id != 0 && (sum->flags & TAGFS_FILE_ACTIVE)) {
            for (uint32_t j = 0; j < sum->block_count; j++) {
                bitmap_set(tfs.block_bitmap.bitmap, sum->start_block + j);
            }
        }
    }

    debug_printf("[TagFS] Block bitmap initialized: %u/%u blocks used\n",
            total_blocks - tfs.superblock.free_blocks, total_blocks);

    debug_printf("[TagFS] Tag index built: %u unique tags\n",
            tfs.tag_index->total_tags);

    debug_printf("[TagFS] LRU metadata cache: %u slots (%u KB)\n",
            MCACHE_CAPACITY,
            (uint32_t)(sizeof(MCacheNode) * MCACHE_CAPACITY / 1024));

    tfs.initialized = true;
    debug_printf("[TagFS] Initialization complete\n");
    return 0;
}

void tagfs_shutdown(void) {
    if (!tfs.initialized) {
        return;
    }

    debug_printf("[TagFS] Shutting down filesystem...\n");

    if (tfs.tag_index) {
        tag_bitmap_destroy(tfs.tag_index);
        tfs.tag_index = NULL;
    }

    if (tfs.block_bitmap.bitmap) {
        kfree(tfs.block_bitmap.bitmap);
        tfs.block_bitmap.bitmap = NULL;
    }

    if (tfs.mcache) {
        mcache_destroy(tfs.mcache);
        kfree(tfs.mcache);
        tfs.mcache = NULL;
    }

    if (tfs.file_summaries) {
        kfree(tfs.file_summaries);
        tfs.file_summaries = NULL;
    }

    tfs.initialized = false;
    debug_printf("[TagFS] Shutdown complete\n");
}

int tagfs_alloc_blocks(uint32_t count, uint32_t* start_block) {
    if (!tfs.initialized || !start_block || count == 0) {
        return -1;
    }

    if (tfs.superblock.free_blocks < count) {
        debug_printf("[TagFS] Not enough free blocks (requested=%u, available=%u)\n",
                count, tfs.superblock.free_blocks);
        return -1;
    }

    uint32_t total = tfs.block_bitmap.total_blocks;
    uint8_t* bitmap = tfs.block_bitmap.bitmap;

    for (uint32_t i = 0; i <= total - count; i++) {
        bool found = true;

        for (uint32_t j = 0; j < count; j++) {
            if (bitmap_test(bitmap, i + j)) {
                found = false;
                i += j;  // skip to next potential start
                break;
            }
        }

        if (found) {
            for (uint32_t j = 0; j < count; j++) {
                bitmap_set(bitmap, i + j);
            }

            *start_block = i;
            tfs.superblock.free_blocks -= count;
            tfs.superblock.fs_modified_time++;

            if (tagfs_write_superblock(&tfs.superblock) != 0) {
                debug_printf("[TagFS] Failed to update superblock after allocation\n");
                return -1;
            }

            return 0;
        }
    }

    debug_printf("[TagFS] Fragmentation: no contiguous space for %u blocks\n", count);
    return -1;
}

int tagfs_free_blocks(uint32_t start_block, uint32_t count) {
    if (!tfs.initialized || count == 0) {
        return -1;
    }

    uint8_t* bitmap = tfs.block_bitmap.bitmap;

    for (uint32_t i = 0; i < count; i++) {
        bitmap_clear(bitmap, start_block + i);
    }

    tfs.superblock.free_blocks += count;
    tfs.superblock.fs_modified_time++;

    if (tagfs_write_superblock(&tfs.superblock) != 0) {
        debug_printf("[TagFS] Failed to update superblock after deallocation\n");
        return -1;
    }

    return 0;
}

int tagfs_query_files(const char* query_tags[], uint32_t tag_count,
                      uint32_t* file_ids, uint32_t max_files) {
    if (!tfs.initialized || !query_tags || !file_ids || tag_count == 0) {
        return 0;
    }

    return tag_bitmap_query(tfs.tag_index, query_tags, tag_count, file_ids, max_files);
}

TagFSFileHandle* tagfs_open(uint32_t file_id, uint32_t flags) {
    if (!tfs.initialized || file_id == 0 || file_id > TAGFS_MAX_FILES) {
        return NULL;
    }

    // Quick check via summary — avoids disk load for inactive files
    uint32_t metadata_index = file_id - 1;
    if (!(tfs.file_summaries[metadata_index].flags & TAGFS_FILE_ACTIVE)) {
        return NULL;
    }

    // Load full metadata from LRU cache (or disk on miss)
    TagFSMetadata* meta = mcache_get(tfs.mcache, file_id);
    if (!meta) {
        return NULL;
    }

    TagFSFileHandle* handle = kmalloc(sizeof(TagFSFileHandle));
    if (!handle) {
        return NULL;
    }

    handle->file_id = file_id;
    handle->flags = flags;
    handle->offset = 0;
    handle->metadata = *meta;  // COPY into handle — safe from LRU eviction

    return handle;
}

void tagfs_close(TagFSFileHandle* handle) {
    if (handle) {
        kfree(handle);
    }
}

int tagfs_read(TagFSFileHandle* handle, void* buffer, uint64_t size) {
    if (!handle || !buffer || !tfs.initialized) {
        return -1;
    }

    TagFSMetadata* meta = &handle->metadata;
    if (handle->offset >= meta->size) {
        return 0;
    }

    uint64_t bytes_to_read = size;
    if (handle->offset + bytes_to_read > meta->size) {
        bytes_to_read = meta->size - handle->offset;
    }

    uint32_t start_block = meta->start_block + (handle->offset / TAGFS_BLOCK_SIZE);
    uint32_t offset_in_block = handle->offset % TAGFS_BLOCK_SIZE;
    uint64_t bytes_read = 0;

    uint8_t* block_buffer = kmalloc(TAGFS_BLOCK_SIZE);
    if (!block_buffer) {
        debug_printf("[TagFS] ERROR: Failed to allocate block buffer\n");
        return -1;
    }

    while (bytes_read < bytes_to_read) {
        uint32_t data_sector = tfs.superblock.data_start_sector + (start_block * 8);

        if (ata_read_sectors(1, data_sector, 8, block_buffer) != 0) {
            kfree(block_buffer);
            return -1;
        }

        uint64_t chunk_size = TAGFS_BLOCK_SIZE - offset_in_block;
        if (bytes_read + chunk_size > bytes_to_read) {
            chunk_size = bytes_to_read - bytes_read;
        }

        if (offset_in_block + chunk_size > TAGFS_BLOCK_SIZE) {
            debug_printf("[TagFS] ERROR: Read would overflow block_buffer (offset=%u, chunk=%llu, block_size=%u)\n",
                         offset_in_block, chunk_size, TAGFS_BLOCK_SIZE);
            return -1;
        }

        memcpy((uint8_t*)buffer + bytes_read, block_buffer + offset_in_block, chunk_size);
        bytes_read += chunk_size;
        handle->offset += chunk_size;

        offset_in_block = 0;
        start_block++;
    }

    kfree(block_buffer);
    return bytes_read;
}

int tagfs_write(TagFSFileHandle* handle, const void* buffer, uint64_t size) {
    if (!handle || !buffer || !tfs.initialized) {
        return -1;
    }

    if (!(handle->flags & TAGFS_HANDLE_WRITE)) {
        return -1;
    }

    TagFSMetadata* meta = &handle->metadata;
    uint64_t new_size = handle->offset + size;

    uint32_t blocks_needed = (new_size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE;

    if (blocks_needed > meta->block_count) {
        uint32_t additional_blocks = blocks_needed - meta->block_count;
        uint32_t new_start_block;

        debug_printf("[TagFS] File %u needs %u more blocks (current: %u, needed: %u)\n",
                meta->file_id, additional_blocks, meta->block_count, blocks_needed);

        if (tagfs_alloc_blocks(additional_blocks, &new_start_block) != 0) {
            debug_printf("[TagFS] Failed to allocate %u blocks (out of space)\n", additional_blocks);
            return -1;
        }

        if (meta->block_count > 0) {
            if (new_start_block != meta->start_block + meta->block_count) {
                debug_printf("[TagFS] Fragmentation prevents file growth (gap between blocks)\n");
                debug_printf("[TagFS]   Current allocation: blocks %u-%u\n",
                        meta->start_block, meta->start_block + meta->block_count - 1);
                debug_printf("[TagFS]   New allocation: blocks %u-%u (not contiguous!)\n",
                        new_start_block, new_start_block + additional_blocks - 1);

                tagfs_free_blocks(new_start_block, additional_blocks);
                return -1;
            }
        } else {
            meta->start_block = new_start_block;
        }

        meta->block_count += additional_blocks;

        if (tagfs_write_metadata(meta->file_id, meta) != 0) {
            debug_printf("[TagFS] CRITICAL: Failed to persist block allocation\n");
            tagfs_free_blocks(new_start_block, additional_blocks);
            return -1;
        }

        debug_printf("[TagFS] File %u grew to %u blocks (allocated blocks %u-%u)\n",
                meta->file_id, meta->block_count,
                meta->start_block, meta->start_block + meta->block_count - 1);
    }

    if (blocks_needed > meta->block_count) {
        debug_printf("[TagFS] ERROR: Block allocation failed\n");
        debug_printf("[TagFS]   Requested: %u blocks\n", blocks_needed);
        debug_printf("[TagFS]   Allocated: %u blocks\n", meta->block_count);
        debug_printf("[TagFS]   Reason: Disk fragmentation or out of space\n");
        return -1;
    }

    uint64_t max_writable_size = (uint64_t)meta->block_count * TAGFS_BLOCK_SIZE;
    if (new_size > max_writable_size) {
        debug_printf("[TagFS] INTERNAL ERROR: new_size (%lu) exceeds allocation (%lu)\n",
                     new_size, max_writable_size);
        debug_printf("[TagFS]   This indicates a bug in block allocation logic\n");
        return -1;
    }

    uint32_t start_block = meta->start_block + (handle->offset / TAGFS_BLOCK_SIZE);
    uint32_t offset_in_block = handle->offset % TAGFS_BLOCK_SIZE;
    uint64_t bytes_written = 0;

    uint8_t* block_buffer = kmalloc(TAGFS_BLOCK_SIZE);
    if (!block_buffer) {
        debug_printf("[TagFS] ERROR: Failed to allocate block buffer\n");
        return -1;
    }

    while (bytes_written < size) {
        uint32_t data_sector = tfs.superblock.data_start_sector + (start_block * 8);

        if (offset_in_block != 0 || (size - bytes_written) < TAGFS_BLOCK_SIZE) {
            if (ata_read_sectors(1, data_sector, 8, block_buffer) != 0) {
                kfree(block_buffer);
                return -1;
            }
        }

        uint64_t chunk_size = TAGFS_BLOCK_SIZE - offset_in_block;
        if (bytes_written + chunk_size > size) {
            chunk_size = size - bytes_written;
        }

        if (offset_in_block + chunk_size > TAGFS_BLOCK_SIZE) {
            debug_printf("[TagFS] ERROR: Write would overflow block_buffer (offset=%u, chunk=%llu, block_size=%u)\n",
                         offset_in_block, chunk_size, TAGFS_BLOCK_SIZE);
            return -1;
        }

        memcpy(block_buffer + offset_in_block, (const uint8_t*)buffer + bytes_written, chunk_size);

        if (ata_write_sectors(1, data_sector, 8, block_buffer) != 0) {
            kfree(block_buffer);
            return -1;
        }

        bytes_written += chunk_size;
        handle->offset += chunk_size;

        offset_in_block = 0;
        start_block++;
    }

    if (handle->offset > meta->size) {
        meta->size = handle->offset;
        meta->modified_time = rtc_get_unix64();
        tagfs_write_metadata(meta->file_id, meta);
    }

    kfree(block_buffer);
    return bytes_written;
}

int tagfs_create_file(const char* filename, const char* tags[], uint32_t tag_count, uint32_t* file_id) {
    if (!tfs.initialized || !filename || !file_id) {
        return -1;
    }

    if (tfs.superblock.total_files >= TAGFS_MAX_FILES) {
        return -1;
    }

    *file_id = tfs.superblock.total_files + 1;
    uint32_t metadata_index = tfs.superblock.total_files;

    TagFSMetadata* meta = kmalloc(sizeof(TagFSMetadata));
    if (!meta) {
        return -1;
    }

    memset(meta, 0, sizeof(TagFSMetadata));
    meta->magic = TAGFS_METADATA_MAGIC;
    meta->file_id = *file_id;
    meta->flags = TAGFS_FILE_ACTIVE;
    meta->size = 0;
    meta->start_block = 0;
    meta->block_count = 0;
    uint64_t now = rtc_get_unix64();
    meta->created_time = now;
    meta->modified_time = now;
    meta->tag_count = 0;

    strncpy(meta->filename, filename, TAGFS_MAX_FILENAME - 1);
    meta->filename[TAGFS_MAX_FILENAME - 1] = '\0';

    if (tags != NULL) {
        for (uint32_t i = 0; i < tag_count && meta->tag_count < TAGFS_MAX_TAGS_PER_FILE; i++) {
            char key[12];
            char value[13];
            uint8_t tag_type;
            if (tagfs_parse_tag(tags[i], key, value, &tag_type) == 0) {
                bool duplicate = false;
                for (uint8_t d = 0; d < meta->tag_count; d++) {
                    if (meta->tags[d].type == tag_type &&
                        strcmp(meta->tags[d].key, key) == 0 &&
                        strcmp(meta->tags[d].value, value) == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) continue;

                memset(&meta->tags[meta->tag_count], 0, sizeof(TagFSTag));

                meta->tags[meta->tag_count].type = tag_type;

                size_t key_len = strlen(key);
                if (key_len > 10) key_len = 10;
                memcpy(meta->tags[meta->tag_count].key, key, key_len);
                meta->tags[meta->tag_count].key[key_len] = '\0';

                size_t value_len = strlen(value);
                if (value_len > 11) value_len = 11;
                memcpy(meta->tags[meta->tag_count].value, value, value_len);
                meta->tags[meta->tag_count].value[value_len] = '\0';

                meta->tag_count++;
            }
        }
    }

    if (tagfs_write_metadata(*file_id, meta) != 0) {
        kfree(meta);
        return -1;
    }

    // Update summary for new file
    tfs.file_summaries[metadata_index].file_id = meta->file_id;
    tfs.file_summaries[metadata_index].flags = meta->flags;
    tfs.file_summaries[metadata_index].start_block = meta->start_block;
    tfs.file_summaries[metadata_index].block_count = meta->block_count;

    // Insert into LRU cache
    mcache_put(tfs.mcache, *file_id, meta);

    tfs.superblock.total_files++;
    tagfs_write_superblock(&tfs.superblock);

    for (uint32_t i = 0; i < meta->tag_count; i++) {
        char tag_string[64];
        tagfs_format_tag(tag_string, meta->tags[i].key, meta->tags[i].value);
        tag_bitmap_add_file(tfs.tag_index, tag_string, *file_id);
    }

    kfree(meta);
    return 0;
}

int tagfs_delete_file(uint32_t file_id) {
    if (!tfs.initialized || file_id == 0 || file_id > TAGFS_MAX_FILES) {
        return -1;
    }

    uint32_t metadata_index = file_id - 1;

    // Check via summary first
    if (!(tfs.file_summaries[metadata_index].flags & TAGFS_FILE_ACTIVE)) {
        return -1;
    }

    // Load full metadata for block info
    TagFSMetadata* meta = mcache_get(tfs.mcache, file_id);
    if (!meta) {
        return -1;
    }

    tag_bitmap_remove_file(tfs.tag_index, file_id);

    if (meta->block_count > 0) {
        tagfs_free_blocks(meta->start_block, meta->block_count);
    }

    meta->flags = 0;
    tagfs_write_metadata(file_id, meta);

    // Trim total_files using summaries (no full metadata needed)
    while (tfs.superblock.total_files > 0) {
        uint32_t last_idx = tfs.superblock.total_files - 1;
        if (!(tfs.file_summaries[last_idx].flags & TAGFS_FILE_ACTIVE)) {
            tfs.superblock.total_files--;
        } else {
            break;
        }
    }
    tagfs_write_superblock(&tfs.superblock);

    // Invalidate stale cache entry
    mcache_invalidate(tfs.mcache, file_id);

    return 0;
}

int tagfs_add_tag(uint32_t file_id, const char* key, const char* value, uint8_t type) {
    if (!tfs.initialized || file_id == 0 || file_id > TAGFS_MAX_FILES || !key || !value) {
        return -1;
    }

    TagFSMetadata* meta = mcache_get(tfs.mcache, file_id);
    if (!meta || !(meta->flags & TAGFS_FILE_ACTIVE)) {
        return -1;
    }

    // Check for duplicate: same key, value, and type
    for (uint8_t i = 0; i < meta->tag_count; i++) {
        if (meta->tags[i].type == type &&
            strcmp(meta->tags[i].key, key) == 0 &&
            strcmp(meta->tags[i].value, value) == 0) {
            return 0;  // Already exists, not an error
        }
    }

    if (meta->tag_count >= TAGFS_MAX_TAGS_PER_FILE) {
        return -1;
    }

    memset(&meta->tags[meta->tag_count], 0, sizeof(TagFSTag));

    meta->tags[meta->tag_count].type = type;

    size_t key_len = strlen(key);
    if (key_len > 10) key_len = 10;
    memcpy(meta->tags[meta->tag_count].key, key, key_len);
    meta->tags[meta->tag_count].key[key_len] = '\0';

    size_t value_len = strlen(value);
    if (value_len > 11) value_len = 11;
    memcpy(meta->tags[meta->tag_count].value, value, value_len);
    meta->tags[meta->tag_count].value[value_len] = '\0';

    meta->tag_count++;

    if (strcmp(key, "trashed") == 0) {
        meta->flags |= TAGFS_FILE_TRASHED;
    }

    char tag_string[64];
    tagfs_format_tag(tag_string, key, value);
    tag_bitmap_add_file(tfs.tag_index, tag_string, file_id);

    return tagfs_write_metadata(file_id, meta);
}

int tagfs_remove_tag(uint32_t file_id, const char* key) {
    if (!tfs.initialized || file_id == 0 || file_id > TAGFS_MAX_FILES || !key) {
        return -1;
    }

    TagFSMetadata* meta = mcache_get(tfs.mcache, file_id);
    if (!meta || !(meta->flags & TAGFS_FILE_ACTIVE)) {
        return -1;
    }

    if (meta->tag_count == 0) {
        return -1;
    }

    for (uint32_t i = 0; i < meta->tag_count; i++) {
        if (strcmp(meta->tags[i].key, key) == 0) {
            if (strcmp(key, "trashed") == 0) {
                meta->flags &= ~TAGFS_FILE_TRASHED;
            }

            tag_bitmap_remove_file(tfs.tag_index, file_id);

            uint32_t last_index = (uint32_t)meta->tag_count - 1;
            if (i < last_index) {
                for (uint32_t j = i; j < last_index; j++) {
                    meta->tags[j] = meta->tags[j + 1];
                }
            }
            meta->tag_count--;

            for (uint32_t j = 0; j < meta->tag_count; j++) {
                char tag_string[64];
                tagfs_format_tag(tag_string, meta->tags[j].key, meta->tags[j].value);
                tag_bitmap_add_file(tfs.tag_index, tag_string, file_id);
            }

            return tagfs_write_metadata(file_id, meta);
        }
    }

    return -1;
}

bool tagfs_has_tag(uint32_t file_id, const char* key, const char* value) {
    if (!tfs.initialized || file_id == 0 || file_id > TAGFS_MAX_FILES || !key) {
        return false;
    }

    TagFSMetadata* meta = mcache_get(tfs.mcache, file_id);
    if (!meta || !(meta->flags & TAGFS_FILE_ACTIVE)) {
        return false;
    }

    bool key_wildcard = (strcmp(key, "...") == 0);
    bool value_wildcard = (value && strcmp(value, "...") == 0);

    for (uint32_t i = 0; i < meta->tag_count; i++) {
        bool key_ok = key_wildcard || strcmp(meta->tags[i].key, key) == 0;
        bool val_ok = !value || value_wildcard || strcmp(meta->tags[i].value, value) == 0;
        if (key_ok && val_ok) {
            return true;
        }
    }

    return false;
}

int tagfs_get_tags(uint32_t file_id, TagFSTag* tags, uint32_t max_tags) {
    if (!tfs.initialized || file_id == 0 || file_id > TAGFS_MAX_FILES || !tags) {
        return 0;
    }

    TagFSMetadata* meta = mcache_get(tfs.mcache, file_id);
    if (!meta || !(meta->flags & TAGFS_FILE_ACTIVE)) {
        return 0;
    }

    uint32_t count = meta->tag_count < max_tags ? meta->tag_count : max_tags;
    memcpy(tags, meta->tags, count * sizeof(TagFSTag));

    return count;
}

int tagfs_rename_file(uint32_t file_id, const char* new_filename) {
    if (!tfs.initialized) {
        debug_printf("[TagFS] ERROR: Not initialized\n");
        return -1;
    }

    if (file_id == 0 || file_id > TAGFS_MAX_FILES) {
        debug_printf("[TagFS] ERROR: Invalid file_id %u\n", file_id);
        return -1;
    }

    if (!new_filename || new_filename[0] == '\0') {
        debug_printf("[TagFS] ERROR: Empty filename\n");
        return -1;
    }

    TagFSMetadata* metadata = mcache_get(tfs.mcache, file_id);
    if (!metadata || !(metadata->flags & TAGFS_FILE_ACTIVE)) {
        debug_printf("[TagFS] ERROR: File %u not active\n", file_id);
        return -1;
    }

    size_t len = strlen(new_filename);
    if (len >= TAGFS_MAX_FILENAME) {
        len = TAGFS_MAX_FILENAME - 1;
    }

    strncpy(metadata->filename, new_filename, len);
    metadata->filename[len] = '\0';

    metadata->modified_time = rtc_get_unix64();

    int result = tagfs_write_metadata(file_id, metadata);
    if (result != 0) {
        debug_printf("[TagFS] ERROR: Failed to write metadata for file %u\n", file_id);
        return -1;
    }

    debug_printf("[TagFS] Renamed file %u to '%s'\n", file_id, new_filename);
    return 0;
}

int tagfs_write_metadata_journaled(uint32_t file_id, const TagFSMetadata* metadata) {
    if (!metadata || file_id == 0 || file_id > TAGFS_MAX_FILES) {
        return -1;
    }

    uint32_t txn_id;
    if (journal_begin(&txn_id) != 0) {
        debug_printf("[TagFS] Journal begin failed for metadata write\n");
        return tagfs_write_metadata_raw(file_id, metadata);
    }

    if (journal_log_metadata(txn_id, file_id, metadata) != 0) {
        debug_printf("[TagFS] Journal log metadata failed\n");
        journal_abort(txn_id);
        return tagfs_write_metadata_raw(file_id, metadata);
    }

    if (journal_commit(txn_id) != 0) {
        debug_printf("[TagFS] Journal commit failed for metadata\n");
        journal_abort(txn_id);
        return -1;
    }

    return 0;
}

int tagfs_write_superblock_journaled(const TagFSSuperblock* sb) {
    if (!sb) {
        return -1;
    }

    if (sb->magic != TAGFS_MAGIC || sb->version != TAGFS_VERSION) {
        return -1;
    }

    uint32_t txn_id;
    if (journal_begin(&txn_id) != 0) {
        debug_printf("[TagFS] Journal begin failed for superblock write\n");
        return tagfs_write_superblock_raw(sb);
    }

    if (journal_log_superblock(txn_id, sb) != 0) {
        debug_printf("[TagFS] Journal log superblock failed\n");
        journal_abort(txn_id);
        return tagfs_write_superblock_raw(sb);
    }

    if (journal_commit(txn_id) != 0) {
        debug_printf("[TagFS] Journal commit failed for superblock\n");
        journal_abort(txn_id);
        return -1;
    }

    return 0;
}

int tagfs_defrag_file(uint32_t file_id, uint32_t target_block) {
    if (!tfs.initialized) {
        debug_printf("[TagFS] Defrag: not initialized\n");
        return -1;
    }

    TagFSMetadata* meta = tagfs_get_metadata(file_id);
    if (!meta || !(meta->flags & TAGFS_FILE_ACTIVE)) {
        debug_printf("[TagFS] Defrag: file %u not found or inactive\n", file_id);
        return -1;
    }

    if (meta->block_count == 0) {
        debug_printf("[TagFS] Defrag: file %u has no blocks, nothing to defragment\n", file_id);
        return 0;
    }

    uint32_t new_start;
    if (target_block != 0) {
        for (uint32_t i = 0; i < meta->block_count; i++) {
            if (bitmap_test(tfs.block_bitmap.bitmap, target_block + i)) {
                debug_printf("[TagFS] Defrag: target block %u already allocated\n", target_block);
                return -1;
            }
        }
        new_start = target_block;
        for (uint32_t i = 0; i < meta->block_count; i++) {
            bitmap_set(tfs.block_bitmap.bitmap, new_start + i);
        }
        tfs.superblock.free_blocks -= meta->block_count;
    } else {
        if (tagfs_alloc_blocks(meta->block_count, &new_start) != 0) {
            debug_printf("[TagFS] Defrag: failed to allocate %u contiguous blocks\n",
                        meta->block_count);
            return -1;
        }
    }

    uint8_t* buffer = kmalloc(TAGFS_BLOCK_SIZE);
    if (!buffer) {
        debug_printf("[TagFS] Defrag: failed to allocate buffer\n");
        tagfs_free_blocks(new_start, meta->block_count);
        return -1;
    }

    for (uint32_t i = 0; i < meta->block_count; i++) {
        uint32_t old_sector = tfs.superblock.data_start_sector
                              + (meta->start_block + i) * 8;
        uint32_t new_sector = tfs.superblock.data_start_sector
                              + (new_start + i) * 8;

        if (ata_read_sectors(1, old_sector, 8, buffer) != 0) {
            debug_printf("[TagFS] Defrag: failed to read block %u\n", i);
            kfree(buffer);
            tagfs_free_blocks(new_start, meta->block_count);
            return -1;
        }

        if (ata_write_sectors(1, new_sector, 8, buffer) != 0) {
            debug_printf("[TagFS] Defrag: failed to write block %u\n", i);
            kfree(buffer);
            tagfs_free_blocks(new_start, meta->block_count);
            return -1;
        }
    }

    kfree(buffer);

    uint32_t old_start = meta->start_block;
    meta->start_block = new_start;

    if (tagfs_write_metadata_raw(file_id, meta) != 0) {
        debug_printf("[TagFS] Defrag: failed to update metadata\n");
        meta->start_block = old_start;
        tagfs_free_blocks(new_start, meta->block_count);
        return -1;
    }

    // Update cache + summary after raw write
    mcache_put(tfs.mcache, file_id, meta);
    summary_update(file_id, meta);

    tagfs_free_blocks(old_start, meta->block_count);

    debug_printf("[TagFS] Defrag: file %u moved from block %u to %u (%u blocks)\n",
                file_id, old_start, new_start, meta->block_count);

    return 0;
}

// Uses compact summaries — no full metadata load needed
uint32_t tagfs_get_fragmentation_score(void) {
    if (!tfs.initialized) {
        return 0;
    }

    uint32_t total_gaps = 0;
    uint32_t total_files = 0;

    for (uint32_t i = 0; i < TAGFS_MAX_FILES; i++) {
        TagFSFileSummary* sum = &tfs.file_summaries[i];
        if (!(sum->flags & TAGFS_FILE_ACTIVE) || sum->block_count == 0) {
            continue;
        }

        total_files++;

        uint32_t expected_next = sum->start_block + sum->block_count;
        uint32_t next_allocated = expected_next;

        while (next_allocated < tfs.block_bitmap.total_blocks &&
               !bitmap_test(tfs.block_bitmap.bitmap, next_allocated)) {
            next_allocated++;
        }

        if (next_allocated > expected_next) {
            total_gaps += (next_allocated - expected_next);
        }
    }

    if (total_files == 0) {
        return 0;
    }

    uint32_t avg_gap = total_gaps / total_files;
    uint32_t score = (avg_gap * 100) / tfs.superblock.total_blocks;

    return score > 100 ? 100 : score;
}

// Uses compact summaries — no full metadata load needed
int tagfs_list_all_files(uint32_t* file_ids, uint32_t max_files) {
    if (!tfs.initialized) {
        debug_printf("[TagFS] ERROR: Not initialized\n");
        return 0;
    }

    if (!file_ids || max_files == 0) {
        debug_printf("[TagFS] ERROR: Invalid parameters\n");
        return 0;
    }

    uint32_t count = 0;

    for (uint32_t i = 0; i < TAGFS_MAX_FILES && count < max_files; i++) {
        TagFSFileSummary* sum = &tfs.file_summaries[i];

        if (sum->file_id != 0 &&
            (sum->flags & TAGFS_FILE_ACTIVE) &&
            !(sum->flags & TAGFS_FILE_TRASHED)) {
            file_ids[count++] = sum->file_id;
        }
    }

    return count;
}

void tagfs_sync(void) {
    if (!tfs.initialized) {
        return;
    }

    debug_printf("[TagFS] Syncing filesystem to disk...\n");

    if (tagfs_write_superblock(&tfs.superblock) != 0) {
        debug_printf("[TagFS] WARNING: Failed to sync superblock\n");
    }

    debug_printf("[TagFS] Sync complete\n");
}
