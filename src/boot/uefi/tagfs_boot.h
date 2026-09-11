#ifndef TAGFS_BOOT_H
#define TAGFS_BOOT_H


#include "uefi.h"

#include "volume_deed.h"


#define TAGFS_MAGIC              0x54414746U
#define TAGFS_REGISTRY_MAGIC     0x54524547U
#define TAGFS_FILETBL_MAGIC      0x54465442U
#define TAGFS_MPOOL_MAGIC        0x544D504CU

#define TAGFS_VERSION            1
#define TAGFS_BLOCK_SIZE         4096
#define TAGFS_SECTOR_SIZE        512
#define TAGFS_SECTORS_PER_BLOCK  (TAGFS_BLOCK_SIZE / TAGFS_SECTOR_SIZE)


#define TAGFS_FILE_ACTIVE        (1U << 0)
#define TAGFS_FILE_TRASHED       (1U << 1)

#define TAGFS_REGISTRY_DATA_SIZE  4080
#define TAGFS_MPOOL_DATA_SIZE     4080
#define TAGFS_FTABLE_PER_BLOCK    510
#define TAGFS_INVALID_TAG_ID      0xFFFFU

#define TAGFS_TAG_FLAG_HAS_VALUE  0x01U
#define TAGFS_TAG_FLAG_SYSTEM     0x02U

#define META_RECORD_LEN_OFF      0
#define META_RECORD_FILE_ID_OFF  2
#define META_RECORD_FLAGS_OFF    6
#define META_RECORD_SIZE_OFF     10
#define META_RECORD_TAGCOUNT_OFF 34
#define META_RECORD_EXTCOUNT_OFF 36
#define META_RECORD_NAMELEN_OFF  38
#define META_RECORD_VARDATA_OFF  42


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

#define TAG_RECORD_ID_OFF       0
#define TAG_RECORD_FLAGS_OFF    2
#define TAG_RECORD_KEYLEN_OFF   3
#define TAG_RECORD_VALLEN_OFF   4
#define TAG_RECORD_KEY_OFF      6

_Static_assert(sizeof(TagBootRegistryBlock)   == 4096, "TagBootRegistryBlock must be 4096 bytes");
_Static_assert(sizeof(TagBootFileTableBlock)  == 4096, "TagBootFileTableBlock must be 4096 bytes");
_Static_assert(sizeof(TagBootMetaPoolBlock)   == 4096, "TagBootMetaPoolBlock must be 4096 bytes");
_Static_assert(sizeof(TagBootFileExtent)      == 6,    "TagBootFileExtent must be 6 bytes");

#endif