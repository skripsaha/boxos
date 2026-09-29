#ifndef COW_H
#define COW_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../lock_rank/lock_rank.h"
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
 * the Ledger names and the redirects in the DiskBook. They are read at mount,
 * named, and held in memory as they were found, so the blocks they kept stay
 * taken (CowRollCall) until the volume's census returns them.
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

error_t TagFS_CowInit(void);
void    TagFS_CowShutdown(void);

/* The answer every snapshot request gets on this format — create, delete,
 * list or info, named in `asked`: ERR_UNSUPPORTED, said once a boot. */
error_t TagFS_SnapshotRefused(const char *asked);

/* The snapshots an earlier kernel left on this volume: the manifest the
 * Ledger names at `manifest_block` (0 when it names none), and the DiskBook's
 * redirects into them. Read, held as found, and named out loud. Called once,
 * by the mount, after the DiskBook is opened and before the roll call. */
void    CowNameStanding(uint32_t manifest_block);

/* Mark the blocks of the data run that belong to copy-on-write into the map
 * the mount's roll call is building. Called from the fsck in tagfs_init,
 * beside IntegrityMarkMapBlocks; marks only, never clears. */
void    CowRollCall(uint8_t *computed_bm, uint32_t total_blocks);

#endif // COW_H
