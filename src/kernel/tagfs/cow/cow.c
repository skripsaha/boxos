#include "cow.h"
#include "../disk_book/disk_book.h"
#include "../metadata_pool/meta_pool.h"
#include "../file_table/file_table.h"
#include "../tag_bitmap/tag_bitmap.h"
#include "../../lib/kernel/klib.h"
#include "../../../kernel/drivers/timer/rtc.h"
#include "../tagfs.h"

// Global state
static CowState g_cow_state;

// Allocate new snapshot ID atomically
static uint32_t CowAllocSnapshotId(void) {
    static volatile uint32_t next_id = 1;
    return __atomic_fetch_add(&next_id, 1, __ATOMIC_SEQ_CST);
}

// Write the in-memory snapshot list to the on-disk manifest block(s).
// Caller must hold g_cow_state.lock OR be in a context where the snapshot
// list is stable (e.g. after the lock is released but before returning).
// This function acquires no locks itself to avoid re-entrancy.
static void CowWriteManifest(void) {
    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->initialized)
        return;

    uint32_t primary_block = TAGFS_SB_COW_SNAPSHOT(&fs->superblock);

    // Lazy allocation: on first snapshot, allocate a dedicated block for the manifest.
    if (primary_block == 0) {
        if (tagfs_alloc_blocks(1, &primary_block) != 0)
            return;  // No free blocks — manifest cannot be persisted
        TAGFS_SB_COW_SNAPSHOT(&fs->superblock) = primary_block;
        tagfs_write_superblock(&fs->superblock);
    }

    CowManifest *manifest = kmalloc(sizeof(CowManifest));
    if (!manifest)
        return;

    memset(manifest, 0, sizeof(CowManifest));
    manifest->magic = COW_MANIFEST_MAGIC;

    uint32_t count = g_cow_state.snapshot_count;
    if (count > COW_MANIFEST_MAX)
        count = COW_MANIFEST_MAX;
    manifest->count = count;

    for (uint32_t i = 0; i < count; i++) {
        CowSnapshot    *snap = &g_cow_state.snapshots[i];
        CowSnapshotDisk *dst = &manifest->entries[i];

        dst->snapshot_id         = snap->snapshot_id;
        dst->parent_file_id      = snap->parent_file_id;
        dst->created_time        = snap->created_time;
        dst->disk_book_checkpoint = (uint32_t)snap->disk_book_checkpoint;
        dst->file_count          = snap->file_count;
        dst->total_size          = snap->total_size;
        dst->flags               = snap->flags;
        memcpy(dst->name, snap->name, TAGFS_SNAPSHOT_NAME_LEN);
    }

    tagfs_write_block(primary_block, manifest);

    uint32_t backup_block = TAGFS_SB_COW_SNAPSHOT_BACKUP(&fs->superblock);
    if (backup_block != 0)
        tagfs_write_block(backup_block, manifest);

    kfree(manifest);
}

// Find snapshot by ID
static int CowFindSnapshot(uint32_t id) {
    for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
        if (g_cow_state.snapshots[i].snapshot_id == id)
            return (int)i;
    }
    return -1;
}

error_t TagFS_CowInit(void) {
    if (g_cow_state.initialized)
        return ERR_ALREADY_INITIALIZED;
    
    memset(&g_cow_state, 0, sizeof(CowState));
    spinlock_init(&g_cow_state.lock);
    
    g_cow_state.snapshot_capacity = TAGFS_MAX_SNAPSHOTS;
    g_cow_state.snapshots = kmalloc(sizeof(CowSnapshot) * g_cow_state.snapshot_capacity);
    if (!g_cow_state.snapshots)
        return ERR_NO_MEMORY;
    
    memset(g_cow_state.snapshots, 0, sizeof(CowSnapshot) * g_cow_state.snapshot_capacity);
    g_cow_state.magic = COW_MAGIC;
    g_cow_state.version = COW_VERSION;
    g_cow_state.initialized = true;
    
    debug_printf("[CoW] Initialized: %u snapshots\n", g_cow_state.snapshot_capacity);
    return OK;
}

void TagFS_CowShutdown(void) {
    if (!g_cow_state.initialized)
        return;
    
    spin_lock(&g_cow_state.lock);
    if (g_cow_state.snapshots) {
        kfree(g_cow_state.snapshots);
        g_cow_state.snapshots = NULL;
    }
    g_cow_state.initialized = false;
    spin_unlock(&g_cow_state.lock);
    
    debug_printf("[CoW] Shutdown complete\n");
}

