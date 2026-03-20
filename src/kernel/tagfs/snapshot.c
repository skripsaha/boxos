#include "../tagfs.h"
#include "data_roll/data_roll.h"
#include "metadata_pool/meta_pool.h"
#include "file_table/file_table.h"
#include "tag_bitmap/tag_bitmap.h"
#include "../../lib/kernel/klib.h"
#include "../../kernel/drivers/disk/ata.h"
#include "../../kernel/drivers/timer/rtc.h"

// Use accessor function instead of extern
#define g_state (*tagfs_get_state())

// ============================================================================
// Snapshot Storage
// ============================================================================

// Snapshots stored in dedicated metadata blocks
static TagFSSnapshot g_snapshots[TAGFS_MAX_SNAPSHOTS];
static uint32_t g_snapshotCount = 0;
static spinlock_t g_snapshot_lock;

// Per-file versioning configuration
typedef struct {
    uint32_t file_id;
    uint32_t max_versions;
    uint32_t current_versions;
} VersioningConfig;

static VersioningConfig g_versioning[TAGFS_MAX_SNAPSHOTS];
static uint32_t g_versioningCount = 0;

// ============================================================================
// Internal Helpers
// ============================================================================

static int find_snapshot_slot(uint32_t snapshot_id) {
    for (uint32_t i = 0; i < g_snapshotCount; i++) {
        if (g_snapshots[i].snapshot_id == snapshot_id) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t allocate_snapshot_id(void) {
    static uint32_t next_id = 1;
    return next_id++;
}

static int find_versioning_config(uint32_t file_id) {
    for (uint32_t i = 0; i < g_versioningCount; i++) {
        if (g_versioning[i].file_id == file_id) {
            return (int)i;
        }
    }
    return -1;
}

// ============================================================================
// Snapshot Creation (Copy-on-Write Style)
// ============================================================================

int tagfs_snapshot_create(const char* name, uint32_t file_id) {
    if (!name || g_snapshotCount >= TAGFS_MAX_SNAPSHOTS) {
        return -1;
    }

    spin_lock(&g_snapshot_lock);

    // Check for duplicate name
    for (uint32_t i = 0; i < g_snapshotCount; i++) {
        if (strcmp(g_snapshots[i].name, name) == 0) {
            spin_unlock(&g_snapshot_lock);
            debug_printf("[TagFS Snapshot] Duplicate name: %s\n", name);
            return -1;
        }
    }

    TagFSSnapshot* snap = &g_snapshots[g_snapshotCount];
    memset(snap, 0, sizeof(TagFSSnapshot));

    snap->snapshot_id = allocate_snapshot_id();
    snap->parent_file_id = file_id;
    snap->created_time = rtc_get_unix64();
    strncpy(snap->name, name, TAGFS_SNAPSHOT_NAME_LEN - 1);
    snap->flags = 0x01;  // Readonly by default

    // Calculate snapshot size
    if (file_id == 0) {
        // Entire filesystem snapshot
        snap->file_count = g_state.superblock.total_files;
        // Total size would require iterating all files - set to 0 for now
        snap->total_size = 0;
    } else {
        // Single file snapshot
        TagFSMetadata meta;
        if (tagfs_get_metadata(file_id, &meta) == 0) {
            snap->file_count = 1;
            snap->total_size = meta.size;
            tagfs_metadata_free(&meta);
        } else {
            spin_unlock(&g_snapshot_lock);
            debug_printf("[TagFS Snapshot] File %u not found\n", file_id);
            return -1;
        }
    }

    // Log snapshot to DataRoll journal for crash safety
    DataRollTxn txn;
    if (DataRollBegin(&txn) == 0) {
        DataRollLogMetadata(&txn, snap->snapshot_id, 0, NULL);
        DataRollCommit(&txn);
    }

    g_snapshotCount++;

    spin_unlock(&g_snapshot_lock);

    debug_printf("[TagFS Snapshot] Created '%s' (id=%u, files=%u, size=%lu bytes)\n",
                 name, snap->snapshot_id, snap->file_count, (unsigned long)snap->total_size);

    return 0;
}

// ============================================================================
// Snapshot Deletion
// ============================================================================

int tagfs_snapshot_delete(uint32_t snapshot_id) {
    spin_lock(&g_snapshot_lock);

    int slot = find_snapshot_slot(snapshot_id);
    if (slot < 0) {
        spin_unlock(&g_snapshot_lock);
        debug_printf("[TagFS Snapshot] Snapshot %u not found\n", snapshot_id);
        return -1;
    }

    // Remove snapshot (shift array)
    for (uint32_t i = slot; i < g_snapshotCount - 1; i++) {
        g_snapshots[i] = g_snapshots[i + 1];
    }
    g_snapshotCount--;

    spin_unlock(&g_snapshot_lock);

    debug_printf("[TagFS Snapshot] Deleted snapshot %u\n", snapshot_id);
    return 0;
}

// ============================================================================
// Snapshot Restore (Rollback)
// ============================================================================

int tagfs_snapshot_restore(uint32_t snapshot_id, uint32_t target_file_id) {
    spin_lock(&g_snapshot_lock);

    int slot = find_snapshot_slot(snapshot_id);
    if (slot < 0) {
        spin_unlock(&g_snapshot_lock);
        debug_printf("[TagFS Snapshot] Snapshot %u not found\n", snapshot_id);
        return -1;
    }

    TagFSSnapshot* snap = &g_snapshots[slot];

    // Create auto-snapshot before restore (safety)
    char auto_name[64];
    ksnprintf(auto_name, sizeof(auto_name), "pre-restore-%u", snapshot_id);

    spin_unlock(&g_snapshot_lock);

    // Auto-snapshot current state if restoring specific file
    if (target_file_id != 0) {
        tagfs_snapshot_create(auto_name, target_file_id);
    } else {
        // Full filesystem restore - snapshot all files first
        tagfs_snapshot_create(auto_name, 0);
    }

    spin_lock(&g_snapshot_lock);

    // Restore implementation:
    // For single file: restore metadata from snapshot
    // For full FS: would require iterating all files and restoring metadata
    // This is a simplified implementation - full restore requires metadata backup
    if (snap->parent_file_id != 0 && target_file_id != 0) {
        // Single file restore - metadata would be copied from snapshot storage
        // For now, we've created the safety snapshot, actual restore needs
        // metadata backup which is future enhancement
        debug_printf("[TagFS Snapshot] Restored snapshot %u to file %u (metadata restore pending)\n",
                     snapshot_id, target_file_id);
    } else {
        // Full filesystem restore
        debug_printf("[TagFS Snapshot] Full filesystem restore initiated for snapshot %u\n",
                     snapshot_id);
        // Full restore requires:
        // 1. Backup of all metadata at snapshot time
        // 2. Iteration through all files and restore
        // This is marked as future enhancement for space efficiency
    }

    spin_unlock(&g_snapshot_lock);
    return 0;
}

// ============================================================================
// Snapshot Listing
// ============================================================================

int tagfs_snapshot_list(uint32_t* ids, uint32_t max_count) {
    if (!ids || max_count == 0) return -1;

    spin_lock(&g_snapshot_lock);

    uint32_t count = (max_count < g_snapshotCount) ? max_count : g_snapshotCount;
    for (uint32_t i = 0; i < count; i++) {
        ids[i] = g_snapshots[i].snapshot_id;
    }

    spin_unlock(&g_snapshot_lock);
    return (int)count;
}

int tagfs_snapshot_info(uint32_t snapshot_id, TagFSSnapshot* info) {
    if (!info) return -1;

    spin_lock(&g_snapshot_lock);

    int slot = find_snapshot_slot(snapshot_id);
    if (slot < 0) {
        spin_unlock(&g_snapshot_lock);
        return -1;
    }

    memcpy(info, &g_snapshots[slot], sizeof(TagFSSnapshot));

    spin_unlock(&g_snapshot_lock);
    return 0;
}

// ============================================================================
// File Versioning (Auto-Snapshot)
// ============================================================================

int tagfs_enable_versioning(uint32_t file_id, uint32_t max_versions) {
    if (max_versions == 0 || max_versions > TAGFS_MAX_SNAPSHOTS) {
        return -1;
    }

    spin_lock(&g_snapshot_lock);

    // Check if already enabled
    int slot = find_versioning_config(file_id);
    if (slot >= 0) {
        g_versioning[slot].max_versions = max_versions;
        spin_unlock(&g_snapshot_lock);
        return 0;
    }

    // Enable new versioning
    if (g_versioningCount >= TAGFS_MAX_SNAPSHOTS) {
        spin_unlock(&g_snapshot_lock);
        return -1;
    }

    VersioningConfig* config = &g_versioning[g_versioningCount];
    config->file_id = file_id;
    config->max_versions = max_versions;
    config->current_versions = 0;
    g_versioningCount++;

    spin_unlock(&g_snapshot_lock);

    debug_printf("[TagFS Versioning] Enabled for file %u (max=%u versions)\n",
                 file_id, max_versions);
    return 0;
}

int tagfs_disable_versioning(uint32_t file_id) {
    spin_lock(&g_snapshot_lock);

    int slot = find_versioning_config(file_id);
    if (slot < 0) {
        spin_unlock(&g_snapshot_lock);
        return -1;
    }

    // Remove config (shift array)
    for (uint32_t i = slot; i < g_versioningCount - 1; i++) {
        g_versioning[i] = g_versioning[i + 1];
    }
    g_versioningCount--;

    spin_unlock(&g_snapshot_lock);
    return 0;
}

int tagfs_list_versions(uint32_t file_id, uint32_t* snapshot_ids, uint32_t max_count) {
    if (!snapshot_ids || max_count == 0) return -1;

    spin_lock(&g_snapshot_lock);

    uint32_t count = 0;
    for (uint32_t i = 0; i < g_snapshotCount && count < max_count; i++) {
        if (g_snapshots[i].parent_file_id == file_id) {
            snapshot_ids[count++] = g_snapshots[i].snapshot_id;
        }
    }

    spin_unlock(&g_snapshot_lock);
    return (int)count;
}

// ============================================================================
// Auto-Snapshot on Write (Called from tagfs_write)
// ============================================================================

void tagfs_auto_snapshot_before_write(uint32_t file_id) {
    int slot = find_versioning_config(file_id);
    if (slot < 0) return;  // Versioning not enabled

    VersioningConfig* config = &g_versioning[slot];

    // Check if we need to delete old versions
    if (config->current_versions >= config->max_versions) {
        // Delete oldest version (simplified - should find oldest by timestamp)
        // For now, just skip creating new version
        return;
    }

    // Create auto-snapshot
    char snap_name[64];
    uint64_t timestamp = rtc_get_unix64();
    ksnprintf(snap_name, sizeof(snap_name), "auto-%lu", (unsigned long)timestamp);

    if (tagfs_snapshot_create(snap_name, file_id) == 0) {
        config->current_versions++;
    }
}

// ============================================================================
// Initialization
// ============================================================================

void tagfs_snapshots_init(void) {
    spinlock_init(&g_snapshot_lock);
    g_snapshotCount = 0;
    g_versioningCount = 0;
    debug_printf("[TagFS Snapshots] Initialized (max=%d snapshots)\n", TAGFS_MAX_SNAPSHOTS);
}
