#include "../tagfs.h"
#include "disk_book/disk_book.h"
#include "metadata_pool/meta_pool.h"
#include "../../lib/kernel/klib.h"
#include "../../kernel/drivers/timer/rtc.h"

#define g_state (*tagfs_get_state())

// ============================================================================
// Snapshot Storage
// ============================================================================

static TagFSSnapshot g_snapshots[TAGFS_MAX_SNAPSHOTS];
static uint32_t g_snapshot_count = 0;
static spinlock_t g_snapshot_lock;

// ============================================================================
// Internal Helpers
// ============================================================================

static int find_snapshot_slot(uint32_t snapshot_id) {
    for (uint32_t i = 0; i < g_snapshot_count; i++) {
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

// ============================================================================
// Snapshot Creation
// ============================================================================

int tagfs_snapshot_create(const char* name, uint32_t file_id) {
    if (!name || g_snapshot_count >= TAGFS_MAX_SNAPSHOTS) {
        return ERR_INVALID_ARGUMENT;
    }

    spin_lock(&g_snapshot_lock);

    for (uint32_t i = 0; i < g_snapshot_count; i++) {
        if (strcmp(g_snapshots[i].name, name) == 0) {
            spin_unlock(&g_snapshot_lock);
            debug_printf("[TagFS Snapshot] Duplicate name: %s\n", name);
            return ERR_ALREADY_EXISTS;
        }
    }

    TagFSSnapshot* snap = &g_snapshots[g_snapshot_count];
    memset(snap, 0, sizeof(TagFSSnapshot));

    snap->snapshot_id = allocate_snapshot_id();
    snap->parent_file_id = file_id;
    snap->created_time = rtc_get_unix64();
    strncpy(snap->name, name, TAGFS_SNAPSHOT_NAME_LEN - 1);
    snap->flags = 0x01;

    if (file_id == 0) {
        snap->file_count = g_state.superblock.total_files;
        snap->total_size = 0;
    } else {
        TagFSMetadata meta;
        if (tagfs_get_metadata(file_id, &meta) == OK) {
            snap->file_count = 1;
            snap->total_size = meta.size;
            tagfs_metadata_free(&meta);
        } else {
            spin_unlock(&g_snapshot_lock);
            debug_printf("[TagFS Snapshot] File %u not found\n", file_id);
            return ERR_FILE_NOT_FOUND;
        }
    }

    DiskBookTxn txn;
    if (DiskBookBegin(&txn) == OK) {
        DiskBookLogMetadata(&txn, snap->snapshot_id, 0, NULL);
        DiskBookCommit(&txn);
    }

    g_snapshot_count++;
    spin_unlock(&g_snapshot_lock);

    debug_printf("[TagFS Snapshot] Created '%s' (id=%u, files=%u)\n",
                 name, snap->snapshot_id, snap->file_count);
    return OK;
}

// ============================================================================
// Snapshot Deletion
// ============================================================================

int tagfs_snapshot_delete(uint32_t snapshot_id) {
    spin_lock(&g_snapshot_lock);

    int slot = find_snapshot_slot(snapshot_id);
    if (slot < 0) {
        spin_unlock(&g_snapshot_lock);
        return ERR_SNAPSHOT_NOT_FOUND;
    }

    for (uint32_t i = slot; i < g_snapshot_count - 1; i++) {
        g_snapshots[i] = g_snapshots[i + 1];
    }
    g_snapshot_count--;

    spin_unlock(&g_snapshot_lock);
    debug_printf("[TagFS Snapshot] Deleted snapshot %u\n", snapshot_id);
    return OK;
}

// ============================================================================
// Snapshot Listing
// ============================================================================

int tagfs_snapshot_list(uint32_t* ids, uint32_t max_count) {
    if (!ids || max_count == 0) {
        return ERR_INVALID_ARGUMENT;
    }

    spin_lock(&g_snapshot_lock);

    uint32_t count = (max_count < g_snapshot_count) ? max_count : g_snapshot_count;
    for (uint32_t i = 0; i < count; i++) {
        ids[i] = g_snapshots[i].snapshot_id;
    }

    spin_unlock(&g_snapshot_lock);
    return (int)count;
}

int tagfs_snapshot_info(uint32_t snapshot_id, TagFSSnapshot* info) {
    if (!info) {
        return ERR_NULL_POINTER;
    }

    spin_lock(&g_snapshot_lock);

    int slot = find_snapshot_slot(snapshot_id);
    if (slot < 0) {
        spin_unlock(&g_snapshot_lock);
        return ERR_SNAPSHOT_NOT_FOUND;
    }

    memcpy(info, &g_snapshots[slot], sizeof(TagFSSnapshot));
    spin_unlock(&g_snapshot_lock);
    return OK;
}

// ============================================================================
// Initialization
// ============================================================================

void tagfs_snapshots_init(void) {
    spinlock_init(&g_snapshot_lock);
    g_snapshot_count = 0;
    debug_printf("[TagFS Snapshots] Initialized (max=%d)\n", TAGFS_MAX_SNAPSHOTS);
}
