#include "disk_book.h"
#include "../tagfs.h"
#include "../cow/cow.h"
#include "../../lib/kernel/klib.h"
#include "../../lib/kernel/crypto.h"
#include "../../../kernel/drivers/timer/rtc.h"
#include "../../../kernel/drivers/disk/ahci.h"
#include "boardroom.h"

static int disk_book_read_sectors(uint64_t vlba, uint16_t count, void *buf) {
    return tagfs_volume_read(vlba, count, buf);
}

static int disk_book_write_sectors(uint64_t vlba, uint16_t count, const void *buf) {
    return tagfs_volume_write(vlba, count, buf);
}

static DiskBookSuperblock g_sb;
static bool      g_initialized       = false;
static spinlock_t g_lock;
static uint64_t  g_sb_sector         = 0;
static uint64_t  g_sb_backup_sector  = 0;
static uint32_t  g_redirects_logged  = 0;
static uint32_t  g_replay_count      = 0;
static uint32_t  g_crc_errors        = 0;
static uint32_t  g_appends_since_flush = 0;
static uint64_t  g_init_time         = 0;

static uint32_t entry_crc(const DiskBookEntry *e) {
    DiskBookEntry tmp = *e;
    tmp.record_crc32 = 0;
    return KCrc32((const uint8_t *)&tmp, sizeof(DiskBookEntry));
}

static uint64_t record_sector(uint32_t idx) {
    return g_sb.start_sector + (uint64_t)idx * DISK_BOOK_SECTORS_PER_ENTRY;
}

static int write_superblock_locked(void) {
    g_sb.crc32 = 0;
    g_sb.crc32 = KCrc32((const uint8_t *)&g_sb, sizeof(DiskBookSuperblock));

    uint8_t buf[TAGFS_SECTOR_SIZE];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &g_sb, sizeof(DiskBookSuperblock));
    if (disk_book_write_sectors(g_sb_sector, 1, buf) != 0)
        return ERR_IO;
    disk_book_write_sectors(g_sb_backup_sector, 1, buf);
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

error_t DiskBookInit(uint64_t head_sector, uint64_t backup_sector,
                    uint64_t records_sector) {
    if (g_initialized)
        return ERR_DISKBOOK_NOT_INITIALIZED;

    spinlock_init(&g_lock);
    g_sb_sector        = head_sector;
    g_sb_backup_sector = backup_sector;

    DiskBookSuperblock disk;
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

error_t DiskBookLogRedirect(uint32_t snapshot_id, uint32_t old_block,
                            uint32_t new_block, uint64_t tag_bits) {
    if (!g_initialized)
        return ERR_NOT_INITIALIZED;
    if (old_block == 0 || new_block == 0)
        return ERR_INVALID_ARGUMENT;

    spin_lock(&g_lock);

    if (g_sb.count >= g_sb.capacity) {
        spin_unlock(&g_lock);
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
        g_sb.generation--;
        spin_unlock(&g_lock);
        return ERR_IO;
    }

    g_sb.count++;
    write_superblock_locked();
    g_redirects_logged++;

    bool do_flush = (++g_appends_since_flush >= DISK_BOOK_FLUSH_THRESH);
    if (do_flush)
        g_appends_since_flush = 0;

    spin_unlock(&g_lock);

    if (do_flush)
        tagfs_flush_cache();
    return OK;
}

void DiskBookResetRedirects(void) {
    if (!g_initialized)
        return;
    spin_lock(&g_lock);
    g_sb.count = 0;
    g_sb.generation++;
    write_superblock_locked();
    spin_unlock(&g_lock);
}

error_t DiskBookCheckpoint(void) {
    if (!g_initialized)
        return ERR_NOT_INITIALIZED;
    spin_lock(&g_lock);
    error_t err = write_superblock_locked();
    g_appends_since_flush = 0;
    spin_unlock(&g_lock);
    if (err == OK)
        tagfs_flush_cache();
    return err;
}

void DiskBookShutdown(void) {
    if (!g_initialized)
        return;
    DiskBookCheckpoint();
    spin_lock(&g_lock);
    g_initialized = false;
    spin_unlock(&g_lock);
    debug_printf("[DiskBook] Shutdown complete (%u redirects retained)\n", g_sb.count);
}

error_t DiskBookGetStats(DiskBookStats *stats) {
    if (!stats)
        return ERR_INVALID_ARGUMENT;
    if (!g_initialized) {
        memset(stats, 0, sizeof(*stats));
        return ERR_NOT_INITIALIZED;
    }
    spin_lock(&g_lock);
    stats->total_entries    = g_sb.capacity;
    stats->used_entries     = g_sb.count;
    stats->free_entries     = g_sb.capacity - g_sb.count;
    stats->redirects_logged = g_redirects_logged;
    stats->replay_count     = g_replay_count;
    stats->crc_errors       = g_crc_errors;
    stats->generation       = g_sb.generation;
    stats->uptime_seconds   = (uint32_t)(rtc_get_unix64() - g_init_time);
    spin_unlock(&g_lock);
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