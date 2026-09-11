#ifndef DISK_BOOK_H
#define DISK_BOOK_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../tagfs_constants.h"
#include "../../core/error/error.h"


#define DISK_BOOK_MAGIC             0x44424F4F
#define DISK_BOOK_SB_MAGIC          0x44425342
#define DISK_BOOK_VERSION           2
#define DISK_BOOK_ENTRY_SIZE        1024
#define DISK_BOOK_SECTORS_PER_ENTRY 2
#define DISK_BOOK_CAPACITY          512
#define DISK_BOOK_FLUSH_THRESH      64

#define DISK_BOOK_TYPE_REDIRECT     0x01

typedef struct __packed {
    uint32_t magic;
    uint32_t version;
    uint64_t start_sector;
    uint32_t capacity;
    uint32_t count;
    uint32_t generation;
    uint32_t flags;
    uint32_t crc32;
    uint8_t  uuid[16];
    uint8_t  reserved[460];
} DiskBookSuperblock;

STATIC_ASSERT(sizeof(DiskBookSuperblock) == 512, "DiskBookSuperblock_must_be_512_bytes");

typedef struct __packed {
    uint32_t magic;
    uint32_t sequence;
    uint16_t type;
    uint16_t flags;
    uint32_t snapshot_id;
    uint32_t old_block;
    uint32_t new_block;
    uint64_t tag_bits;
    uint32_t record_crc32;
    uint8_t  reserved[988];
} DiskBookEntry;

STATIC_ASSERT(sizeof(DiskBookEntry) == 1024, "DiskBookEntry_must_be_1024_bytes");

typedef struct {
    uint32_t total_entries;
    uint32_t used_entries;
    uint32_t free_entries;
    uint32_t redirects_logged;
    uint32_t replay_count;
    uint32_t crc_errors;
    uint32_t generation;
    uint32_t uptime_seconds;
} DiskBookStats;

error_t  DiskBookInit(uint64_t head_sector, uint64_t backup_sector,
                     uint64_t records_sector);
error_t  DiskBookValidateAndReplay(void);
void     DiskBookShutdown(void);

error_t  DiskBookLogRedirect(uint32_t snapshot_id, uint32_t old_block,
                             uint32_t new_block, uint64_t tag_bits);

void     DiskBookResetRedirects(void);

error_t  DiskBookCheckpoint(void);

error_t  DiskBookGetStats(DiskBookStats* stats);
error_t  DiskBookPrintStats(void);

bool     DiskBookIsInitialized(void);
uint32_t DiskBookGetCheckpointSeq(void);

#endif