#ifndef DISK_BOOK_H
#define DISK_BOOK_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../tagfs_constants.h"
#include "../../core/error/error.h"

// ============================================================================
// DiskBook — the redirect log an earlier kernel kept for its CoW snapshots
//
// A kernel that took snapshots on format v1 logged here, for each block a
// write copied away from a snapshot, which block the snapshot kept (old) and
// which the file moved onto (new): a dense array of CRC-protected records,
// `count` of them live, behind a head and its copy.
//
// Nothing appends to it any more. A format-v1 volume refuses every snapshot
// request (TagFS_SnapshotRefused), and no write copies a block for one, so the
// book is only ever READ, once, by the mount that names the snapshots an
// earlier kernel left (CowNameStanding). It goes with format v2, whose
// snapshots are bookmarks that need no log.
// ============================================================================

#define DISK_BOOK_MAGIC             0x44424F4F  /* "DBOO" — redirect record    */
#define DISK_BOOK_SB_MAGIC          0x44425342  /* "DBSB" — superblock         */
#define DISK_BOOK_VERSION           2
#define DISK_BOOK_ENTRY_SIZE        1024
#define DISK_BOOK_SECTORS_PER_ENTRY 2
#define DISK_BOOK_CAPACITY          512

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
    uint32_t crc32;          /* CRC32 of this 512-byte block, crc32 field=0 */
    uint8_t  uuid[16];
    uint8_t  reserved[460];
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

/* One redirect the replay read and found whole. */
typedef void (*DiskBookRedirectSeen)(uint32_t snapshot_id, uint32_t old_block,
                                     uint32_t new_block, void *ctx);

/*
 * Every live redirect record of the DiskBook that begins at volume block
 * `book_block` (the Deed's disk_book_block), read off the medium, checked, and
 * handed to `seen` in the order the book holds them; a record that fails its
 * magic or its checksum is counted, said, and skipped. Writes nothing.
 *
 * OK once a head was taken, whatever its records held; ERR_TAGFS_CORRUPTED
 * when neither copy of the head is one this kernel takes — a book of another
 * version or geometry, or a checksum failed in both — and nothing is
 * replayed; ERR_NO_MEMORY when there was no room to read it through.
 */
error_t DiskBookReplay(uint32_t book_block, DiskBookRedirectSeen seen, void *ctx);

#endif // DISK_BOOK_H
