#include "tagfs.h"
#include "tag_registry/tag_registry.h"
#include "tag_bitmap/tag_bitmap.h"
#include "file_table/file_table.h"
#include "metadata_pool/meta_pool.h"
#include "error.h"
#include "ahci_sync.h"

static TagFSState g_state;

// ----------------------------------------------------------------------------
// Bitmap bit helpers
// ----------------------------------------------------------------------------

static inline void bitmap_set_bit(uint8_t* bitmap, uint32_t bit) {
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static inline void bitmap_clear_bit(uint8_t* bitmap, uint32_t bit) {
    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

static inline bool bitmap_test_bit(const uint8_t* bitmap, uint32_t bit) {
    return (bitmap[bit / 8] & (uint8_t)(1u << (bit % 8))) != 0;
}

// ----------------------------------------------------------------------------
// Disk I/O helpers
// ----------------------------------------------------------------------------

static uint64_t block_to_sector(uint32_t block) {
    uint32_t data_start = g_state.superblock.block_bitmap_sector +
                          g_state.superblock.block_bitmap_sector_count;
    return (uint64_t)data_start + (uint64_t)block * 8;
}

static int read_block(uint32_t block, void* buffer) {
    return ahci_read_sectors_sync(0, block_to_sector(block), 8, buffer);
}

static int write_block(uint32_t block, const void* buffer) {
    return ahci_write_sectors_sync(0, block_to_sector(block), 8, (const void*)buffer);
}

int tagfs_read_block(uint32_t block, void* buffer) {
    return read_block(block, buffer);
}

int tagfs_write_block(uint32_t block, const void* buffer) {
    return write_block(block, buffer);
}

// ----------------------------------------------------------------------------
// Superblock I/O
// ----------------------------------------------------------------------------

static int read_superblock(uint32_t sector, TagFSSuperblock* out) {
    uint8_t buf[512];
    if (ahci_read_sectors_sync(0, (uint64_t)sector, 1, buf) != 0) {
        return -1;
    }
    memcpy(out, buf, sizeof(TagFSSuperblock));
    return 0;
}

static int write_superblock_to_sector(uint32_t sector, const TagFSSuperblock* sb) {
    uint8_t buf[512];
    memset(buf, 0, 512);
    memcpy(buf, sb, sizeof(TagFSSuperblock));
    return ahci_write_sectors_sync(0, (uint64_t)sector, 1, buf);
}

int tagfs_write_superblock(const TagFSSuperblock* sb) {
    if (!sb) return -1;
    int r = write_superblock_to_sector(TAGFS_SUPERBLOCK_SECTOR, sb);
    if (r != 0) {
        debug_printf("[TagFS] Failed to write primary superblock\n");
        return r;
    }
    if (write_superblock_to_sector(TAGFS_BACKUP_SB_SECTOR, sb) != 0) {
        debug_printf("[TagFS] Warning: failed to write backup superblock\n");
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Free extent list
// ----------------------------------------------------------------------------

static void free_list_destroy(void) {
    FreeExtent* cur = g_state.block_bitmap.free_list;
    while (cur) {
        FreeExtent* next = cur->next;
        kfree(cur);
        cur = next;
    }
    g_state.block_bitmap.free_list  = NULL;
    g_state.block_bitmap.extent_count = 0;
}

static void free_list_build(void) {
    free_list_destroy();

    uint32_t total  = g_state.block_bitmap.total_blocks;
    uint8_t* bitmap = g_state.block_bitmap.bitmap;
    FreeExtent** tail = &g_state.block_bitmap.free_list;

    uint32_t i = 0;
    while (i < total) {
        // skip used blocks
        while (i < total) {
            if ((i & 7) == 0 && i + 8 <= total && bitmap[i / 8] == 0xFF) {
                i += 8;
                continue;
            }
            if (bitmap_test_bit(bitmap, i)) {
                i++;
                continue;
            }
            break;
        }
        if (i >= total) break;

        uint32_t start = i;
        while (i < total) {
            if ((i & 7) == 0 && i + 8 <= total && bitmap[i / 8] == 0x00) {
                i += 8;
                continue;
            }
            if (!bitmap_test_bit(bitmap, i)) {
                i++;
                continue;
            }
            break;
        }

        FreeExtent* ext = kmalloc(sizeof(FreeExtent));
        if (!ext) {
            debug_printf("[TagFS] free_list_build: kmalloc failed\n");
            break;
        }
        ext->start = start;
        ext->count = i - start;
        ext->next  = NULL;
        *tail = ext;
        tail  = &ext->next;
        g_state.block_bitmap.extent_count++;
    }
}

// ----------------------------------------------------------------------------
// tagfs_format
// ----------------------------------------------------------------------------

int tagfs_format(uint32_t total_blocks) {
    debug_printf("[TagFS] Formatting: total_blocks=%u\n", total_blocks);

    // --- Compute layout ---
    //
    // Sector layout (all fixed):
    //   1034 : primary superblock
    //   1035 : backup superblock
    //   1036 : journal superblock
    //   1037 : journal superblock backup
    //   1038 : journal entries  (512 entries * 2 sectors = 1024 sectors -> 1038..2061)
    //   2062 : block bitmap start
    //
    uint32_t bitmap_sector_start = 2062;
    uint32_t bitmap_bytes        = (total_blocks + 7) / 8;
    uint32_t bitmap_sectors      = (bitmap_bytes + 511) / 512;

    // --- Build initial block bitmap in memory ---
    uint8_t* bitmap = kmalloc(bitmap_bytes);
    if (!bitmap) {
        debug_printf("[TagFS] format: failed to allocate bitmap\n");
        return -1;
    }
    memset(bitmap, 0, bitmap_bytes);

    // Blocks 0,1,2 are reserved (tag registry, file table, metadata pool)
    bitmap_set_bit(bitmap, 0);
    bitmap_set_bit(bitmap, 1);
    bitmap_set_bit(bitmap, 2);

    // --- Write initial TagRegistryBlock at block 0 ---
    TagRegistryBlock* reg_block = kmalloc(TAGFS_BLOCK_SIZE);
    if (!reg_block) {
        debug_printf("[TagFS] format: failed to allocate registry block\n");
        kfree(bitmap);
        return -1;
    }
    memset(reg_block, 0, TAGFS_BLOCK_SIZE);
    reg_block->magic       = TAGFS_REGISTRY_MAGIC;
    reg_block->next_block  = 0;
    reg_block->entry_count = 0;
    reg_block->used_bytes  = 0;

    // Temporarily set up g_state.superblock enough for block_to_sector to work
    g_state.superblock.block_bitmap_sector       = bitmap_sector_start;
    g_state.superblock.block_bitmap_sector_count = bitmap_sectors;

    if (write_block(0, reg_block) != 0) {
        debug_printf("[TagFS] format: failed to write registry block\n");
        kfree(reg_block);
        kfree(bitmap);
        return -1;
    }
    kfree(reg_block);

    // --- Write initial FileTableBlock at block 1 ---
    FileTableBlock* ft_block = kmalloc(TAGFS_BLOCK_SIZE);
    if (!ft_block) {
        debug_printf("[TagFS] format: failed to allocate file table block\n");
        kfree(bitmap);
        return -1;
    }
    memset(ft_block, 0, TAGFS_BLOCK_SIZE);
    ft_block->magic       = TAGFS_FILETBL_MAGIC;
    ft_block->next_block  = 0;
    ft_block->entry_count = 0;
    ft_block->reserved    = 0;

    if (write_block(1, ft_block) != 0) {
        debug_printf("[TagFS] format: failed to write file table block\n");
        kfree(ft_block);
        kfree(bitmap);
        return -1;
    }
    kfree(ft_block);

    // --- Write initial MetaPoolBlock at block 2 ---
    MetaPoolBlock* mp_block = kmalloc(TAGFS_BLOCK_SIZE);
    if (!mp_block) {
        debug_printf("[TagFS] format: failed to allocate meta pool block\n");
        kfree(bitmap);
        return -1;
    }
    memset(mp_block, 0, TAGFS_BLOCK_SIZE);
    mp_block->magic        = TAGFS_MPOOL_MAGIC;
    mp_block->next_block   = 0;
    mp_block->used_bytes   = 0;
    mp_block->record_count = 0;

    if (write_block(2, mp_block) != 0) {
        debug_printf("[TagFS] format: failed to write meta pool block\n");
        kfree(mp_block);
        kfree(bitmap);
        return -1;
    }
    kfree(mp_block);

    // --- Write block bitmap to disk ---
    uint32_t bitmap_buf_size = bitmap_sectors * 512;
    uint8_t* bitmap_buf = kmalloc(bitmap_buf_size);
    if (!bitmap_buf) {
        debug_printf("[TagFS] format: failed to allocate bitmap write buffer\n");
        kfree(bitmap);
        return -1;
    }
    memset(bitmap_buf, 0, bitmap_buf_size);
    memcpy(bitmap_buf, bitmap, bitmap_bytes);

    if (ahci_write_sectors_sync(0, (uint64_t)bitmap_sector_start, (uint16_t)bitmap_sectors, bitmap_buf) != 0) {
        debug_printf("[TagFS] format: failed to write block bitmap\n");
        kfree(bitmap_buf);
        kfree(bitmap);
        return -1;
    }
    kfree(bitmap_buf);
    kfree(bitmap);

    // --- Fill superblock ---
    TagFSSuperblock sb;
    memset(&sb, 0, sizeof(sb));

    sb.magic                    = TAGFS_MAGIC;
    sb.version                  = TAGFS_VERSION;
    sb.block_size               = TAGFS_BLOCK_SIZE;
    sb.total_blocks             = total_blocks;
    sb.free_blocks              = total_blocks - 3;
    sb.total_files              = 0;
    sb.next_file_id             = 1;
    sb.next_tag_id              = 1;
    sb.total_tags               = 0;

    sb.tag_registry_block       = 0;
    sb.tag_registry_block_count = 1;
    sb.file_table_block         = 1;
    sb.file_table_block_count   = 1;
    sb.metadata_pool_block      = 2;
    sb.metadata_pool_block_count = 1;
    sb.block_bitmap_sector       = bitmap_sector_start;
    sb.block_bitmap_sector_count = bitmap_sectors;
    sb.journal_superblock_sector = TAGFS_JOURNAL_SB_SECTOR;

    sb.fs_created_time          = 0;
    sb.fs_modified_time         = 0;
    sb.backup_superblock_sector = TAGFS_BACKUP_SB_SECTOR;

    if (tagfs_write_superblock(&sb) != 0) {
        debug_printf("[TagFS] format: failed to write superblock\n");
        return -1;
    }

    // --- Write zeroed journal superblock ---
    uint8_t journal_sb_buf[512];
    memset(journal_sb_buf, 0, 512);
    // Write magic "JOUR" at offset 0 so it's identifiable
    uint32_t journal_magic = JOURNAL_MAGIC;
    memcpy(journal_sb_buf, &journal_magic, 4);
    if (ahci_write_sectors_sync(0, TAGFS_JOURNAL_SB_SECTOR, 1, journal_sb_buf) != 0) {
        debug_printf("[TagFS] format: warning — failed to write journal superblock\n");
    }

    debug_printf("[TagFS] Format complete: %u total blocks, %u free\n",
                 total_blocks, sb.free_blocks);
    return 0;
}

// ----------------------------------------------------------------------------
// tagfs_init
// ----------------------------------------------------------------------------

int tagfs_init(void) {
    if (g_state.initialized) {
        debug_printf("[TagFS] Already initialized\n");
        return 0;
    }

    debug_printf("[TagFS] Initializing...\n");

    spinlock_init(&g_state.lock);

    // --- Read superblock ---
    TagFSSuperblock sb;
    if (read_superblock(TAGFS_SUPERBLOCK_SECTOR, &sb) != 0 || sb.magic != TAGFS_MAGIC) {
        debug_printf("[TagFS] Primary superblock invalid, trying backup at sector %u\n",
                     TAGFS_BACKUP_SB_SECTOR);
        if (read_superblock(TAGFS_BACKUP_SB_SECTOR, &sb) != 0 || sb.magic != TAGFS_MAGIC) {
            debug_printf("[TagFS] CRITICAL: Both superblocks invalid — not formatted?\n");
            return -1;
        }
        debug_printf("[TagFS] Using backup superblock\n");
        // Restore primary
        write_superblock_to_sector(TAGFS_SUPERBLOCK_SECTOR, &sb);
    }

    if (sb.version != TAGFS_VERSION) {
        debug_printf("[TagFS] Unsupported version %u (expected %u)\n", sb.version, TAGFS_VERSION);
        return -1;
    }

    memcpy(&g_state.superblock, &sb, sizeof(TagFSSuperblock));
    debug_printf("[TagFS] Superblock OK: %u blocks, %u free, %u files\n",
                 sb.total_blocks, sb.free_blocks, sb.total_files);

    // --- Tag Registry ---
    g_state.registry = kmalloc(sizeof(TagRegistry));
    if (!g_state.registry) {
        debug_printf("[TagFS] Failed to allocate tag registry\n");
        return -1;
    }
    if (tag_registry_init(g_state.registry) != 0) {
        debug_printf("[TagFS] tag_registry_init failed\n");
        kfree(g_state.registry);
        g_state.registry = NULL;
        return -1;
    }
    if (tag_registry_load(g_state.registry, sb.tag_registry_block) != 0) {
        debug_printf("[TagFS] Warning: tag_registry_load failed (empty registry)\n");
    }

    // --- File Table ---
    if (file_table_init(sb.file_table_block, sb.file_table_block_count) != 0) {
        debug_printf("[TagFS] file_table_init failed\n");
        tag_registry_destroy(g_state.registry);
        kfree(g_state.registry);
        g_state.registry = NULL;
        return -1;
    }

    // --- Metadata Pool ---
    if (meta_pool_init(sb.metadata_pool_block, sb.metadata_pool_block_count) != 0) {
        debug_printf("[TagFS] meta_pool_init failed\n");
        file_table_shutdown();
        tag_registry_destroy(g_state.registry);
        kfree(g_state.registry);
        g_state.registry = NULL;
        return -1;
    }

    // --- Bitmap Index ---
    g_state.bitmap_index = tag_bitmap_create(64, 256);
    if (!g_state.bitmap_index) {
        debug_printf("[TagFS] tag_bitmap_create failed\n");
        meta_pool_shutdown();
        file_table_shutdown();
        tag_registry_destroy(g_state.registry);
        kfree(g_state.registry);
        g_state.registry = NULL;
        return -1;
    }

    // --- Block Bitmap ---
    uint32_t bitmap_bytes = (sb.total_blocks + 7) / 8;
    g_state.block_bitmap.bitmap = kmalloc(bitmap_bytes);
    if (!g_state.block_bitmap.bitmap) {
        debug_printf("[TagFS] Failed to allocate block bitmap\n");
        tag_bitmap_destroy(g_state.bitmap_index);
        g_state.bitmap_index = NULL;
        meta_pool_shutdown();
        file_table_shutdown();
        tag_registry_destroy(g_state.registry);
        kfree(g_state.registry);
        g_state.registry = NULL;
        return -1;
    }
    memset(g_state.block_bitmap.bitmap, 0, bitmap_bytes);
    g_state.block_bitmap.total_blocks = sb.total_blocks;
    g_state.block_bitmap.free_list    = NULL;
    g_state.block_bitmap.extent_count = 0;

    // Read block bitmap from disk
    uint32_t bm_sector_count = sb.block_bitmap_sector_count;
    uint32_t bm_buf_size     = bm_sector_count * 512;
    uint8_t* bm_buf = kmalloc(bm_buf_size);
    if (bm_buf) {
        if (ahci_read_sectors_sync(0, (uint64_t)sb.block_bitmap_sector, (uint16_t)bm_sector_count, bm_buf) == 0) {
            uint32_t copy_bytes = bitmap_bytes < bm_buf_size ? bitmap_bytes : bm_buf_size;
            memcpy(g_state.block_bitmap.bitmap, bm_buf, copy_bytes);
            debug_printf("[TagFS] Block bitmap loaded from disk\n");
        } else {
            debug_printf("[TagFS] Warning: failed to read block bitmap from disk\n");
        }
        kfree(bm_buf);
    }

    free_list_build();

    g_state.initialized = true;
    debug_printf("[TagFS] Initialized successfully\n");
    return 0;
}

// ----------------------------------------------------------------------------
// tagfs_sync / tagfs_shutdown
// ----------------------------------------------------------------------------

void tagfs_sync(void) {
    if (!g_state.initialized) return;

    tag_registry_flush(g_state.registry);
    file_table_flush();
    meta_pool_flush();

    // Write block bitmap to disk
    uint32_t bitmap_bytes   = (g_state.superblock.total_blocks + 7) / 8;
    uint32_t sector_count   = g_state.superblock.block_bitmap_sector_count;
    uint32_t bm_buf_size    = sector_count * 512;
    uint8_t* bm_buf = kmalloc(bm_buf_size);
    if (bm_buf) {
        memset(bm_buf, 0, bm_buf_size);
        uint32_t copy_bytes = bitmap_bytes < bm_buf_size ? bitmap_bytes : bm_buf_size;
        memcpy(bm_buf, g_state.block_bitmap.bitmap, copy_bytes);
        if (ahci_write_sectors_sync(0, (uint64_t)g_state.superblock.block_bitmap_sector,
                                   (uint16_t)sector_count, bm_buf) != 0) {
            debug_printf("[TagFS] sync: failed to write block bitmap\n");
        }
        kfree(bm_buf);
    }

    tagfs_write_superblock(&g_state.superblock);
}

void tagfs_shutdown(void) {
    if (!g_state.initialized) return;

    tagfs_sync();

    tag_registry_destroy(g_state.registry);
    kfree(g_state.registry);
    g_state.registry = NULL;

    tag_bitmap_destroy(g_state.bitmap_index);
    g_state.bitmap_index = NULL;

    file_table_shutdown();
    meta_pool_shutdown();

    free_list_destroy();
    if (g_state.block_bitmap.bitmap) {
        kfree(g_state.block_bitmap.bitmap);
        g_state.block_bitmap.bitmap = NULL;
    }
    g_state.block_bitmap.total_blocks = 0;

    g_state.initialized = false;
    debug_printf("[TagFS] Shutdown complete\n");
}

// ----------------------------------------------------------------------------
// tagfs_create_file
// ----------------------------------------------------------------------------

int tagfs_create_file(const char* filename, const uint16_t* tag_ids, uint16_t tag_count,
                      uint32_t* out_file_id) {
    if (!g_state.initialized) return -1;
    if (!filename || !out_file_id) return -1;

    spin_lock(&g_state.lock);

    uint32_t file_id = g_state.superblock.next_file_id++;

    // Build metadata
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    meta.file_id       = file_id;
    meta.flags         = TAGFS_FILE_ACTIVE;
    meta.size          = 0;
    meta.created_time  = 0;
    meta.modified_time = 0;
    meta.extent_count  = 0;
    meta.extents       = NULL;

    size_t name_len = strlen(filename);
    meta.filename = kmalloc(name_len + 1);
    if (!meta.filename) {
        debug_printf("[TagFS] create_file: kmalloc for filename failed\n");
        spin_unlock(&g_state.lock);
        return -1;
    }
    memcpy(meta.filename, filename, name_len + 1);

    meta.tag_count = tag_count;
    meta.tag_ids   = NULL;
    if (tag_count > 0 && tag_ids) {
        meta.tag_ids = kmalloc(sizeof(uint16_t) * tag_count);
        if (!meta.tag_ids) {
            debug_printf("[TagFS] create_file: kmalloc for tag_ids failed\n");
            kfree(meta.filename);
            spin_unlock(&g_state.lock);
            return -1;
        }
        memcpy(meta.tag_ids, tag_ids, sizeof(uint16_t) * tag_count);
    }

    // Write to metadata pool
    uint32_t meta_block, meta_offset;
    if (meta_pool_write(&meta, &meta_block, &meta_offset) != 0) {
        debug_printf("[TagFS] create_file: meta_pool_write failed for file_id=%u\n", file_id);
        kfree(meta.tag_ids);
        kfree(meta.filename);
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Update file table
    if (file_table_update(file_id, meta_block, meta_offset) != 0) {
        debug_printf("[TagFS] create_file: file_table_update failed for file_id=%u\n", file_id);
        kfree(meta.tag_ids);
        kfree(meta.filename);
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Add to bitmap index
    for (uint16_t i = 0; i < tag_count; i++) {
        if (meta.tag_ids[i] != TAGFS_INVALID_TAG_ID) {
            tag_bitmap_set(g_state.bitmap_index, meta.tag_ids[i], file_id);
        }
    }

    g_state.superblock.total_files++;

    *out_file_id = file_id;

    kfree(meta.tag_ids);
    kfree(meta.filename);

    spin_unlock(&g_state.lock);

    debug_printf("[TagFS] Created file '%s' file_id=%u\n", filename, file_id);
    return 0;
}

// ----------------------------------------------------------------------------
// tagfs_delete_file
// ----------------------------------------------------------------------------

int tagfs_delete_file(uint32_t file_id) {
    if (!g_state.initialized) return -1;

    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0) {
        debug_printf("[TagFS] delete_file: file_id=%u not found\n", file_id);
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Read metadata to get extents and tags for cleanup
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    int has_meta = meta_pool_read(meta_block, meta_offset, &meta);

    // Remove from bitmap index
    tag_bitmap_remove_file(g_state.bitmap_index, file_id);

    // Delete from metadata pool
    meta_pool_delete(meta_block, meta_offset);

    // Delete from file table
    file_table_delete(file_id);

    // Free data blocks if we could read metadata
    if (has_meta == 0 && meta.extents) {
        for (uint16_t i = 0; i < meta.extent_count; i++) {
            tagfs_free_blocks(meta.extents[i].start_block, meta.extents[i].block_count);
        }
        tagfs_metadata_free(&meta);
    }

    if (g_state.superblock.total_files > 0) {
        g_state.superblock.total_files--;
    }

    spin_unlock(&g_state.lock);

    debug_printf("[TagFS] Deleted file_id=%u\n", file_id);
    return 0;
}

// ----------------------------------------------------------------------------
// tagfs_rename_file
// ----------------------------------------------------------------------------

int tagfs_rename_file(uint32_t file_id, const char* new_filename) {
    if (!g_state.initialized) return -1;
    if (!new_filename) return -1;

    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0) {
        spin_unlock(&g_state.lock);
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0) {
        debug_printf("[TagFS] rename_file: failed to read metadata for file_id=%u\n", file_id);
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Replace filename
    if (meta.filename) kfree(meta.filename);
    size_t len = strlen(new_filename);
    meta.filename = kmalloc(len + 1);
    if (!meta.filename) {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }
    memcpy(meta.filename, new_filename, len + 1);

    // Delete old record, write new
    meta_pool_delete(meta_block, meta_offset);

    uint32_t new_block, new_offset;
    if (meta_pool_write(&meta, &new_block, &new_offset) != 0) {
        debug_printf("[TagFS] rename_file: meta_pool_write failed\n");
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }

    file_table_update(file_id, new_block, new_offset);

    tagfs_metadata_free(&meta);

    spin_unlock(&g_state.lock);
    return 0;
}

// ----------------------------------------------------------------------------
// Tag operations
// ----------------------------------------------------------------------------

int tagfs_add_tag(uint32_t file_id, uint16_t tag_id) {
    if (!g_state.initialized) return -1;

    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0) {
        spin_unlock(&g_state.lock);
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0) {
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Check if tag already present
    for (uint16_t i = 0; i < meta.tag_count; i++) {
        if (meta.tag_ids[i] == tag_id) {
            tagfs_metadata_free(&meta);
            spin_unlock(&g_state.lock);
            return 0;
        }
    }

    // Grow tag list
    uint16_t new_count = meta.tag_count + 1;
    uint16_t* new_ids  = kmalloc(sizeof(uint16_t) * new_count);
    if (!new_ids) {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }
    if (meta.tag_ids) {
        memcpy(new_ids, meta.tag_ids, sizeof(uint16_t) * meta.tag_count);
    }
    new_ids[meta.tag_count] = tag_id;
    kfree(meta.tag_ids);
    meta.tag_ids  = new_ids;
    meta.tag_count = new_count;

    meta_pool_delete(meta_block, meta_offset);

    uint32_t new_block, new_offset;
    int r = meta_pool_write(&meta, &new_block, &new_offset);
    if (r == 0) {
        file_table_update(file_id, new_block, new_offset);
        tag_bitmap_set(g_state.bitmap_index, tag_id, file_id);
    }

    tagfs_metadata_free(&meta);
    spin_unlock(&g_state.lock);
    return r;
}

int tagfs_remove_tag(uint32_t file_id, uint16_t tag_id) {
    if (!g_state.initialized) return -1;

    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0) {
        spin_unlock(&g_state.lock);
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0) {
        spin_unlock(&g_state.lock);
        return -1;
    }

    // Find and remove
    bool found = false;
    for (uint16_t i = 0; i < meta.tag_count; i++) {
        if (meta.tag_ids[i] == tag_id) {
            meta.tag_ids[i] = meta.tag_ids[meta.tag_count - 1];
            meta.tag_count--;
            found = true;
            break;
        }
    }

    if (!found) {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }

    meta_pool_delete(meta_block, meta_offset);

    uint32_t new_block, new_offset;
    int r = meta_pool_write(&meta, &new_block, &new_offset);
    if (r == 0) {
        file_table_update(file_id, new_block, new_offset);
        tag_bitmap_clear(g_state.bitmap_index, tag_id, file_id);
    }

    tagfs_metadata_free(&meta);
    spin_unlock(&g_state.lock);
    return r;
}

bool tagfs_has_tag(uint32_t file_id, uint16_t tag_id) {
    if (!g_state.initialized) return false;

    uint32_t results[1];
    uint16_t tag_arr[1] = { tag_id };
    int count = tag_bitmap_query(g_state.bitmap_index, tag_arr, 1, NULL, 0, results, 1);

    for (int i = 0; i < count; i++) {
        if (results[i] == file_id) return true;
    }
    return false;
}

int tagfs_add_tag_string(uint32_t file_id, const char* key, const char* value) {
    if (!g_state.initialized) return -1;
    if (!key) return -1;
    uint16_t tag_id = tag_registry_intern(g_state.registry, key, value);
    if (tag_id == TAGFS_INVALID_TAG_ID) return -1;
    return tagfs_add_tag(file_id, tag_id);
}

int tagfs_remove_tag_string(uint32_t file_id, const char* key) {
    if (!g_state.initialized) return -1;
    if (!key) return -1;
    uint16_t tag_id = tag_registry_lookup(g_state.registry, key, NULL);
    if (tag_id == TAGFS_INVALID_TAG_ID) return -1;
    return tagfs_remove_tag(file_id, tag_id);
}

bool tagfs_has_tag_string(uint32_t file_id, const char* key, const char* value) {
    if (!g_state.initialized) return false;
    if (!key) return false;
    uint16_t tag_id = tag_registry_lookup(g_state.registry, key, value);
    if (tag_id == TAGFS_INVALID_TAG_ID) return false;
    return tagfs_has_tag(file_id, tag_id);
}

// ----------------------------------------------------------------------------
// Query
// ----------------------------------------------------------------------------

int tagfs_query_files(const char* query_strings[], uint32_t count,
                      uint32_t* out_file_ids, uint32_t max_results) {
    if (!g_state.initialized) return 0;
    if (!query_strings || count == 0 || !out_file_ids || max_results == 0) return 0;

    uint16_t* tag_ids = kmalloc(sizeof(uint16_t) * count);
    if (!tag_ids) return 0;

    TagKeyGroup** groups = kmalloc(sizeof(TagKeyGroup*) * count);
    if (!groups) {
        kfree(tag_ids);
        return 0;
    }

    uint32_t tag_count   = 0;
    uint32_t group_count = 0;

    for (uint32_t i = 0; i < count; i++) {
        const char* qs = query_strings[i];
        if (!qs) continue;

        char key[128];
        char value[128];
        tagfs_parse_tag(qs, key, sizeof(key), value, sizeof(value));

        // Check for wildcard (value == "...")
        if (strcmp(value, "...") == 0) {
            TagKeyGroup* grp = tag_registry_key_group(g_state.registry, key);
            if (grp) {
                groups[group_count++] = grp;
            }
        } else {
            uint16_t tid = tag_registry_lookup(g_state.registry, key,
                                               value[0] ? value : NULL);
            if (tid == TAGFS_INVALID_TAG_ID) {
                // Tag not in registry — no files can match
                kfree(tag_ids);
                kfree(groups);
                return 0;
            }
            tag_ids[tag_count++] = tid;
        }
    }

    int result = tag_bitmap_query(g_state.bitmap_index,
                                  tag_ids, tag_count,
                                  groups, group_count,
                                  out_file_ids, max_results);

    kfree(tag_ids);
    kfree(groups);
    return result;
}

int tagfs_list_all_files(uint32_t* out_file_ids, uint32_t max_results) {
    if (!g_state.initialized) return 0;
    if (!out_file_ids || max_results == 0) return 0;

    uint32_t found = 0;
    uint32_t max_id = g_state.superblock.next_file_id;

    for (uint32_t fid = 1; fid < max_id && found < max_results; fid++) {
        uint32_t mb, mo;
        if (file_table_lookup(fid, &mb, &mo) == 0 && mb != 0) {
            out_file_ids[found++] = fid;
        }
    }

    return (int)found;
}

// ----------------------------------------------------------------------------
// File I/O (open / close / read / write)
// ----------------------------------------------------------------------------

TagFSFileHandle* tagfs_open(uint32_t file_id, uint32_t flags) {
    if (!g_state.initialized) return NULL;

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0) {
        debug_printf("[TagFS] open: file_id=%u not found\n", file_id);
        return NULL;
    }

    TagFSFileHandle* handle = kmalloc(sizeof(TagFSFileHandle));
    if (!handle) return NULL;

    handle->file_id      = file_id;
    handle->flags        = flags;
    handle->offset       = 0;
    handle->file_size    = 0;
    handle->extents      = NULL;
    handle->extent_count = 0;

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) == 0) {
        handle->file_size    = meta.size;
        handle->extent_count = meta.extent_count;
        if (meta.extent_count > 0 && meta.extents) {
            handle->extents = kmalloc(sizeof(FileExtent) * meta.extent_count);
            if (handle->extents) {
                memcpy(handle->extents, meta.extents,
                       sizeof(FileExtent) * meta.extent_count);
            }
        }
        tagfs_metadata_free(&meta);
    }

    return handle;
}

void tagfs_close(TagFSFileHandle* handle) {
    if (!handle) return;
    if (handle->extents) kfree(handle->extents);
    kfree(handle);
}

int tagfs_read(TagFSFileHandle* handle, void* buffer, uint64_t size) {
    if (!handle || !buffer || size == 0) return -1;
    if (!g_state.initialized) return -1;

    if (handle->offset >= handle->file_size) return 0;

    uint64_t remaining = handle->file_size - handle->offset;
    if (size > remaining) size = remaining;

    uint8_t block_buf[TAGFS_BLOCK_SIZE];
    uint8_t* out = (uint8_t*)buffer;
    uint64_t bytes_read = 0;

    while (bytes_read < size) {
        uint64_t file_pos = handle->offset + bytes_read;
        uint64_t extent_start = 0;
        int found = -1;

        for (uint16_t i = 0; i < handle->extent_count; i++) {
            uint64_t extent_size = (uint64_t)handle->extents[i].block_count * TAGFS_BLOCK_SIZE;
            if (file_pos < extent_start + extent_size) {
                found = i;
                break;
            }
            extent_start += extent_size;
        }

        if (found < 0) break;

        uint64_t offset_in_extent = file_pos - extent_start;
        uint32_t block_index      = (uint32_t)(offset_in_extent / TAGFS_BLOCK_SIZE);
        uint32_t offset_in_block  = (uint32_t)(offset_in_extent % TAGFS_BLOCK_SIZE);
        uint32_t disk_block       = handle->extents[found].start_block + block_index;

        if (read_block(disk_block, block_buf) != 0) {
            debug_printf("[TagFS] read: failed to read block %u\n", disk_block);
            break;
        }

        uint32_t chunk = TAGFS_BLOCK_SIZE - offset_in_block;
        if (chunk > size - bytes_read) chunk = (uint32_t)(size - bytes_read);

        memcpy(out + bytes_read, block_buf + offset_in_block, chunk);
        bytes_read += chunk;
    }

    handle->offset += bytes_read;
    return (int)bytes_read;
}

int tagfs_write(TagFSFileHandle* handle, const void* buffer, uint64_t size) {
    if (!handle || !buffer || size == 0) return -1;
    if (!g_state.initialized) return -1;
    if (!(handle->flags & TAGFS_HANDLE_WRITE)) return -1;

    const uint8_t* in = (const uint8_t*)buffer;
    uint8_t block_buf[TAGFS_BLOCK_SIZE];
    uint64_t bytes_written = 0;

    while (bytes_written < size) {
        uint64_t file_pos    = handle->offset + bytes_written;
        uint64_t extent_start = 0;
        int found = -1;

        for (uint16_t i = 0; i < handle->extent_count; i++) {
            uint64_t extent_size = (uint64_t)handle->extents[i].block_count * TAGFS_BLOCK_SIZE;
            if (file_pos < extent_start + extent_size) {
                found = i;
                break;
            }
            extent_start += extent_size;
        }

        if (found < 0) {
            uint32_t new_start;
            if (tagfs_alloc_blocks(1, &new_start) != 0) {
                debug_printf("[TagFS] write: block alloc failed\n");
                break;
            }

            uint16_t new_count = handle->extent_count + 1;
            FileExtent* new_extents = kmalloc(sizeof(FileExtent) * new_count);
            if (!new_extents) {
                tagfs_free_blocks(new_start, 1);
                break;
            }
            if (handle->extents && handle->extent_count > 0) {
                memcpy(new_extents, handle->extents, sizeof(FileExtent) * handle->extent_count);
                kfree(handle->extents);
            }
            new_extents[handle->extent_count].start_block = new_start;
            new_extents[handle->extent_count].block_count = 1;
            handle->extents      = new_extents;
            handle->extent_count = new_count;

            found        = handle->extent_count - 1;
            extent_start = file_pos;
        }

        uint64_t offset_in_extent = file_pos - extent_start;
        uint32_t block_index      = (uint32_t)(offset_in_extent / TAGFS_BLOCK_SIZE);
        uint32_t offset_in_block  = (uint32_t)(offset_in_extent % TAGFS_BLOCK_SIZE);
        uint32_t disk_block       = handle->extents[found].start_block + block_index;

        if (offset_in_block != 0 || (size - bytes_written) < TAGFS_BLOCK_SIZE) {
            if (read_block(disk_block, block_buf) != 0) {
                memset(block_buf, 0, TAGFS_BLOCK_SIZE);
            }
        }

        uint32_t chunk = TAGFS_BLOCK_SIZE - offset_in_block;
        if (chunk > size - bytes_written) chunk = (uint32_t)(size - bytes_written);

        memcpy(block_buf + offset_in_block, in + bytes_written, chunk);

        if (write_block(disk_block, block_buf) != 0) {
            debug_printf("[TagFS] write: failed to write block %u\n", disk_block);
            break;
        }

        bytes_written += chunk;
    }

    handle->offset += bytes_written;
    if (handle->offset > handle->file_size) {
        handle->file_size = handle->offset;
    }

    if (bytes_written > 0) {
        uint32_t meta_block, meta_offset;
        if (file_table_lookup(handle->file_id, &meta_block, &meta_offset) == 0) {
            TagFSMetadata meta;
            memset(&meta, 0, sizeof(meta));
            if (meta_pool_read(meta_block, meta_offset, &meta) == 0) {
                meta.size = handle->file_size;
                if (meta.extents) kfree(meta.extents);
                meta.extent_count = handle->extent_count;
                meta.extents = kmalloc(sizeof(FileExtent) * handle->extent_count);
                if (meta.extents) {
                    memcpy(meta.extents, handle->extents,
                           sizeof(FileExtent) * handle->extent_count);
                }
                meta_pool_delete(meta_block, meta_offset);
                uint32_t new_mb, new_mo;
                if (meta_pool_write(&meta, &new_mb, &new_mo) == 0) {
                    file_table_update(handle->file_id, new_mb, new_mo);
                }
                tagfs_metadata_free(&meta);
            }
        }
    }

    return (int)bytes_written;
}

// ----------------------------------------------------------------------------
// Block allocator
// ----------------------------------------------------------------------------

int tagfs_alloc_blocks(uint32_t count, uint32_t* out_start_block) {
    if (!g_state.initialized) return -1;
    if (count == 0 || !out_start_block) return -1;

    spin_lock(&g_state.lock);

    FreeExtent* prev   = NULL;
    FreeExtent* cur    = g_state.block_bitmap.free_list;

    while (cur) {
        if (cur->count >= count) {
            *out_start_block = cur->start;

            // Mark bits used
            for (uint32_t i = 0; i < count; i++) {
                bitmap_set_bit(g_state.block_bitmap.bitmap, cur->start + i);
            }

            if (cur->count == count) {
                // Remove extent from list
                if (prev) prev->next = cur->next;
                else       g_state.block_bitmap.free_list = cur->next;
                kfree(cur);
                g_state.block_bitmap.extent_count--;
            } else {
                cur->start += count;
                cur->count -= count;
            }

            if (g_state.superblock.free_blocks >= count) {
                g_state.superblock.free_blocks -= count;
            }

            spin_unlock(&g_state.lock);
            return 0;
        }
        prev = cur;
        cur  = cur->next;
    }

    debug_printf("[TagFS] alloc_blocks: no free extent of size %u\n", count);
    spin_unlock(&g_state.lock);
    return -1;
}

int tagfs_free_blocks(uint32_t start_block, uint32_t count) {
    if (!g_state.initialized) return -1;
    if (count == 0) return 0;

    spin_lock(&g_state.lock);

    uint32_t end = start_block + count;

    // Clear bits
    for (uint32_t i = 0; i < count; i++) {
        bitmap_clear_bit(g_state.block_bitmap.bitmap, start_block + i);
    }
    g_state.superblock.free_blocks += count;

    // Insert into free list in sorted order, merging adjacent extents
    FreeExtent* prev = NULL;
    FreeExtent* cur  = g_state.block_bitmap.free_list;

    while (cur && cur->start < start_block) {
        prev = cur;
        cur  = cur->next;
    }

    // Check merge right (new extent is adjacent to cur from left)
    bool merge_right = cur && cur->start == end;
    // Check merge left (prev is adjacent to new extent from right)
    bool merge_left  = prev && (prev->start + prev->count) == start_block;

    if (merge_left && merge_right) {
        prev->count += count + cur->count;
        prev->next = cur->next;
        kfree(cur);
        g_state.block_bitmap.extent_count--;
    } else if (merge_left) {
        prev->count += count;
    } else if (merge_right) {
        cur->start  = start_block;
        cur->count += count;
    } else {
        FreeExtent* ext = kmalloc(sizeof(FreeExtent));
        if (!ext) {
            debug_printf("[TagFS] free_blocks: kmalloc for extent failed\n");
            spin_unlock(&g_state.lock);
            return -1;
        }
        ext->start = start_block;
        ext->count = count;
        ext->next  = cur;
        if (prev) prev->next = ext;
        else       g_state.block_bitmap.free_list = ext;
        g_state.block_bitmap.extent_count++;
    }

    spin_unlock(&g_state.lock);
    return 0;
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

int tagfs_get_metadata(uint32_t file_id, TagFSMetadata* out) {
    if (!g_state.initialized) return -1;
    if (!out) return -1;
    uint32_t block, offset;
    if (file_table_lookup(file_id, &block, &offset) != 0) return -1;
    return meta_pool_read(block, offset, out);
}

TagFSState* tagfs_get_state(void) {
    return &g_state;
}

void tagfs_format_tag(char* dest, size_t dest_size, const char* key, const char* value) {
    if (!dest || dest_size == 0 || !key) return;
    if (value && value[0]) {
        ksnprintf(dest, dest_size, "%s:%s", key, value);
    } else {
        strncpy(dest, key, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
}

int tagfs_parse_tag(const char* tag_string, char* key, size_t key_size,
                    char* value, size_t value_size) {
    if (!tag_string || !key || !value || key_size == 0 || value_size == 0) return -1;

    const char* colon = strchr(tag_string, ':');
    if (colon) {
        size_t klen = (size_t)(colon - tag_string);
        if (klen >= key_size) klen = key_size - 1;
        memcpy(key, tag_string, klen);
        key[klen] = '\0';

        strncpy(value, colon + 1, value_size - 1);
        value[value_size - 1] = '\0';
    } else {
        strncpy(key, tag_string, key_size - 1);
        key[key_size - 1] = '\0';
        value[0] = '\0';
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Defrag
// ----------------------------------------------------------------------------

int tagfs_defrag_file(uint32_t file_id, uint32_t target_block) {
    if (!g_state.initialized) return -1;

    spin_lock(&g_state.lock);

    uint32_t meta_block, meta_offset;
    if (file_table_lookup(file_id, &meta_block, &meta_offset) != 0) {
        spin_unlock(&g_state.lock);
        return -1;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (meta_pool_read(meta_block, meta_offset, &meta) != 0) {
        spin_unlock(&g_state.lock);
        return -1;
    }

    if (meta.extent_count <= 1) {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return 0;
    }

    uint32_t total_blocks = 0;
    for (uint16_t i = 0; i < meta.extent_count; i++) {
        total_blocks += meta.extents[i].block_count;
    }

    uint32_t new_start;
    (void)target_block;
    spin_unlock(&g_state.lock);
    if (tagfs_alloc_blocks(total_blocks, &new_start) != 0) {
        tagfs_metadata_free(&meta);
        return -1;
    }
    spin_lock(&g_state.lock);

    uint8_t block_buf[TAGFS_BLOCK_SIZE];
    uint32_t dest_block  = new_start;
    bool copy_failed     = false;

    for (uint16_t i = 0; i < meta.extent_count && !copy_failed; i++) {
        for (uint16_t b = 0; b < meta.extents[i].block_count && !copy_failed; b++) {
            if (read_block(meta.extents[i].start_block + b, block_buf) != 0 ||
                write_block(dest_block, block_buf) != 0) {
                copy_failed = true;
                break;
            }
            dest_block++;
        }
    }

    if (copy_failed) {
        spin_unlock(&g_state.lock);
        tagfs_free_blocks(new_start, total_blocks);
        tagfs_metadata_free(&meta);
        return -1;
    }

    for (uint16_t i = 0; i < meta.extent_count; i++) {
        for (uint16_t b = 0; b < meta.extents[i].block_count; b++) {
            bitmap_clear_bit(g_state.block_bitmap.bitmap,
                             meta.extents[i].start_block + b);
        }
        g_state.superblock.free_blocks += meta.extents[i].block_count;
    }

    if (meta.extents) kfree(meta.extents);
    meta.extents = kmalloc(sizeof(FileExtent));
    if (!meta.extents) {
        tagfs_metadata_free(&meta);
        spin_unlock(&g_state.lock);
        return -1;
    }
    meta.extents[0].start_block = new_start;
    meta.extents[0].block_count = (uint16_t)total_blocks;
    meta.extent_count = 1;

    meta_pool_delete(meta_block, meta_offset);
    uint32_t new_mb, new_mo;
    if (meta_pool_write(&meta, &new_mb, &new_mo) == 0) {
        file_table_update(file_id, new_mb, new_mo);
    }

    tagfs_metadata_free(&meta);
    spin_unlock(&g_state.lock);
    return 0;
}

uint32_t tagfs_get_fragmentation_score(void) {
    if (!g_state.initialized) return 0;

    uint32_t score  = 0;
    uint32_t max_id = g_state.superblock.next_file_id;

    for (uint32_t fid = 1; fid < max_id; fid++) {
        uint32_t mb, mo;
        if (file_table_lookup(fid, &mb, &mo) != 0 || mb == 0) continue;

        TagFSMetadata meta;
        memset(&meta, 0, sizeof(meta));
        if (meta_pool_read(mb, mo, &meta) == 0) {
            if (meta.extent_count > 1) {
                score += (meta.extent_count - 1);
            }
            tagfs_metadata_free(&meta);
        }
    }

    return score;
}

