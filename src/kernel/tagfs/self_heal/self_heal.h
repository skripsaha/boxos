#ifndef SELF_HEAL_H
#define SELF_HEAL_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../tagfs_constants.h"
#include "../../core/error/error.h"

// Self-Healing with Metadata Mirroring

#define HEAL_MAGIC              0x4845414C
#define HEAL_VERSION            1
#define HEAL_SCRUB_INTERVAL_MS  3600000
#define HEAL_SCRUB_BATCH_SIZE   64
#define HEAL_MIRROR_COUNT       2

#define HEAL_PATTERN_ZERO       0x00
#define HEAL_PATTERN_ONES       0xFF
#define HEAL_PATTERN_RANDOM     0xAA

typedef struct {
    uint32_t block_number;
    uint64_t detect_time;
    uint32_t corruption_type;
    uint8_t  severity;
    uint8_t  recovered;
    uint8_t  reserved[2];
    uint8_t  original_data[64];
    uint8_t  recovered_data[64];
} HealCorruptionRecord;

#define HEAL_MAX_RECORDS 256

typedef struct {
    uint32_t block_number;
    uint8_t  data[TAGFS_BLOCK_SIZE];
    uint32_t crc32;
    uint64_t last_verified;
    uint8_t  is_valid;
    uint8_t  mirror_index;
    uint8_t  reserved[2];
} HealMirrorEntry;

typedef struct {
    uint64_t blocks_scrubbed;
    uint64_t corruptions_detected;
    uint64_t corruptions_fixed;
    uint64_t corruptions_unrecoverable;
    uint32_t scrub_runs;
    uint64_t last_scrub_time;
    uint64_t next_scrub_time;
    uint32_t mirror_syncs;
    uint32_t crc_errors;
} HealStats;

typedef struct {
    uint32_t magic;
    uint32_t version;
    HealMirrorEntry mirrors[HEAL_MIRROR_COUNT];
    HealCorruptionRecord records[HEAL_MAX_RECORDS];
    uint32_t record_count;
    uint32_t record_head;
    uint32_t scrub_block_start;
    uint32_t scrub_block_current;
    uint64_t last_scrub_time;
    uint64_t scrub_interval_ms;
    bool scrub_active;
    HealStats stats;
    spinlock_t lock;
    bool enabled;
    bool initialized;
} HealState;

// Public API
error_t TagFS_SelfHealInit(void);
void TagFS_SelfHealShutdown(void);

void TagFS_SelfHealEnable(bool enable);
bool TagFS_SelfHealIsEnabled(void);

error_t TagFS_SelfHealOnMetadataWrite(uint32_t block_number, const uint8_t *data);
error_t TagFS_SelfHealOnMetadataRead(uint32_t block_number, uint8_t *data, bool *recovered);

error_t TagFS_SelfHealScrubRun(void);
error_t TagFS_SelfHealScrubBlock(uint32_t block_number);
error_t TagFS_SelfHealScheduleScrub(void);

error_t TagFS_SelfHealRecover(uint32_t block_number, uint8_t *recovered_data, bool *success);

error_t TagFS_SelfHealGetStats(HealStats *stats);
error_t TagFS_SelfHealPrintStats(void);

error_t TagFS_SelfHealGetCorruptionRecords(HealCorruptionRecord *records, uint32_t max_records, uint32_t *count);
error_t TagFS_SelfHealClearRecords(void);

bool TagFS_SelfHealIsInitialized(void);
uint32_t TagFS_SelfHealComputeCrc32(const uint8_t *data, uint32_t length);

#endif // SELF_HEAL_H