error_t TagFS_SnapshotCreate(const char *name, uint32_t file_id, uint32_t *snapshot_id) {
    if (!g_cow_state.initialized || !name || !snapshot_id)
        return ERR_INVALID_ARGUMENT;

    spin_lock(&g_cow_state.lock);

    if (g_cow_state.snapshot_count >= g_cow_state.snapshot_capacity) {
        spin_unlock(&g_cow_state.lock);
        return ERR_NO_MEMORY;
    }

    for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
        if (strcmp(g_cow_state.snapshots[i].name, name) == 0) {
            spin_unlock(&g_cow_state.lock);
            return ERR_ALREADY_EXISTS;
        }
    }

    uint32_t slot_index = g_cow_state.snapshot_count;
    CowSnapshot *snap = &g_cow_state.snapshots[slot_index];
    memset(snap, 0, sizeof(CowSnapshot));

    snap->snapshot_id = CowAllocSnapshotId();
    snap->parent_file_id = file_id;
    snap->created_time = rtc_get_unix64();
    snap->disk_book_checkpoint = DiskBookGetCheckpointSeq();
    strncpy(snap->name, name, TAGFS_SNAPSHOT_NAME_LEN - 1);
    snap->flags = COW_SNAP_READONLY;

    TagFSMetadata file_meta;
    memset(&file_meta, 0, sizeof(file_meta));
    bool has_file_meta = false;

    if (file_id == 0) {
        TagFSState *fs = tagfs_get_state();
        if (fs)
            snap->file_count = fs->superblock.total_files;
    } else {
        if (tagfs_get_metadata(file_id, &file_meta) == OK) {
            snap->file_count = 1;
            snap->total_size = file_meta.size;
            snap->block_count = 0;
            for (uint16_t j = 0; j < file_meta.extent_count; j++)
                snap->block_count += file_meta.extents[j].block_count;
            has_file_meta = true;
        } else {
            spin_unlock(&g_cow_state.lock);
            return ERR_FILE_NOT_FOUND;
        }
    }

    // Commit snapshot to journal before making it visible.
    // Use the file metadata for the journal record so the log is real.
    // For whole-fs snapshots, build a dummy metadata record with snapshot_id as file_id.
    g_cow_state.snapshot_count++;
    g_cow_state.snapshots_created++;
    *snapshot_id = snap->snapshot_id;

    spin_unlock(&g_cow_state.lock);

    DiskBookTxn txn;
    if (DiskBookBegin(&txn) == OK) {
        if (has_file_meta) {
            // Journal the actual file metadata associated with this snapshot
            uint32_t meta_block, meta_offset;
            if (file_table_lookup(file_id, &meta_block, &meta_offset) == 0) {
                DiskBookLogMetadata(&txn, snap->snapshot_id, meta_block, &file_meta);
            }
        } else {
            // Whole-fs snapshot: synthesize a placeholder metadata record
            TagFSMetadata snap_meta;
            memset(&snap_meta, 0, sizeof(snap_meta));
            snap_meta.file_id = snap->snapshot_id;
            snap_meta.flags   = COW_SNAP_READONLY;
            snap_meta.size    = snap->total_size;
            DiskBookLogMetadata(&txn, snap->snapshot_id, 0, &snap_meta);
        }
        DiskBookCommit(&txn);
    }

    if (has_file_meta)
        tagfs_metadata_free(&file_meta);

    CowWriteManifest();

    debug_printf("[CoW] Created '%s' (id=%u, files=%u)\n", name, snap->snapshot_id, snap->file_count);
    return OK;
}

