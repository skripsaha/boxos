#include "disk_book.h"
#include "../tagfs.h"
#include "../cow/cow.h"
#include "../../lib/kernel/klib.h"
#include "../../lib/kernel/crypto.h"
#include "../../../kernel/drivers/timer/rtc.h"
#include "../../../kernel/drivers/disk/ahci.h"
#include "boardroom.h"
#include "atomics.h"      /* cpu_pause — the one instruction a re-ask spends */

// Sector I/O through the volume's own door (tagfs_volume_read/write), not the
// Boardroom directly: the DiskBook lives in a run of the volume named by its
// Deed, so every sector it names is counted from the volume's start. Going to
// the medium itself would mean knowing where the volume begins, which is
// exactly the knowledge that stopped a volume from being able to move.
static int disk_book_read_sectors(uint64_t vlba, uint16_t count, void *buf) {
    return tagfs_volume_read(vlba, count, buf);
}

static int disk_book_write_sectors(uint64_t vlba, uint16_t count, const void *buf) {
    return tagfs_volume_write(vlba, count, buf);
}

// ----------------------------------------------------------------------------
// Global state
// ----------------------------------------------------------------------------
static DiskBookSuperblock g_sb;
static bool      g_initialized       = false;
static TagFSLock g_lock              = TAGFS_LOCK_INIT("diskbook", LOCK_RANK_LEAF);
static uint64_t  g_sb_sector         = 0;   /* all three counted from the */
static uint64_t  g_sb_backup_sector  = 0;   /* start of the volume         */
static uint32_t  g_redirects_logged  = 0;
static uint32_t  g_replay_count      = 0;
static uint32_t  g_crc_errors        = 0;
static uint32_t  g_appends_since_flush = 0;
static uint64_t  g_init_time         = 0;

// CRC32 over a record with its record_crc32 field treated as zero.
static uint32_t entry_crc(const DiskBookEntry *e) {
    DiskBookEntry tmp = *e;
    tmp.record_crc32 = 0;
    return KCrc32((const uint8_t *)&tmp, sizeof(DiskBookEntry));
}

static uint64_t record_sector(uint32_t idx) {
    return g_sb.start_sector + (uint64_t)idx * DISK_BOOK_SECTORS_PER_ENTRY;
}

// ----------------------------------------------------------------------------
// Superblock I/O (primary + backup, each exactly one sector)
// ----------------------------------------------------------------------------
static int write_superblock_locked(void) {
    /* Stamp CRC32 over the whole block with the crc32 field zeroed, so a torn
     * superblock write is detected on the next mount and the backup is used. */
    g_sb.crc32 = 0;
    g_sb.crc32 = KCrc32((const uint8_t *)&g_sb, sizeof(DiskBookSuperblock));

    uint8_t buf[TAGFS_SECTOR_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &g_sb, sizeof(DiskBookSuperblock));
    if (disk_book_write_sectors(g_sb_sector, 1, buf) != 0)
        return ERR_IO;
    disk_book_write_sectors(g_sb_backup_sector, 1, buf);   /* best-effort mirror */
    return OK;
}

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
        debug_printf("[DiskBook] primary superblock invalid (magic/CRC) — trying backup\n");
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
    uint8_t buf[DISK_BOOK_ENTRY_SIZE];
    if (disk_book_read_sectors(record_sector(idx), DISK_BOOK_SECTORS_PER_ENTRY, buf) != 0)
        return ERR_IO;
    memcpy(out, buf, sizeof(DiskBookEntry));
    return OK;
}

static int write_entry(uint32_t idx, const DiskBookEntry *e) {
    if (idx >= g_sb.capacity)
        return ERR_INVALID_ARGUMENT;
    uint8_t buf[DISK_BOOK_ENTRY_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, e, sizeof(DiskBookEntry));
    if (disk_book_write_sectors(record_sector(idx), DISK_BOOK_SECTORS_PER_ENTRY, buf) != 0)
        return ERR_IO;
    return OK;
}

static void format_fresh_locked(uint64_t records_sector) {
    memset(&g_sb, 0, sizeof(g_sb));
    g_sb.magic        = DISK_BOOK_SB_MAGIC;
    g_sb.version      = DISK_BOOK_VERSION;
    g_sb.start_sector = records_sector;
    g_sb.capacity     = DISK_BOOK_CAPACITY;
    g_sb.count        = 0;
    g_sb.generation   = 0;
    g_sb.flags        = 0;
}

