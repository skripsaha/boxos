#ifndef TAGFS_CONSTANTS_H
#define TAGFS_CONSTANTS_H


#define TAGFS_VERSION               1
#define TAGFS_BLOCK_SIZE            4096
#define TAGFS_SECTOR_SIZE           512
#define TAGFS_BLOCK_SECTORS         (TAGFS_BLOCK_SIZE / TAGFS_SECTOR_SIZE)


#define DISK_BOOK_JOURNAL_SECTORS   1024

#define TAGFS_INVALID_TAG_ID        0xFFFF
#define TAGFS_MAX_TAG_ID            0x7FFE

#define TAGFS_TAG_FLAG_HAS_VALUE    0x01
#define TAGFS_TAG_FLAG_SYSTEM       0x02

#define TAGFS_FILE_ACTIVE           (1 << 0)
#define TAGFS_FILE_TRASHED          (1 << 1)
#define TAGFS_FILE_HIDDEN           (1 << 2)

#define TAGFS_HANDLE_READ           (1 << 0)
#define TAGFS_HANDLE_WRITE          (1 << 1)

#define TAGFS_REG_BUCKETS           512
#define TAGFS_KEY_BUCKETS           128
#define TAGFS_REGISTRY_DATA_SIZE    4080
#define TAGFS_MPOOL_DATA_SIZE       4080
#define TAGFS_FTABLE_PER_BLOCK      510

#define TAGFS_MAX_SNAPSHOTS         64
#define TAGFS_SNAPSHOT_NAME_LEN     32

#define TAGFS_BITMAP_INITIAL_TAG_CAP    64
#define TAGFS_BITMAP_INITIAL_FILE_CAP   256

#define TAGFS_READ_AHEAD_BLOCKS         4

#endif