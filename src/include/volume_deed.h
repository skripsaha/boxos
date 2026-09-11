#ifndef VOLUME_DEED_H
#define VOLUME_DEED_H



#define VOLUME_DEED_MAGIC_0  'B'
#define VOLUME_DEED_MAGIC_1  'O'
#define VOLUME_DEED_MAGIC_2  'X'
#define VOLUME_DEED_MAGIC_3  'D'
#define VOLUME_DEED_MAGIC_4  'E'
#define VOLUME_DEED_MAGIC_5  'E'
#define VOLUME_DEED_MAGIC_6  'D'
#define VOLUME_DEED_MAGIC_7  '\0'

#define VOLUME_DEED_ROLE_HEAD  1u
#define VOLUME_DEED_ROLE_TAIL  2u

#define VOLUME_DEED_SECTOR_BYTES  512u

typedef struct __attribute__((packed)) {
    uint8_t  magic[8];
    uint16_t prologue_bytes;
    uint16_t stamp_bytes;
    uint32_t crc32;
    uint8_t  uuid[16];
    uint64_t sectors;
    uint64_t tail_sector;
    uint32_t role;
} VolumeDeed;

typedef struct __attribute__((packed)) {
    uint16_t kind;
    uint16_t bytes;
} VolumeStamp;

#define VOLUME_STAMP_GEOMETRY  1
#define VOLUME_STAMP_LAYOUT    2
#define VOLUME_STAMP_BORN      3
#define VOLUME_STAMP_BOOT      4

typedef struct __attribute__((packed)) {
    uint32_t logical_bytes;
    uint32_t physical_bytes;
    uint32_t grain_bytes;
    uint32_t block_bytes;
} VolumeGeometry;

typedef struct __attribute__((packed)) {
    uint64_t total_blocks;
    uint32_t state_block;
    uint32_t state_blocks;
    uint32_t tag_registry_block;
    uint32_t tag_registry_blocks;
    uint32_t file_table_block;
    uint32_t file_table_blocks;
    uint32_t metadata_pool_block;
    uint32_t metadata_pool_blocks;
    uint32_t block_bitmap_block;
    uint32_t block_bitmap_blocks;
    uint32_t disk_book_block;
    uint32_t disk_book_blocks;
    uint32_t data_block;
    uint32_t data_blocks;
} VolumeLayout;

typedef struct __attribute__((packed)) {
    uint32_t kernel_block;
    uint32_t kernel_blocks;
    uint32_t kernel_bytes;
    uint32_t reserved;
} VolumeBoot;

typedef struct __attribute__((packed)) {
    uint64_t created_unix;
    char     maker[16];
} VolumeBorn;

#endif