// ----------------------------------------------------------------------------
// Init
// ----------------------------------------------------------------------------
error_t DiskBookInit(uint64_t head_sector, uint64_t backup_sector,
                    uint64_t records_sector) {
    if (g_initialized)
        return ERR_DISKBOOK_NOT_INITIALIZED;

    TagFSLockInit(&g_lock, "diskbook", LOCK_RANK_LEAF);
    g_sb_sector        = head_sector;
    g_sb_backup_sector = backup_sector;

    DiskBookSuperblock disk;
    /* read_superblock_into() needs g_sb_sector set (done above); it fills `disk`. */
    int rc = read_superblock_into(&disk);
    if (rc == OK && disk.version == DISK_BOOK_VERSION &&
        disk.capacity == DISK_BOOK_CAPACITY &&
        disk.count <= DISK_BOOK_CAPACITY &&
        disk.start_sector == records_sector) {
        g_sb = disk;
        debug_printf("[DiskBook] Existing redirect log: %u redirects, gen=%u\n",
                     g_sb.count, g_sb.generation);
    } else {
        format_fresh_locked(records_sector);
        if (write_superblock_locked() != OK) {
            debug_printf("[DiskBook] Init failed: superblock write error\n");
            return ERR_DISKBOOK_WRITE_FAILED;
        }
        debug_printf("[DiskBook] Fresh redirect log (capacity %u)\n", g_sb.capacity);
    }

    g_initialized = true;
    g_init_time   = rtc_get_unix64();
    return OK;
}

// ----------------------------------------------------------------------------
// Replay — restore durable redirects into the (already-initialized) CoW layer.
// Must run AFTER TagFS_CowInit + CoW manifest restore so snapshots exist.
// Idempotent: TagFS_CowRestoreRedirect de-dups. Content-addressed: every record
// is CRC-verified; a torn record is counted and skipped, never applied.
// ----------------------------------------------------------------------------
error_t DiskBookValidateAndReplay(void) {
    if (!g_initialized)
        return ERR_NOT_INITIALIZED;

    uint32_t applied = 0;
    for (uint32_t i = 0; i < g_sb.count && i < g_sb.capacity; i++) {
        DiskBookEntry e;
        if (read_entry(i, &e) != OK)
            break;
        if (e.magic != DISK_BOOK_MAGIC) {
            g_crc_errors++;
            continue;
        }
        if (entry_crc(&e) != e.record_crc32) {
            g_crc_errors++;
            continue;
        }
        if (e.type == DISK_BOOK_TYPE_REDIRECT) {
            TagFS_CowRestoreRedirect(e.snapshot_id, e.old_block, e.new_block);
            applied++;
        }
    }

    g_replay_count += applied;
    debug_printf("[DiskBook] Replay: %u redirects restored (%u crc errors)\n",
                 applied, g_crc_errors);
    return OK;
}

/*
 * ‼ A FLUSH THAT DID NOT HAPPEN MUST NOT BE COUNTED AS ONE — AND THE THING
 * IT COSTS IS NOT A COUNT.
 *
 * g_appends_since_flush answers one question: when does this book push itself
 * out. Both places below zero it BEFORE the flush, on the promise of a flush,
 * and when the flush refused the answer used to be thrown away — so the book
 * had forgotten that sixty-odd redirects were still only in the drive's
 * cache, and nothing anywhere remembered it.
 *
 * Winding the counter back to the threshold is NOT the repair, and was tried:
 * it makes the very next append cross the threshold again, and the one after
 * that, and every one after that, so a sick medium turns one refused flush
 * into a whole-port FLUSH CACHE EXT attempt — patience window, retries,
 * COMRESETs — on EVERY redirect, for ever. A storm dressed as diligence.
 *
 * What a refused flush actually costs is not a number of appends, it is a
 * CLAIM: this book can no longer say its on-disk records have reached the
 * media. That is one bit, it is sticky, and it is cleared by the next flush
 * that does succeed — because FLUSH CACHE EXT empties the whole write cache,
 * not the part of it this book wrote, so ONE later success makes every
 * earlier redirect durable at once. Which is also why asking again
 * immediately buys nothing: the scheduled push sixty-four appends from now
 * covers exactly the same bytes.
 */
static bool g_not_proven_durable = false;

