#ifndef BRAID_H
#define BRAID_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../lock_rank/lock_rank.h"
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
//
// ‼ EVERY BRAID IS AN INSTANCE ITS OWNER HOLDS. There is no Braid of the
// machine: the volume's is the one its mount assembled (TagFSState.braid),
// and a test builds its own. The one there used to be was shared
// by the startup tests and the volume's every read and write: the tests left it
// initialised on every boot, so every block took its lock to be told "not
// healthy", and between a test's second disk and its removal a healthy mirror
// stood on seats 0 and 1 for the live volume to be sent to (MP-D14).

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

/* One disk of a braid: a Boardroom seat, its index in the braid. `member` is
 * whether it was added and not removed; `online` whether a member answers now.
 * Only a member that is online is read or written, and only a change of either
 * moves the braid's counts. */
typedef struct {
    uint8_t  disk_id;
    bool     member;
    bool     online;
    uint64_t total_blocks;
    uint64_t used_blocks;
    uint64_t read_count;
    uint64_t write_count;
    uint64_t error_count;
    uint64_t last_seen;
} BraidDisk;

// Statistics
typedef struct {
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t checksum_errors;
    uint64_t auto_heals;
    uint64_t disk_failures;
    uint64_t tag_assignments;
} BraidStats;

/* A braid: its disks, what it has done, and the lock that keeps both. The
 * lock is a leaf and is never held across a medium command: every read and
 * write takes a snapshot of which disks answer, lets go, moves the block, and
 * comes back only to count. `disk_count` is one past the highest index ever a
 * member. */
typedef struct BraidState {
    uint32_t       magic;
    uint32_t       version;
    BraidMode      mode;
    uint8_t        disk_count;
    uint8_t        active_disks;
    uint16_t       reserved;
    BraidDisk      disks[BRAID_MAX_DISKS];
    BraidStats     stats;
    BoxHashContext hash_ctx;
    TagFSLock      lock;
    bool           initialized;
} BraidState;

// Initialization. The checksums are seeded from `seed` (16 bytes — the
// volume's uuid for the volume's braid), so they verify again across mounts.
error_t BraidInit(BraidState *b, BraidMode mode, const uint8_t *seed);
void    BraidShutdown(BraidState *b);

// Disk management. Adding a member twice is refused ERR_ALREADY_INITIALIZED,
// removing or setting a disk that is not one ERR_DEVICE_NOT_READY; setting a
// member to what it already is changes nothing.
error_t BraidAddDisk(BraidState *b, uint8_t disk_id, uint64_t total_blocks);
error_t BraidRemoveDisk(BraidState *b, uint8_t disk_id);
error_t BraidSetDiskOnline(BraidState *b, uint8_t disk_id, bool online);

// Block operations. A mirror or weave reads the first member that answers
// (and whose copy matches `expected_checksum` when one is given); a stripe
// reads the one member its block lives on.
error_t BraidReadBlock(BraidState *b, uint64_t block_num, void *data, const BoxHash *expected_checksum);
error_t BraidWriteBlock(BraidState *b, uint64_t block_num, const void *data, const uint8_t *tag_context);

// Tag-aware operations
error_t BraidReadBlockTagged(BraidState *b, uint64_t block_num, void *data, const uint8_t *tag_context);
error_t BraidWriteBlockTagged(BraidState *b, uint64_t block_num, const void *data, const uint8_t *tag_context);

// Auto-healing
error_t BraidAutoHeal(BraidState *b, uint64_t block_num);

// Statistics
error_t BraidGetStats(BraidState *b, BraidStats *stats);
error_t BraidPrintStats(BraidState *b);

// Health check
bool    BraidIsHealthy(BraidState *b);
uint8_t BraidGetActiveDiskCount(BraidState *b);

#endif // BRAID_H