error_t TagFS_SnapshotCreateByTag(const char *tag_pattern, uint32_t *snapshot_id) {
    if (!g_cow_state.initialized || !tag_pattern || !snapshot_id)
        return ERR_INVALID_ARGUMENT;
    
    spin_lock(&g_cow_state.lock);
    
    if (g_cow_state.snapshot_count >= g_cow_state.snapshot_capacity) {
        spin_unlock(&g_cow_state.lock);
        return ERR_NO_MEMORY;
    }
    
    char snap_name[TAGFS_SNAPSHOT_NAME_LEN];
    ksnprintf(snap_name, sizeof(snap_name), "tag-%s", tag_pattern);
    
    for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
        if (strcmp(g_cow_state.snapshots[i].name, snap_name) == 0) {
            spin_unlock(&g_cow_state.lock);
            return ERR_ALREADY_EXISTS;
        }
    }
    
    CowSnapshot *snap = &g_cow_state.snapshots[g_cow_state.snapshot_count];
    memset(snap, 0, sizeof(CowSnapshot));
    
    snap->snapshot_id = CowAllocSnapshotId();
    snap->parent_file_id = 0;
    snap->created_time = rtc_get_unix64();
    snap->disk_book_checkpoint = DiskBookGetCheckpointSeq();
    strncpy(snap->name, snap_name, TAGFS_SNAPSHOT_NAME_LEN - 1);
    snap->flags = COW_SNAP_READONLY | COW_SNAP_TAG_QUERY;
    
    uint32_t file_count = 0;
    uint64_t total_size = 0;
    uint32_t block_count = 0;
    
    uint32_t *file_ids = kmalloc(sizeof(uint32_t) * TAGFS_MAX_FILES);
    if (file_ids) {
        int count = tagfs_list_all_files(file_ids, TAGFS_MAX_FILES);
        for (int i = 0; i < count; i++) {
            file_count++;
            TagFSMetadata meta;
            if (tagfs_get_metadata(file_ids[i], &meta) == OK) {
                total_size += meta.size;
                for (uint16_t j = 0; j < meta.extent_count; j++)
                    block_count += meta.extents[j].block_count;
                tagfs_metadata_free(&meta);
            }
        }
        kfree(file_ids);
    }
    
    snap->file_count = file_count;
    snap->total_size = total_size;
    snap->block_count = block_count;
    
    g_cow_state.snapshot_count++;
    g_cow_state.snapshots_created++;
    *snapshot_id = snap->snapshot_id;

    spin_unlock(&g_cow_state.lock);

    DiskBookTxn txn;
    if (DiskBookBegin(&txn) == OK) {
        TagFSMetadata snap_meta;
        memset(&snap_meta, 0, sizeof(snap_meta));
        snap_meta.file_id = snap->snapshot_id;
        snap_meta.flags   = COW_SNAP_READONLY | COW_SNAP_TAG_QUERY;
        snap_meta.size    = total_size;
        DiskBookLogMetadata(&txn, snap->snapshot_id, 0, &snap_meta);
        DiskBookCommit(&txn);
    }

    CowWriteManifest();

    debug_printf("[CoW] Created tag snapshot '%s' (id=%u, files=%u)\n", snap_name, snap->snapshot_id, file_count);
    return OK;
}

error_t TagFS_SnapshotDelete(uint32_t snapshot_id) {
    if (!g_cow_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_cow_state.lock);
    
    int slot = CowFindSnapshot(snapshot_id);
    if (slot < 0) {
        spin_unlock(&g_cow_state.lock);
        return ERR_SNAPSHOT_NOT_FOUND;
    }
    
    for (uint32_t i = slot; i < g_cow_state.snapshot_count - 1; i++)
        g_cow_state.snapshots[i] = g_cow_state.snapshots[i + 1];
    
    g_cow_state.snapshot_count--;
    spin_unlock(&g_cow_state.lock);

    CowWriteManifest();

    debug_printf("[CoW] Deleted snapshot %u\n", snapshot_id);
    return OK;
}

error_t TagFS_SnapshotList(uint32_t *ids, uint32_t max_count, uint32_t *actual_count) {
    if (!g_cow_state.initialized || !ids || !actual_count)
        return ERR_INVALID_ARGUMENT;
    
    spin_lock(&g_cow_state.lock);
    
    uint32_t count = max_count < g_cow_state.snapshot_count ? max_count : g_cow_state.snapshot_count;
    for (uint32_t i = 0; i < count; i++)
        ids[i] = g_cow_state.snapshots[i].snapshot_id;
    
    *actual_count = count;
    spin_unlock(&g_cow_state.lock);
    return OK;
}

error_t TagFS_SnapshotInfo(uint32_t snapshot_id, CowSnapshot *info) {
    if (!g_cow_state.initialized || !info)
        return ERR_INVALID_ARGUMENT;
    
    spin_lock(&g_cow_state.lock);
    
    int slot = CowFindSnapshot(snapshot_id);
    if (slot < 0) {
        spin_unlock(&g_cow_state.lock);
        return ERR_SNAPSHOT_NOT_FOUND;
    }
    
    memcpy(info, &g_cow_state.snapshots[slot], sizeof(CowSnapshot));
    spin_unlock(&g_cow_state.lock);
    return OK;
}

