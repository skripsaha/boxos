#ifndef TAGFS_BOOT_H
#define TAGFS_BOOT_H

/*
 * TagFS on-disk structures needed at boot time.
 * Must match kernel's tagfs.h and create_tagfs.c exactly.
 * No kernel headers included — standalone for UEFI environment.
 */

#include "uefi.h"

/* =========================================================================
 * Constants (must match tagfs.h / create_tagfs.c)
 * ========================================================================= */

#define TAGFS_MAGIC              0x54414746U   /* "TAGF" */
#define TAGFS_REGISTRY_MAGIC     0x54524547U   /* "TREG" */
#define TAGFS_FILETBL_MAGIC      0x54465442U   /* "TFTB" */
#define TAGFS_MPOOL_MAGIC        0x544D504CU   /* "TMPL" */

#define TAGFS_VERSION            1
#define TAGFS_BLOCK_SIZE         4096
#define TAGFS_SECTOR_SIZE        512
#define TAGFS_SECTORS_PER_BLOCK  (TAGFS_BLOCK_SIZE / TAGFS_SECTOR_SIZE)  /* 8 */

#define TAGFS_SUPERBLOCK_SECTOR  1034U
#define TAGFS_BACKUP_SB_SECTOR   1035U

#define TAGFS_FILE_ACTIVE        (1U << 0)
#define TAGFS_FILE_TRASHED       (1U << 1)

#define TAGFS_REGISTRY_DATA_SIZE  4080
#define TAGFS_MPOOL_DATA_SIZE     4080
#define TAGFS_FTABLE_PER_BLOCK    510
#define TAGFS_INVALID_TAG_ID      0xFFFFU

/* Boot hint byte offsets within superblock reserved[] (absolute byte offsets 108+) */
#define BOOT_HINT_KERNEL_BLOCK    0    /* reserved[0..3]  — start block of kernel file */
#define BOOT_HINT_KERNEL_BLOCKS   4    /* reserved[4..7]  — number of blocks */
#define BOOT_HINT_KERNEL_SIZE     8    /* reserved[8..11] — file size in bytes */
#define BOOT_HINT_DATA_START     12    /* reserved[12..15]— data_start_sector */

/* Metadata record header layout (must match pack_metadata_record in create_tagfs.c):
 *   uint16_t record_len     +0
 *   uint32_t file_id        +2
 *   uint32_t flags          +6
 *   uint64_t size           +10
 *   uint64_t created_time   +18
 *   uint64_t modified_time  +26
 *   uint16_t tag_count      +34
 *   uint16_t extent_count   +36
 *   uint16_t name_len       +38
 *   uint16_t crc16          +40
 *   uint16_t tag_ids[tag_count]  +42
 *   FileExtent extents[]    after tag_ids
 *   char     filename[]     after extents
 */
#define META_RECORD_LEN_OFF      0
#define META_RECORD_FILE_ID_OFF  2
#define META_RECORD_FLAGS_OFF    6
#define META_RECORD_SIZE_OFF     10
#define META_RECORD_TAGCOUNT_OFF 34
#define META_RECORD_EXTCOUNT_OFF 36
#define META_RECORD_NAMELEN_OFF  38
#define META_RECORD_VARDATA_OFF  42   /* start of tag_ids[] */

/* =========================================================================
 * On-disk structures (packed, must be 512 / 4096 bytes exactly)
 * ========================================================================= */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_files;
    uint32_t next_file_id;
    uint32_t next_tag_id;
    uint32_t total_tags;

    uint32_t tag_registry_block;
    uint32_t tag_registry_block_count;
    uint32_t file_table_block;
    uint32_t file_table_block_count;
    uint32_t metadata_pool_block;
    uint32_t metadata_pool_block_count;
    uint32_t block_bitmap_sector;
    uint32_t block_bitmap_sector_count;
    uint32_t disk_book_superblock_sector;

    uint64_t fs_created_time;
    uint64_t fs_modified_time;
    uint8_t  fs_uuid[16];
    uint32_t backup_superblock_sector;

    /* reserved[0..15]  — boot hints (bytes 108-123)
     * reserved[16..19] — CoW snapshot manifest block
     * reserved[20..23] — CoW snapshot backup block
     * reserved[399]    — CRC sentinel
     * reserved[400..403] — CRC32 */
    uint8_t  reserved[404];
} TagBootSuperblock;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t next_block;
    uint16_t entry_count;
    uint16_t used_bytes;
    uint8_t  _reserved[4];
    uint8_t  data[TAGFS_REGISTRY_DATA_SIZE];
} TagBootRegistryBlock;

typedef struct __attribute__((packed)) {
    uint32_t meta_block;
    uint32_t meta_offset;
} TagBootFileTableEntry;

typedef struct __attribute__((packed)) {
    uint32_t              magic;
    uint32_t              next_block;
    uint32_t              entry_count;
    uint32_t              _reserved;
    TagBootFileTableEntry entries[TAGFS_FTABLE_PER_BLOCK];
} TagBootFileTableBlock;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t next_block;
    uint16_t used_bytes;
    uint16_t record_count;
    uint8_t  _reserved[4];
    uint8_t  payload[TAGFS_MPOOL_DATA_SIZE];
} TagBootMetaPoolBlock;

typedef struct __attribute__((packed)) {
    uint32_t start_block;
    uint16_t block_count;
} TagBootFileExtent;

/* =========================================================================
 * Tag registry on-disk record format
 * Record in TagBootRegistryBlock.data[]:
 *   uint16_t tag_id
 *   uint8_t  flags     (0=label, 1=key:value)
 *   uint8_t  key_len
 *   uint16_t value_len
 *   char     key[key_len]
 *   char     value[value_len]
 * ========================================================================= */
#define TAG_RECORD_ID_OFF       0
#define TAG_RECORD_FLAGS_OFF    2
#define TAG_RECORD_KEYLEN_OFF   3
#define TAG_RECORD_VALLEN_OFF   4
#define TAG_RECORD_KEY_OFF      6

/* Compile-time size checks */
_Static_assert(sizeof(TagBootSuperblock)      == 512,  "TagBootSuperblock must be 512 bytes");
_Static_assert(sizeof(TagBootRegistryBlock)   == 4096, "TagBootRegistryBlock must be 4096 bytes");
_Static_assert(sizeof(TagBootFileTableBlock)  == 4096, "TagBootFileTableBlock must be 4096 bytes");
_Static_assert(sizeof(TagBootMetaPoolBlock)   == 4096, "TagBootMetaPoolBlock must be 4096 bytes");
_Static_assert(sizeof(TagBootFileExtent)      == 6,    "TagBootFileExtent must be 6 bytes");

#endif /* TAGFS_BOOT_H */
