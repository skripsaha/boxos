/*
 * create_tagfs_v1.h — the format-v1 writer's own words: the copies of the
 * kernel's v1 constants and on-disk structures it writes, the volume's shape,
 * and what its two files (create_tagfs_v1.c, create_tagfs_v1_pack.c) share.
 *
 * Format v1 lives until its path is retired, and then this header and those
 * two files go together.
 *
 * ‼ EVERYTHING IT WRITES IS COUNTED FROM THE START OF THE VOLUME'S GROUND
 *
 * — the run of sectors the medium's partition table claims for BoxOS, read
 * the way the kernel reads it (create_tagfs_ground.c):
 *
 *   block 0           the Deed — the volume's title to its ground
 *   block 1, 2         the Ledger, two copies, written alternately
 *   block 3, 4         the DiskBook's own head and its backup
 *   block 5 +          the DiskBook's records
 *   then               the block bitmap, covering exactly the data run
 *   then               the data: registry, file table, metadata pool, files
 *   last block         the Deed again, so the far end proves the volume is
 *                      all there
 *
 * Nothing shares a 4096-byte physical block with its own backup, which is what
 * the old layout did with the superblock, its copy, and the journal head — all
 * three inside sectors 1032..1039, one failed erase away from going together.
 */

#ifndef CREATE_TAGFS_V1_H
#define CREATE_TAGFS_V1_H

#include "create_tagfs.h"
#include "volume_ledger.h"   /* and what is true of the volume today           */

/* ====================================================================
 * Constants
 *
 * The format's own come from the headers the kernel reads it with: the
 * block and its sectors from volume_deed.h, the chains' magics from
 * boxos_magic.h. The rest below are copies of the kernel's
 * tagfs_constants.h and disk_book.h, which a host tool cannot include —
 * they take the kernel's integer types — and must match them.
 * ==================================================================== */

#include "boxos_magic.h"     /* TAGFS_REGISTRY_MAGIC, TAGFS_FILETBL_MAGIC, TAGFS_MPOOL_MAGIC */

#define TAGFS_FILE_ACTIVE      (1 << 0)

/* The DiskBook's on-disk geometry, from the kernel's disk_book.h. Its head and
 * the copy of its head get a block each — they were one sector apart, which on
 * every medium in use is the same physical block. */
#define DISK_BOOK_SB_MAGIC            0x44425342  /* "DBSB" */
#define DISK_BOOK_VERSION             2
#define DISK_BOOK_CAPACITY            512
#define DISK_BOOK_SECTORS_PER_ENTRY   2
#define DISK_BOOK_ENTRY_BLOCKS \
    ((DISK_BOOK_CAPACITY * DISK_BOOK_SECTORS_PER_ENTRY) / TAGFS_BLOCK_SECTORS)

/* A Deed occupies one block wherever it sits. */
#define DEED_BLOCKS   1

#define TAGFS_REGISTRY_DATA_SIZE   4080
#define TAGFS_MPOOL_DATA_SIZE      4080
#define TAGFS_FTABLE_PER_BLOCK     510

/* The highest tag id format v1 carries (plot_width.h): a record's tags and
 * the registry's records hold an id in a u16, and a kernel from before tag
 * ids were u32 read bit 15 as its Logbook's. The cargo interns none past it
 * (seed_reserved_tags). */
#define TAGFS_V1_TAG_ID_MOST  0x7FFEu

#define RECORD_HEADER_SIZE    42   /* metadata pool record header (40 fields + 2 CRC16) */
#define RECORD_CRC_OFFSET     40   /* CRC16 stored at bytes [40..41] of packed record */
#define MPOOL_BLOCK_HEADER    16   /* MetaPoolBlock header before payload */
#define INTEGRITY_DIGESTS_PER_BLOCK  (TAGFS_BLOCK_SIZE / 8)   /* integrity.c ENTRIES_PER_BLOCK */

/* ====================================================================
 * On-disk structures — must match kernel's tagfs.h exactly
 * ==================================================================== */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t next_block;
    uint16_t entry_count;
    uint16_t used_bytes;
    uint8_t  reserved[4];
    uint8_t  data[TAGFS_REGISTRY_DATA_SIZE];
} TagRegistryBlock;

typedef struct __attribute__((packed)) {
    uint32_t meta_block;
    uint32_t meta_offset;
} FileTableEntry;

typedef struct __attribute__((packed)) {
    uint32_t       magic;
    uint32_t       next_block;
    uint32_t       entry_count;
    uint32_t       reserved;
    FileTableEntry entries[TAGFS_FTABLE_PER_BLOCK];
} FileTableBlock;

typedef struct __attribute__((packed)) {
    uint32_t start_block;
    uint16_t block_count;
} FileExtent;

/* `kept` sits in two of the four bytes this header set aside and never used, so
 * the block is the same 4096 bytes it always was. It pairs with record_count —
 * one counts what was ever written here, the other what is still held. Nothing
 * is retired at build time, so on a fresh image the two are equal, and the
 * kernel counts the records rather than believing either of them. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t next_block;
    uint16_t used_bytes;
    uint16_t record_count;
    uint16_t kept;
    uint8_t  reserved[2];
    uint8_t  payload[TAGFS_MPOOL_DATA_SIZE];
} MetaPoolBlock;

/* ====================================================================
 * Layout computation — in the volume's own 4096-byte blocks, counted from
 * the start of its ground. Nothing here is a sector number on a medium.
 * ==================================================================== */

typedef struct {
    uint32_t volume_blocks;     /* the whole ground, in blocks           */
    uint32_t deed_block;        /* 0 — the title                         */
    uint32_t ledger_block;      /* two copies, ledger_block and +1       */
    uint32_t disk_book_block;   /* head, backup, then the records        */
    uint32_t disk_book_blocks;
    uint32_t bitmap_block;
    uint32_t bitmap_blocks;
    uint32_t data_block;
    uint32_t data_blocks;
    uint64_t tail_sector;       /* where the far copy of the Deed goes   */
} VolumeShape;

int      write_block(FILE* disk, uint32_t data_start_block, uint32_t block,
                     const void* data);
int      compute_layout(uint64_t ground_sectors, VolumeShape* out);
int      write_deed(FILE* disk, uint32_t role, uint64_t vsector,
                    const uint8_t uuid[16], const VolumeShape* shape,
                    const VolumeBoot* boot, uint32_t mpool_blocks,
                    uint32_t ftable_blocks, uint32_t registry_blocks,
                    const VolumePacked* packed);
uint32_t pool_block_mix(uint32_t block, const MetaPoolBlock* blk);
int      write_ledger(FILE* disk, const VolumeShape* shape, const VolumeLedger* src);
int      write_integrity_map(FILE* disk, uint32_t data_start_block, uint32_t first,
                             uint32_t count, uint32_t total_blocks);
int      write_registry_chain(FILE* disk, uint32_t data_start_block,
                              uint32_t* next_free, uint32_t total_blocks,
                              uint32_t* out_blocks, SettleMark* mark);
uint64_t V1RecordBytes(const FileInfo* fi);
uint64_t pack_metadata_record(uint8_t* buf, const FileInfo* fi);

#endif /* CREATE_TAGFS_V1_H */
