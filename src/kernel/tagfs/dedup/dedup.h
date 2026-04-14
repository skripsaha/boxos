#ifndef DEDUP_H
#define DEDUP_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../tagfs_constants.h"
#include "../../core/error/error.h"
#include "../box_hash/box_hash.h"

// Tag-Aware Deduplication with BoxHash Checksums

#define DEDUP_MAGIC              0x44454450
#define DEDUP_VERSION            1
#define DEDUP_HASH_BUCKETS       8192
#define DEDUP_MAX_REFS           0xFFFF
#define DEDUP_GC_THRESHOLD_SECS  3600   // Evict unreferenced entries older than 1 hour

// Use BoxHash for block identification
typedef BoxHash DedupHash;
#define DEDUP_HASH_BYTES      BOX_HASH_BYTES

typedef struct DedupEntry {
    DedupHash hash;
    uint32_t physical_block;
    uint32_t ref_count;
    uint64_t create_time;
    uint64_t last_access;
    uint32_t tag_context;
    uint8_t is_compressed;
    uint8_t compression_ratio;
    uint16_t reserved;
    struct DedupEntry *next;
} DedupEntry;

typedef struct {
    uint64_t total_blocks;
    uint64_t unique_blocks;
    uint64_t duplicate_blocks;
    uint64_t bytes_saved;
    uint64_t dedup_ratio;
    uint32_t hash_collisions;
    uint32_t gc_runs;
    uint32_t entries_freed;
} DedupStats;

typedef struct {
    uint32_t magic;
    uint32_t version;
    BoxHashContext hash_ctx;      // BoxHash context with salt+key
    DedupEntry **hash_table;
    uint32_t hash_buckets;
    uint32_t entry_count;
    uint64_t last_gc_time;
    uint32_t gc_interval_entries;
    DedupStats stats;
    spinlock_t lock;
    bool initialized;
} DedupState;

// Public API
error_t TagFS_DedupInit(void);
void TagFS_DedupShutdown(void);

error_t TagFS_DedupCheck(const uint8_t *block_data, uint32_t *existing_block, bool *is_duplicate);
error_t TagFS_DedupRegister(uint32_t physical_block, const uint8_t *block_data, uint32_t tag_context);
error_t TagFS_DedupUnregister(uint32_t physical_block);
error_t TagFS_DedupAllocBlock(const uint8_t *block_data, uint32_t *allocated_block, int *is_duplicate, uint32_t tag_context);

error_t TagFS_DedupFindByTag(uint32_t tag_context, uint32_t *blocks, uint32_t max_blocks, uint32_t *count);
error_t TagFS_DedupGetTagStats(uint32_t tag_context, uint64_t *blocks, uint64_t *bytes);

error_t TagFS_DedupCompress(const uint8_t *in_data, uint16_t in_size, uint8_t *out_data, uint16_t *out_size);
error_t TagFS_DedupDecompress(const uint8_t *in_data, uint16_t in_size, uint8_t *out_data, uint16_t *out_size);

error_t TagFS_DedupGC(void);

error_t TagFS_DedupGetStats(DedupStats *stats);
error_t TagFS_DedupPrintStats(void);

bool TagFS_DedupIsInitialized(void);
DedupHash TagFS_DedupComputeHash(const uint8_t *data, uint32_t size);

#endif // DEDUP_H
