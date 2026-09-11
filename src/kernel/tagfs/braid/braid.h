#ifndef BRAID_H
#define BRAID_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../../core/error/error.h"
#include "../box_hash/box_hash.h"


#define BRAID_MAGIC         0x42524149
#define BRAID_VERSION       1
#define BRAID_MAX_DISKS     8
#define BRAID_BLOCK_SIZE    4096

typedef enum {
    BraidModeMirror = 0,
    BraidModeStripe = 1,
    BraidModeWeave = 2
} BraidMode;

typedef struct __packed {
    BoxHash checksum;
    uint32_t disk_map;
    uint8_t  mode;
    uint8_t  tag_context[16];
    uint16_t flags;
    uint8_t  reserved[1];
    uint64_t last_verified;
} BraidBlockMeta;

STATIC_ASSERT(sizeof(BraidBlockMeta) == 64, "BraidBlockMeta_must_be_64_bytes");

#define BRAID_BLOCK_VALID     (1 << 0)
#define BRAID_BLOCK_CORRUPTED (1 << 1)
#define BRAID_BLOCK_HEALED    (1 << 2)
#define BRAID_BLOCK_TAGGED    (1 << 3)

typedef struct {
    uint8_t  disk_id;
    bool     online;
    uint64_t total_blocks;
    uint64_t used_blocks;
    uint64_t read_count;
    uint64_t write_count;
    uint64_t error_count;
    uint64_t last_seen;
} BraidDisk;

typedef struct {
    uint32_t magic;
    uint32_t version;
    BraidMode mode;
    uint8_t  disk_count;
    uint8_t  active_disks;
    uint16_t reserved;
    BraidDisk disks[BRAID_MAX_DISKS];
    BoxHashContext hash_ctx;
    spinlock_t lock;
    bool initialized;
} BraidState;

typedef struct {
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t checksum_errors;
    uint64_t auto_heals;
    uint64_t disk_failures;
    uint64_t tag_assignments;
} BraidStats;


error_t BraidInit(BraidMode mode);
void BraidShutdown(void);

error_t BraidAddDisk(uint8_t disk_id, uint64_t total_blocks);
error_t BraidRemoveDisk(uint8_t disk_id);
error_t BraidSetDiskOnline(uint8_t disk_id, bool online);

error_t BraidReadBlock(uint64_t block_num, void *data, BoxHash *expected_checksum);
error_t BraidWriteBlock(uint64_t block_num, const void *data, const uint8_t *tag_context);
error_t BraidVerifyBlock(uint64_t block_num, bool *is_valid);

error_t BraidReadBlockTagged(uint64_t block_num, void *data, const uint8_t *tag_context);
error_t BraidWriteBlockTagged(uint64_t block_num, const void *data, const uint8_t *tag_context);

error_t BraidAutoHeal(uint64_t block_num);

error_t BraidGetStats(BraidStats *stats);
error_t BraidPrintStats(void);

bool BraidIsHealthy(void);
uint8_t BraidGetActiveDiskCount(void);

#endif