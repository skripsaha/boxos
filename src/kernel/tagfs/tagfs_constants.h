#ifndef TAGFS_CONSTANTS_H
#define TAGFS_CONSTANTS_H

// ============================================================================
// TagFS Production Constants
// All TagFS modules include this file - no circular dependencies
// ============================================================================

#define TAGFS_VERSION               1
#define TAGFS_BLOCK_SIZE            4096
#define TAGFS_SECTOR_SIZE           512
#define TAGFS_BLOCKS_PER_SECTOR     (TAGFS_BLOCK_SIZE / TAGFS_SECTOR_SIZE)

// Disk Layout (sectors)
#define TAGFS_SUPERBLOCK_SECTOR     1034
#define TAGFS_BACKUP_SB_SECTOR      1035
#define TAGFS_DISK_BOOK_SB_SECTOR   1036
#define TAGFS_DISK_BOOK_START       1038

// Tag constants
#define TAGFS_INVALID_TAG_ID        0xFFFF
#define TAGFS_MAX_TAG_ID            0xFFFE

// File flags
#define TAGFS_FILE_ACTIVE           (1 << 0)
#define TAGFS_FILE_TRASHED          (1 << 1)
#define TAGFS_FILE_HIDDEN           (1 << 2)

// Handle flags
#define TAGFS_HANDLE_READ           (1 << 0)
#define TAGFS_HANDLE_WRITE          (1 << 1)

// Registry constants
#define TAGFS_REG_BUCKETS           512
#define TAGFS_KEY_BUCKETS           128
#define TAGFS_REGISTRY_DATA_SIZE    4080
#define TAGFS_MPOOL_DATA_SIZE       4080
#define TAGFS_FTABLE_PER_BLOCK      510

// Snapshot limits
#define TAGFS_MAX_SNAPSHOTS         64
#define TAGFS_SNAPSHOT_NAME_LEN     32

// Bitmap index initial capacities
#define TAGFS_BITMAP_INITIAL_TAG_CAP    64
#define TAGFS_BITMAP_INITIAL_FILE_CAP   256

// Read-ahead cache
#define TAGFS_READ_AHEAD_BLOCKS         4

// CRC configuration
// Byte offsets into TagFSSuperblock.reserved[] where the sentinel and CRC32
// are stored. reserved[] starts at struct byte 108 (after backup_superblock_sector).
// Absolute byte 507 = reserved[399] = sentinel.
// Absolute bytes 508-511 = reserved[400..403] = CRC32.
#define TAGFS_SB_CRC_OFFSET         400
#define TAGFS_SB_CRC_SENTINEL_OFFSET 399
#define TAGFS_SB_CRC_SENTINEL       0xCC

#endif // TAGFS_CONSTANTS_H
