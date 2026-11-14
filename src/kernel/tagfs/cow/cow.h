#ifndef COW_H
#define COW_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../tagfs_constants.h"
#include "../../core/error/error.h"

// CoW snapshots on format v1: refused, and those an earlier kernel left, named

// On-disk snapshot manifest
#define COW_MANIFEST_MAGIC  0x434F564D   // "COVM"
#define COW_MANIFEST_MAX    60           // fits in one 4096-byte TagFS block

/*
 * ‼ A SNAPSHOT ON FORMAT V1 COULD BE TAKEN AND NEVER READ.
 *
 * Nothing anywhere read a file as a snapshot kept it, and a snapshot of the
 * volume did not even record which files it held. What taking one did do was
 * send every write of the file through a copy under an interrupts-off lock and
 * a redirect log on the medium. So on this format every request for a
 * snapshot is answered ERR_UNSUPPORTED (TagFS_SnapshotRefused), and nothing on
 * the write path asks copy-on-write anything: a write goes where the file's
 * blocks are. Snapshots arrive with format v2, as bookmarks that can be read.
 *
 * A volume an earlier kernel wrote may still carry snapshots — the manifest
 * the Ledger names and the redirects in the DiskBook. The mount reads them and
 * names them, and keeps nothing of them: with no reader of a v1 snapshot, the
 * blocks they kept are owned by nothing, so the mount's census names them
 * among its orphans and keeps them taken until it gives orphans back (owner
 * decision 3). The manifest the Ledger names is the one block claimed here.
 */

// On-disk per-snapshot entry (68 bytes each)
typedef struct __packed {
    uint32_t snapshot_id;
    uint32_t parent_file_id;
    uint64_t created_time;
    uint32_t disk_book_checkpoint;
    char     name[32];
    uint8_t  flags;
    uint8_t  _pad[3];
    uint32_t file_count;
    uint64_t total_size;
} CowSnapshotDisk;

// On-disk manifest block (must fit in TAGFS_BLOCK_SIZE = 4096 bytes)
// Header (8) + 60 * 68 = 4088; pad to 4096.
typedef struct __packed {
    uint32_t        magic;
    uint32_t        count;
    CowSnapshotDisk entries[COW_MANIFEST_MAX];
    uint8_t         _pad[8];
} CowManifest;

STATIC_ASSERT(sizeof(CowManifest) == 4096, "CowManifest must be 4096 bytes");


/* One snapshot an earlier kernel left, as read: what its manifest entry says
 * of it, and every block its redirects say it kept, each once. */
typedef struct {
    uint32_t  snapshot_id;
    uint32_t  parent_file_id;               /* 0: the whole volume */
    char      name[sizeof(((CowSnapshotDisk *)0)->name) + 1];
    uint32_t *kept;
    uint32_t  kept_count;
    uint32_t  kept_cap;
} CowSnapshot;

/* The snapshots of one manifest, with the DiskBook's redirects read into them. */
typedef struct {
    CowSnapshot *snapshots;
    uint32_t     count;
    uint32_t     stray;          /* redirects naming no snapshot of the manifest */
    error_t      book;           /* the DiskBook's replay: OK, or why nothing was replayed */
    bool         short_of_memory;
} CowReading;

/* The manifest at data block `manifest_block` (0: none) and the DiskBook at
 * volume block `book_block`, read into `out` — the book only when the
 * manifest names a snapshot. Writes nothing, holds nothing but `out`. */
void CowReadStanding(uint32_t manifest_block, uint32_t book_block, CowReading *out);

/* A line for the lot and one for each snapshot. */
void CowSayStanding(const CowReading *reading);

void CowReadingDrop(CowReading *reading);

/* The mount: the snapshots of the manifest the Ledger names, read, named out
 * loud, and let go. */
void CowNameStanding(void);

/* The mount's census: the manifest at `manifest_block`, the one block of the
 * data run copy-on-write owns. */
void CowClaimGround(uint32_t manifest_block);

#endif // COW_H