static void disk_book_flush_did_not_happen(error_t why, const char* what)
{
    __atomic_store_n(&g_not_proven_durable, true, __ATOMIC_RELEASE);

    /* A port held for somebody else's flush is not the medium refusing
     * anything and is ordinary traffic on a busy volume: the claim is dropped
     * — nothing was pushed — but no alarm is raised. tagfs_flush_cache keeps
     * the same silence on ERR_BUSY for the same reason, and a line that cries
     * out on ordinary traffic is a line nobody reads when it matters. */
    if (why == ERR_BUSY) {
        return;
    }

    /* tagfs_flush_cache has already named the volume; this says the part only
     * the book knows: what is riding on it. */
    kprintf("[DiskBook] %s: the medium did not take its cache to the media "
            "(error %u) — the redirects written since the last flush that did "
            "succeed are not proven durable until one does\n",
            what, (unsigned)why);
}

// ----------------------------------------------------------------------------
// Append one redirect (durable mirror of the in-memory CoW redirect list)
// ----------------------------------------------------------------------------
error_t DiskBookLogRedirect(uint32_t snapshot_id, uint32_t old_block,
                            uint32_t new_block, uint64_t tag_bits) {
    if (!g_initialized)
        return ERR_NOT_INITIALIZED;
    if (old_block == 0 || new_block == 0)
        return ERR_INVALID_ARGUMENT;

    TagFSLockTake(&g_lock);

    if (g_sb.count >= g_sb.capacity) {
        TagFSLockDrop(&g_lock);
        /* Graceful: in-memory redirect stays authoritative this uptime; only
         * crash recovery beyond the cap degrades to live-data reads. */
        debug_printf("[DiskBook] redirect log full (%u) — not journaled\n", g_sb.capacity);
        return ERR_DISK_FULL;
    }

    uint32_t idx = g_sb.count;

    DiskBookEntry e;
    memset(&e, 0, sizeof(e));
    e.magic       = DISK_BOOK_MAGIC;
    e.sequence    = ++g_sb.generation;
    e.type        = DISK_BOOK_TYPE_REDIRECT;
    e.snapshot_id = snapshot_id;
    e.old_block   = old_block;
    e.new_block   = new_block;
    e.tag_bits    = tag_bits;
    e.record_crc32 = entry_crc(&e);

    if (write_entry(idx, &e) != OK) {
        g_sb.generation--;   /* roll back the bump; slot not written */
        TagFSLockDrop(&g_lock);
        return ERR_IO;
    }

    g_sb.count++;
    write_superblock_locked();   /* persist count+generation (durable on next flush) */
    g_redirects_logged++;

    bool do_flush = (++g_appends_since_flush >= DISK_BOOK_FLUSH_THRESH);
    if (do_flush)
        g_appends_since_flush = 0;

    TagFSLockDrop(&g_lock);

    /* The entry IS written and this function's word is "logged", not
     * "durable" — its own comment above says so ("durable on next flush").
     * So the answer changes the BOOK'S STATE rather than this return value:
     * what a refusal costs is the promise that the periodic push happened,
     * and that promise is the count. */
    if (do_flush) {
        error_t flushed = tagfs_flush_cache();
        if (flushed != OK) {
            disk_book_flush_did_not_happen(flushed, "redirect log");
        } else {
            __atomic_store_n(&g_not_proven_durable, false, __ATOMIC_RELEASE);
        }
    }
    return OK;
}

// ----------------------------------------------------------------------------
// Compaction support: drop all on-disk redirects. Caller (CoW snapshot delete)
// then re-logs the surviving redirects so dead ones are reclaimed.
// ----------------------------------------------------------------------------
void DiskBookResetRedirects(void) {
    if (!g_initialized)
        return;
    TagFSLockTake(&g_lock);
    g_sb.count = 0;
    g_sb.generation++;
    write_superblock_locked();
    TagFSLockDrop(&g_lock);
}