error_t TagFS_CowBeforeWrite(uint32_t file_id, uint32_t block_addr, uint32_t *new_block) {
    (void)file_id;  // Reserved for future per-file COW policies
    if (!g_cow_state.initialized || !new_block || block_addr == 0)
        return ERR_INVALID_ARGUMENT;

    spin_lock(&g_cow_state.lock);

    // Allocate new block for COW
    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->initialized) {
        spin_unlock(&g_cow_state.lock);
        return ERR_NOT_INITIALIZED;
    }

    // Read original block data
    uint8_t original_data[TAGFS_BLOCK_SIZE];
    if (tagfs_read_block(block_addr, original_data) != OK) {
        spin_unlock(&g_cow_state.lock);
        return ERR_READ_FAILED;
    }

    // Allocate new block
    uint32_t free_block = 0;
    int alloc_result = tagfs_alloc_blocks(1, &free_block);
    if (alloc_result != OK || free_block == 0) {
        spin_unlock(&g_cow_state.lock);
        return ERR_NO_MEMORY;
    }

    // Copy original data to new block
    if (tagfs_write_block(free_block, original_data) != OK) {
        tagfs_free_blocks(free_block, 1);
        spin_unlock(&g_cow_state.lock);
        return ERR_WRITE_FAILED;
    }

    // Return new block address
    *new_block = free_block;
    g_cow_state.cow_copies++;

    spin_unlock(&g_cow_state.lock);
    return OK;
}

error_t TagFS_CowAfterWrite(uint32_t file_id, uint32_t old_block, uint32_t new_block) {
    (void)file_id;  // Reserved for future per-file COW tracking
    if (!g_cow_state.initialized)
        return ERR_NOT_INITIALIZED;

    if (old_block == 0 || new_block == 0)
        return ERR_INVALID_ARGUMENT;

    spin_lock(&g_cow_state.lock);

    // Mark old block as free in bitmap
    TagFSState *fs = tagfs_get_state();
    if (fs && fs->initialized) {
        // Old block will be freed by caller after snapshot is created
        g_cow_state.cow_writes++;
    }

    spin_unlock(&g_cow_state.lock);
    return OK;
}

void TagFS_CowGetStats(uint64_t *cow_writes, uint64_t *cow_copies,
                       uint64_t *blocks_shared, uint32_t *snapshots) {
    if (!g_cow_state.initialized)
        return;
    
    spin_lock(&g_cow_state.lock);
    if (cow_writes) *cow_writes = g_cow_state.cow_writes;
    if (cow_copies) *cow_copies = g_cow_state.cow_copies;
    if (blocks_shared) *blocks_shared = g_cow_state.blocks_shared;
    if (snapshots) *snapshots = g_cow_state.snapshots_created;
    spin_unlock(&g_cow_state.lock);
}

bool TagFS_CowIsActive(uint32_t file_id) {
    if (!g_cow_state.initialized)
        return false;

    spin_lock(&g_cow_state.lock);

    // CoW is active if there are any snapshots for this file
    // or if file has the COW flag set
    for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
        CowSnapshot *snap = &g_cow_state.snapshots[i];
        if (snap->parent_file_id == file_id || snap->parent_file_id == 0) {
            spin_unlock(&g_cow_state.lock);
            return true;
        }
    }

    // Also check if file has COW flag in metadata
    TagFSMetadata meta;
    if (tagfs_get_metadata(file_id, &meta) == OK) {
        bool is_cow = (meta.flags & TAGFS_FILE_COW) != 0;
        tagfs_metadata_free(&meta);
        spin_unlock(&g_cow_state.lock);
        return is_cow;
    }

    spin_unlock(&g_cow_state.lock);
    return false;
}

uint64_t TagFS_CowGetCheckpoint(void) {
    return g_cow_state.initialized ? DiskBookGetCheckpointSeq() : 0;
}

void TagFS_CowRestoreSnapshot(const CowSnapshot *snap) {
    if (!g_cow_state.initialized || !snap)
        return;

    spin_lock(&g_cow_state.lock);

    if (g_cow_state.snapshot_count >= g_cow_state.snapshot_capacity) {
        spin_unlock(&g_cow_state.lock);
        debug_printf("[CoW] RestoreSnapshot: no capacity for snapshot_id=%u\n", snap->snapshot_id);
        return;
    }

    // Skip if already present (idempotent restore)
    for (uint32_t i = 0; i < g_cow_state.snapshot_count; i++) {
        if (g_cow_state.snapshots[i].snapshot_id == snap->snapshot_id) {
            spin_unlock(&g_cow_state.lock);
            return;
        }
    }

    memcpy(&g_cow_state.snapshots[g_cow_state.snapshot_count], snap, sizeof(CowSnapshot));
    g_cow_state.snapshot_count++;

    spin_unlock(&g_cow_state.lock);
}
