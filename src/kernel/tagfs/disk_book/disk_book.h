#ifndef DISK_BOOK_H
#define DISK_BOOK_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../tagfs_constants.h"
#include "../../core/error/error.h"

// ============================================================================
// DiskBook — durable, content-addressed redirect log for TagFS CoW snapshots
//
// DiskBook's one load-bearing job is making the CoW redirect map (old_block →
// new_block, per snapshot) survive a power cut. TagFS metadata consistency is
// already guaranteed by append-ordering + mount fsck, so a general write-ahead
// log over that would be redundant. What fsck CANNOT reconstruct is which OLD
// physical block a snapshot froze when a live write was redirected — that
// mapping lived only in RAM and was lost on reboot, so after a crash a
// snapshot read returned live data instead of its frozen view.
//
// Model: a dense on-disk array of CRC-protected, tag-scoped redirect records.
// `count` (in the superblock) is authoritative — records [0..count) are live.
// Appended on each CoW redirect; compacted (rewritten from the live in-memory
// set) when a snapshot is deleted. Replayed at mount into the restored
// snapshots. Each record is content-addressed (CRC32); a torn record is
// skipped. The array persists across clean reboots — redirects live until
// their snapshot is deleted, NOT until shutdown.
//
// Bound: DISK_BOOK_CAPACITY live redirects. Overflow is graceful — the redirect
// is not journaled and that snapshot block falls back to live data on the next
// crash recovery (never corruption). The in-memory redirect list (cow.c) stays
// authoritative at runtime and is unbounded; DiskBook is its durable mirror.
// ============================================================================

#define DISK_BOOK_MAGIC             0x44424F4F  /* "DBOO" — redirect record    */
#define DISK_BOOK_SB_MAGIC          0x44425342  /* "DBSB" — superblock         */
#define DISK_BOOK_VERSION           2
#define DISK_BOOK_ENTRY_SIZE        1024
#define DISK_BOOK_SECTORS_PER_ENTRY 2
#define DISK_BOOK_CAPACITY          512
#define DISK_BOOK_FLUSH_THRESH      64     /* flush cache every N appends      */

// Record types
#define DISK_BOOK_TYPE_REDIRECT     0x01    /* CoW redirect: old_block→new_block */

typedef struct __packed {
    uint32_t magic;          /* DISK_BOOK_SB_MAGIC                           */
    uint32_t version;        /* DISK_BOOK_VERSION                            */
    uint64_t start_sector;   /* first record sector                          */
    uint32_t capacity;       /* max live records (DISK_BOOK_CAPACITY)        */
    uint32_t count;          /* authoritative live record count [0..count)  */
    uint32_t generation;     /* monotonic; bumped on every mutation         */
    uint32_t flags;
    uint8_t  uuid[16];
    uint8_t  reserved[464];
} DiskBookSuperblock;

STATIC_ASSERT(sizeof(DiskBookSuperblock) == 512, "DiskBookSuperblock_must_be_512_bytes");

// One record = DISK_BOOK_ENTRY_SIZE (1024 B = 2 sectors). The redirect payload
// is tiny; the rest is reserved headroom so the on-disk geometry (2 sectors per
// record, 512 records, 1024-sector region) is unchanged from the v1 layout.
typedef struct __packed {
    uint32_t magic;          /* DISK_BOOK_MAGIC                              */
    uint32_t sequence;       /* monotonic generation stamp at write time    */
    uint16_t type;           /* DISK_BOOK_TYPE_*                            */
    uint16_t flags;
    uint32_t snapshot_id;    /* snapshot that froze old_block               */
    uint32_t old_block;      /* frozen (redirected-away) block              */
    uint32_t new_block;      /* live block the file now points at           */
    uint64_t tag_bits;       /* tag scope of the owning file (well-known)   */
    uint32_t record_crc32;   /* CRC32 of the record with this field zeroed  */
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

// Public API
error_t  DiskBookInit(uint32_t superblock_sector);
error_t  DiskBookValidateAndReplay(void);   /* restore redirects into CoW    */
void     DiskBookShutdown(void);

/* Append one CoW redirect durably. tag_bits = owning file's well-known tag
 * bitmask (0 if unknown). Graceful on overflow (returns ERR_DISK_FULL). */
error_t  DiskBookLogRedirect(uint32_t snapshot_id, uint32_t old_block,
                             uint32_t new_block, uint64_t tag_bits);

/* Drop all on-disk redirects (count→0). Used to compact: caller then re-logs
 * the surviving redirects. */
void     DiskBookResetRedirects(void);

error_t  DiskBookCheckpoint(void);          /* flush cache + persist          */

error_t  DiskBookGetStats(DiskBookStats* stats);
error_t  DiskBookPrintStats(void);

bool     DiskBookIsInitialized(void);
uint32_t DiskBookGetCheckpointSeq(void);    /* current generation             */

#endif // DISK_BOOK_H
