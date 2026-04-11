#ifndef DISK_BOOK_H
#define DISK_BOOK_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../tagfs_constants.h"
#include "../../core/error/error.h"

// DiskBook — Tag-Aware Write-Ahead Log for TagFS

#define DISK_BOOK_MAGIC             0x44424F4F
#define DISK_BOOK_SB_MAGIC          0x44425342
#define DISK_BOOK_VERSION           1
#define DISK_BOOK_ENTRY_SIZE        1024
#define DISK_BOOK_DATA_SIZE         970
#define DISK_BOOK_SECTORS_PER_ENTRY 2
#define DISK_BOOK_CAPACITY          2048
#define DISK_BOOK_CHECKPOINT_THRESH 256

// Entry types
#define DISK_BOOK_TYPE_DATA         0x01
#define DISK_BOOK_TYPE_METADATA     0x02
#define DISK_BOOK_TYPE_COMMIT       0x03
#define DISK_BOOK_TYPE_CHECKPOINT   0x04

// Entry states
#define DISK_BOOK_STATE_PENDING     0x00
#define DISK_BOOK_STATE_COMMITTED   0x01
#define DISK_BOOK_STATE_APPLIED     0x02

// Flags
#define DISK_BOOK_FLAG_COMPRESSED   0x01
#define DISK_BOOK_FLAG_ZERO_FILL    0x02

typedef struct __packed {
    uint32_t magic;
    uint32_t version;
    uint64_t start_sector;
    uint64_t end_sector;
    uint32_t head;
    uint32_t tail;
    uint32_t checkpoint_seq;
    uint32_t checkpoint_tail;
    uint32_t next_sequence;
    uint32_t flags;
    uint8_t  uuid[16];
    uint8_t  reserved[448];
} DiskBookSuperblock;

STATIC_ASSERT(sizeof(DiskBookSuperblock) == 512, "DiskBookSuperblock_must_be_512_bytes");

typedef struct __packed {
    uint32_t magic;
    uint32_t sequence;
    uint16_t type;
    uint16_t flags;
    uint32_t file_id;
    uint64_t block_offset;
    uint32_t disk_sector;
    uint16_t data_size;
    uint8_t  state;
    uint8_t  tag_count;
    uint8_t  reserved[2];
    uint8_t  tags[16];
    uint32_t data_crc32;
    uint8_t  data[DISK_BOOK_DATA_SIZE];
} DiskBookEntry;

STATIC_ASSERT(sizeof(DiskBookEntry) == 1024, "DiskBookEntry_must_be_1024_bytes");

typedef struct {
    uint32_t sequence;
    uint32_t first_entry;
    uint32_t entry_count;
    uint8_t  active;
    uint8_t  reserved[3];
} DiskBookTxn;

typedef struct {
    uint32_t total_entries;
    uint32_t used_entries;
    uint32_t free_entries;
    uint32_t transactions;
    uint32_t checkpoints;
    uint32_t writes_total;
    uint32_t writes_failed;
    uint32_t replay_count;
    uint32_t crc_errors;
    uint64_t bytes_logged;
    uint32_t uptime_seconds;
} DiskBookStats;

// Public API
error_t DiskBookInit(uint32_t superblock_sector);
error_t DiskBookReload(void);
error_t DiskBookValidateAndReplay(void);
void DiskBookShutdown(void);

error_t DiskBookBegin(DiskBookTxn* txn);
error_t DiskBookLogData(DiskBookTxn* txn, uint32_t file_id, uint64_t block_offset,
                        uint32_t disk_sector, const void* data, uint16_t data_size);
error_t DiskBookLogMetadata(DiskBookTxn* txn, uint32_t file_id, uint32_t meta_sector,
                            const void* meta);
error_t DiskBookCommit(DiskBookTxn* txn);
void DiskBookAbort(DiskBookTxn* txn);

error_t DiskBookCheckpoint(void);

error_t DiskBookGetStats(DiskBookStats* stats);
error_t DiskBookPrintStats(void);

bool DiskBookIsInitialized(void);
uint32_t DiskBookGetCheckpointSeq(void);

#endif // DISK_BOOK_H
