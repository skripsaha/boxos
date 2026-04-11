#ifndef COW_H
#define COW_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../tagfs_constants.h"
#include "../../core/error/error.h"

// CoW Snapshots with Tag-Based Queries

#define COW_MAGIC           0x434F5753
#define COW_VERSION         1

// Snapshot flags
#define COW_SNAP_READONLY   (1 << 0)
#define COW_SNAP_AUTO       (1 << 1)
#define COW_SNAP_TAG_QUERY  (1 << 2)

typedef struct __packed {
    uint32_t snapshot_id;
    uint32_t parent_file_id;
    uint64_t created_time;
    uint64_t disk_book_checkpoint;
    uint32_t file_count;
    uint64_t total_size;
    uint32_t block_count;
    char     name[32];
    uint8_t  flags;
    uint8_t  tag_count;
    uint16_t tag_ids[8];
    uint8_t  reserved[34];
} CowSnapshot;

// STATIC_ASSERT(sizeof(CowSnapshot) == 128, "CowSnapshot_must_be_128_bytes");

typedef struct {
    uint32_t magic;
    uint32_t version;
    CowSnapshot *snapshots;
    uint32_t snapshot_capacity;
    uint32_t snapshot_count;
    uint64_t cow_writes;
    uint64_t cow_copies;
    uint64_t blocks_shared;
    uint32_t snapshots_created;
    spinlock_t lock;
    bool initialized;
} CowState;

// Public API
error_t TagFS_CowInit(void);
void TagFS_CowShutdown(void);

error_t TagFS_SnapshotCreate(const char *name, uint32_t file_id, uint32_t *snapshot_id);
error_t TagFS_SnapshotCreateByTag(const char *tag_pattern, uint32_t *snapshot_id);
error_t TagFS_SnapshotDelete(uint32_t snapshot_id);
error_t TagFS_SnapshotList(uint32_t *ids, uint32_t max_count, uint32_t *actual_count);
error_t TagFS_SnapshotInfo(uint32_t snapshot_id, CowSnapshot *info);

error_t TagFS_CowBeforeWrite(uint32_t file_id, uint32_t block_addr, uint32_t *new_block);
error_t TagFS_CowAfterWrite(uint32_t file_id, uint32_t old_block, uint32_t new_block);

void TagFS_CowGetStats(uint64_t *cow_writes, uint64_t *cow_copies,
                       uint64_t *blocks_shared, uint32_t *snapshots);

bool TagFS_CowIsActive(uint32_t file_id);
uint64_t TagFS_CowGetCheckpoint(void);

#endif // COW_H
