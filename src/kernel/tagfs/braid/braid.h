#ifndef BRAID_H
#define BRAID_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../../core/error/error.h"
#include "../box_hash/box_hash.h"

// Braid — Tag-Aware Storage Redundancy System for BoxOS
// Unique Features:
//   • Tag-aware disk assignment (files with same tag → same disk)
//   • Per-block BoxHash checksums (auto-detect corruption)
//   • Auto-healing from mirror (no manual intervention)
//   • Three modes: Mirror, Stripe, Weave
//
// Modes:
//   • BraidModeMirror: 2-way mirror (redundancy)
//   • BraidModeStripe: Striping (speed, no redundancy)
//   • BraidModeWeave: 3-way mirror (maximum safety)

#define BRAID_MAGIC         0x42524149  // 'BRAI'
#define BRAID_VERSION       1
#define BRAID_MAX_DISKS     8
#define BRAID_BLOCK_SIZE    4096

// Braid modes
typedef enum {
    BraidModeMirror = 0,  // 2-way mirror
    BraidModeStripe = 1,  // Striping (speed)
    BraidModeWeave = 2    // 3-way mirror (safety)
} BraidMode;

// Per-block metadata
typedef struct __packed {
    BoxHash checksum;           // BoxHash of block data (32 bytes)
    uint32_t disk_map;          // Bitmask of disks containing this block
    uint8_t  mode;              // BraidMode for this block
    uint8_t  tag_context[16];   // Tag context for tag-aware placement
    uint16_t flags;             // Block flags
    uint8_t  reserved[1];       // Padding to 64 bytes
    uint64_t last_verified;     // Last verification timestamp
} BraidBlockMeta;

STATIC_ASSERT(sizeof(BraidBlockMeta) == 64, "BraidBlockMeta_must_be_64_bytes");

// Block flags
#define BRAID_BLOCK_VALID     (1 << 0)
#define BRAID_BLOCK_CORRUPTED (1 << 1)
#define BRAID_BLOCK_HEALED    (1 << 2)
#define BRAID_BLOCK_TAGGED    (1 << 3)

// Disk state
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

// Braid array state
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

// Statistics
typedef struct {
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t checksum_errors;
    uint64_t auto_heals;
    uint64_t disk_failures;
    uint64_t tag_assignments;
} BraidStats;

// Public API

// Initialization
error_t BraidInit(BraidMode mode);
void BraidShutdown(void);

// Disk management
error_t BraidAddDisk(uint8_t disk_id, uint64_t total_blocks);
error_t BraidRemoveDisk(uint8_t disk_id);
error_t BraidSetDiskOnline(uint8_t disk_id, bool online);

// Block operations
error_t BraidReadBlock(uint64_t block_num, void *data, BoxHash *expected_checksum);
error_t BraidWriteBlock(uint64_t block_num, const void *data, const uint8_t *tag_context);
error_t BraidVerifyBlock(uint64_t block_num, bool *is_valid);

// Tag-aware operations
error_t BraidReadBlockTagged(uint64_t block_num, void *data, const uint8_t *tag_context);
error_t BraidWriteBlockTagged(uint64_t block_num, const void *data, const uint8_t *tag_context);

// Auto-healing
error_t BraidAutoHeal(uint64_t block_num);

// Statistics
error_t BraidGetStats(BraidStats *stats);
error_t BraidPrintStats(void);

// Health check
bool BraidIsHealthy(void);
uint8_t BraidGetActiveDiskCount(void);

#endif // BRAID_H
