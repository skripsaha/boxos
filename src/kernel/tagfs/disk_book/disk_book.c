#include "disk_book.h"
#include "../tagfs.h"
#include "../../lib/kernel/klib.h"
#include "../../lib/kernel/crypto.h"

// Sector I/O through the volume's own door (tagfs_volume_read), not the
// Boardroom directly: the DiskBook lives in a run of the volume named by its
// Deed, so every sector it names is counted from the volume's start. Going to
// the medium itself would mean knowing where the volume begins, which is
// exactly the knowledge that stopped a volume from being able to move.
static int disk_book_read_sectors(uint64_t vlba, uint16_t count, void *buf) {
    return tagfs_volume_read(vlba, count, buf);
}

// ----------------------------------------------------------------------------
// Global state
//
// Written only by DiskBookInit and read only by the replay, both on the road
// that mounts the volume, and forgotten by DiskBookShutdown on the road that
// takes it down: nothing reaches the book between the two, so it needs no
// lock.
// ----------------------------------------------------------------------------
static DiskBookSuperblock g_sb;
static bool      g_initialized       = false;
static uint64_t  g_sb_sector         = 0;   /* both counted from the */
static uint64_t  g_sb_backup_sector  = 0;   /* start of the volume   */

// Whether a record is whole: its magic, and a CRC32 over it with its
// record_crc32 field taken as zero. Asked in place, so the record is not
// copied a second time onto the stack to be checked.
static bool entry_whole(DiskBookEntry *e) {
    uint32_t stored = e->record_crc32;
    e->record_crc32 = 0;
    bool whole = e->magic == DISK_BOOK_MAGIC &&
                 KCrc32((const uint8_t *)e, sizeof(DiskBookEntry)) == stored;
    e->record_crc32 = stored;
    return whole;
}

static uint64_t record_sector(uint32_t idx) {
    return g_sb.start_sector + (uint64_t)idx * DISK_BOOK_SECTORS_PER_ENTRY;
}

// ----------------------------------------------------------------------------
// The head (primary + backup, each exactly one sector)
// ----------------------------------------------------------------------------
static bool sb_valid(const DiskBookSuperblock *sb) {
    if (sb->magic != DISK_BOOK_SB_MAGIC)
        return false;
    DiskBookSuperblock t = *sb;
    uint32_t stored = t.crc32;
    t.crc32 = 0;
    return KCrc32((const uint8_t *)&t, sizeof(t)) == stored;
}

static int read_superblock_into(DiskBookSuperblock *out) {
    uint8_t buf[TAGFS_SECTOR_SIZE];
    if (disk_book_read_sectors(g_sb_sector, 1, buf) == 0) {
        memcpy(out, buf, sizeof(DiskBookSuperblock));
        if (sb_valid(out))
            return OK;
    }
    if (disk_book_read_sectors(g_sb_backup_sector, 1, buf) != 0)
        return ERR_CORRUPTED;
    memcpy(out, buf, sizeof(DiskBookSuperblock));
    if (sb_valid(out))
        return OK;
    return ERR_CORRUPTED;
}

static int read_entry(uint32_t idx, DiskBookEntry *out) {
    if (idx >= g_sb.capacity)
        return ERR_INVALID_ARGUMENT;
    if (disk_book_read_sectors(record_sector(idx), DISK_BOOK_SECTORS_PER_ENTRY, out) != 0)
        return ERR_IO;
    return OK;
}

// ----------------------------------------------------------------------------
// Init
// ----------------------------------------------------------------------------
/*
 * ‼ READS THE HEAD AND WRITES NOTHING.
 *
 * A head this kernel does not take — a fresh volume that never had one, a
 * version or a geometry of somebody else's, a checksum that fails in both
 * copies — used to be answered by formatting an empty book over it at mount.
 * Nothing appends to the book any more, so that head would be a write no
 * reader ever needs; the book is simply taken as holding no record to replay.
 * A kernel from before this one formats its own head when it mounts the
 * volume, as it always did.
 */
error_t DiskBookInit(uint64_t head_sector, uint64_t backup_sector,
                    uint64_t records_sector) {
    if (g_initialized)
        return ERR_ALREADY_INITIALIZED;

    g_sb_sector        = head_sector;
    g_sb_backup_sector = backup_sector;

    DiskBookSuperblock disk;
    if (read_superblock_into(&disk) == OK && disk.version == DISK_BOOK_VERSION &&
        disk.capacity == DISK_BOOK_CAPACITY &&
        disk.count <= DISK_BOOK_CAPACITY &&
        disk.start_sector == records_sector) {
        g_sb = disk;
    } else {
        memset(&g_sb, 0, sizeof(g_sb));
    }

    g_initialized = true;
    return OK;
}

// ----------------------------------------------------------------------------
// Replay — every live record, checked, handed to the caller. Content-addressed:
// a torn or foreign record is counted and skipped, never handed on.
// ----------------------------------------------------------------------------
error_t DiskBookValidateAndReplay(DiskBookRedirectSeen seen, void *ctx) {
    if (!g_initialized)
        return ERR_NOT_INITIALIZED;
    if (!seen)
        return ERR_INVALID_ARGUMENT;

    uint32_t skipped = 0;
    uint32_t read    = 0;
    for (uint32_t i = 0; i < g_sb.count && i < g_sb.capacity; i++) {
        DiskBookEntry e;
        if (read_entry(i, &e) != OK)
            break;
        read++;
        if (!entry_whole(&e)) {
            skipped++;
            continue;
        }
        if (e.type == DISK_BOOK_TYPE_REDIRECT)
            seen(e.snapshot_id, e.old_block, e.new_block, ctx);
    }

    if (read < g_sb.count)
        kprintf("[DiskBook] record %u of %u would not read — the redirects from it on "
                "are not replayed\n", read, g_sb.count);
    if (skipped)
        kprintf("[DiskBook] %u of %u redirect record(s) fail their magic or checksum and "
                "are skipped\n", skipped, read);
    return OK;
}

// ----------------------------------------------------------------------------
// Shutdown — the book is forgotten, not sealed: it is only ever read.
// ----------------------------------------------------------------------------
void DiskBookShutdown(void) {
    g_initialized = false;
}

bool DiskBookIsInitialized(void) {
    return g_initialized;
}