// ----------------------------------------------------------------------------
// Checkpoint / shutdown — persist + push the drive cache to media.
// Redirects PERSIST across reboot (they live until their snapshot is deleted),
// so shutdown does NOT clear the log — it only makes it durable.
// ----------------------------------------------------------------------------
error_t DiskBookCheckpoint(void) {
    if (!g_initialized)
        return ERR_NOT_INITIALIZED;
    TagFSLockTake(&g_lock);
    error_t err = write_superblock_locked();
    g_appends_since_flush = 0;
    TagFSLockDrop(&g_lock);
    if (err != OK)
        return err;

    /* ‼ THIS ONE IS A DURABILITY CLAIM, AND IT USED TO RETURN SOMEBODY ELSE'S
     * ANSWER. The value handed back was write_superblock_locked's — "the
     * sectors were accepted" — while the flush that decides whether they are
     * on the media had its answer dropped. A checkpoint whose whole purpose
     * is the sentence "this is now safe against the power going away"
     * returned OK when it was not.
     *
     * ‼ ERR_BUSY IS NOT ONE OF THOSE REFUSALS AND IS NOT RETURNED. It says
     * another whole-port command holds this medium's port at this instant —
     * two flushes of the same disk meeting — and a checkpoint is asked for
     * precisely when somebody needs an answer about the media, not about the
     * traffic. Returning it would also have made this function fail at
     * random on a busy volume, which the checkpoint test in
     * tagfs/tests/journal_tests.c (diskbook_checkpoint) would see as a red
     * run with nothing wrong.
     *
     * So it asks again, and the asking ends on a fact rather than a clock:
     * the holder's own command is bounded, and a holder that stops being a
     * running core has its door taken over on the spot
     * (ahci_port_take_whole). The argument is written out once, above
     * tagfs_flush_cache_insisting in tagfs.c. */
    error_t flushed = tagfs_flush_cache();
    while (flushed == ERR_BUSY) {
        cpu_pause();
        flushed = tagfs_flush_cache();
    }
    if (flushed != OK) {
        disk_book_flush_did_not_happen(flushed, "checkpoint");
        return flushed;
    }
    __atomic_store_n(&g_not_proven_durable, false, __ATOMIC_RELEASE);
    return OK;
}

void DiskBookShutdown(void) {
    if (!g_initialized)
        return;
    /* Said out loud rather than dropped for the same reason as everything
     * above: this is the last checkpoint this book ever gets, and if it did
     * not stick, the only place that can still be known is here.
     *
     * And what is said is the book's WHOLE debt, not this call's: a periodic
     * push that refused an hour ago left redirects in the drive's cache that
     * every checkpoint since may have failed to cover, and
     * g_not_proven_durable is the one bit that remembers it. */
    error_t sealed = DiskBookCheckpoint();
    bool    owed   = __atomic_load_n(&g_not_proven_durable, __ATOMIC_ACQUIRE);

    TagFSLockTake(&g_lock);
    uint32_t held = g_sb.count;      /* read under the lock that guards it */
    g_initialized = false;
    TagFSLockDrop(&g_lock);

    if (sealed != OK || owed) {
        kprintf("[DiskBook] shutting down with %u redirects that were not "
                "proven to reach the media (error %u) — a snapshot read after "
                "the next boot may fall back to live data\n",
                held, (unsigned)sealed);
    }
    debug_printf("[DiskBook] Shutdown complete (%u redirects retained)\n", held);
}

// ----------------------------------------------------------------------------
// Stats / introspection
// ----------------------------------------------------------------------------
error_t DiskBookGetStats(DiskBookStats *stats) {
    if (!stats)
        return ERR_INVALID_ARGUMENT;
    if (!g_initialized) {
        memset(stats, 0, sizeof(*stats));
        return ERR_NOT_INITIALIZED;
    }
    TagFSLockTake(&g_lock);
    stats->total_entries    = g_sb.capacity;
    stats->used_entries     = g_sb.count;
    stats->free_entries     = g_sb.capacity - g_sb.count;
    stats->redirects_logged = g_redirects_logged;
    stats->replay_count     = g_replay_count;
    stats->crc_errors       = g_crc_errors;
    stats->generation       = g_sb.generation;
    stats->uptime_seconds   = (uint32_t)(rtc_get_unix64() - g_init_time);
    TagFSLockDrop(&g_lock);
    return OK;
}

error_t DiskBookPrintStats(void) {
    if (!g_initialized)
        return ERR_NOT_INITIALIZED;
    DiskBookStats s;
    DiskBookGetStats(&s);
    debug_printf("\n=== DiskBook (CoW redirect log) ===\n");
    debug_printf("Capacity:  %u\n", s.total_entries);
    debug_printf("Live:      %u\n", s.used_entries);
    debug_printf("Logged:    %u\n", s.redirects_logged);
    debug_printf("Replayed:  %u\n", s.replay_count);
    debug_printf("CRC errs:  %u\n", s.crc_errors);
    debug_printf("Gen:       %u\n", s.generation);
    debug_printf("===================================\n");
    return OK;
}

bool DiskBookIsInitialized(void) {
    return g_initialized;
}

uint32_t DiskBookGetCheckpointSeq(void) {
    return g_sb.generation;
}
