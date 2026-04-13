#include "disk_book.h"
#include "../tagfs.h"
#include "../../lib/kernel/klib.h"
#include "../../lib/kernel/crypto.h"
#include "../../../kernel/drivers/timer/rtc.h"
#include "../../../kernel/drivers/disk/ahci.h"
#include "../../../kernel/drivers/disk/ahci_sync.h"
#include "../../../kernel/drivers/disk/ata.h"

// Direct sector I/O — bypasses tagfs block abstraction (which adds block_to_sector offset)
static int disk_book_read_sectors(uint64_t lba, uint16_t count, void *buf) {
    if (ahci_is_initialized())
        return ahci_read_sectors_sync(0, lba, count, buf);
    return ata_read_sectors_retry(1, lba, count, (uint8_t *)buf);
}

static int disk_book_write_sectors(uint64_t lba, uint16_t count, const void *buf) {
    if (ahci_is_initialized())
        return ahci_write_sectors_sync(0, lba, count, buf);
    return ata_write_sectors_retry(1, lba, count, (const uint8_t *)buf);
}

// Global state
static DiskBookSuperblock g_disk_book_sb;
static bool g_disk_book_initialized = false;
static spinlock_t g_disk_book_lock;
static uint32_t g_disk_book_sector;
static uint32_t g_disk_book_backup_sector;
static uint32_t g_disk_book_capacity;
static uint32_t g_disk_book_entries_since_checkpoint = 0;
static uint64_t g_disk_book_last_checkpoint_time = 0;
static uint32_t g_disk_book_writes_total = 0;
static uint32_t g_disk_book_replay_count = 0;
static uint32_t g_disk_book_crc_errors = 0;
static uint64_t g_disk_book_bytes_logged = 0;
static uint64_t g_disk_book_init_time = 0;

// CRC32 wrapper using shared crypto library
static uint32_t DiskBookCrc32(const uint8_t* data, uint32_t len) {
    return KCrc32(data, len);
}

// Ring buffer count
static uint32_t DiskBookCount(uint32_t tail, uint32_t head) {
    return head >= tail ? head - tail : g_disk_book_capacity - tail + head;
}

// Check if ring buffer has space
static bool DiskBookHasSpace(uint32_t n) {
    return DiskBookCount(g_disk_book_sb.tail, g_disk_book_sb.head) + n < g_disk_book_capacity - 1;
}

// Allocate entry from ring buffer
static uint32_t DiskBookAlloc(void) {
    uint32_t idx = g_disk_book_sb.head;
    g_disk_book_sb.head = (g_disk_book_sb.head + 1) % g_disk_book_capacity;
    return idx;
}

// Write superblock to disk using direct sector I/O.
// The superblock is exactly 512 bytes = 1 sector.
static int DiskBookWriteSuperblock(void) {
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &g_disk_book_sb, sizeof(DiskBookSuperblock));

    if (disk_book_write_sectors(g_disk_book_sector, 1, buf) != 0)
        return ERR_IO;
    disk_book_write_sectors(g_disk_book_backup_sector, 1, buf);
    return OK;
}

// Read superblock from disk using direct sector I/O.
static int DiskBookReadSuperblock(void) {
    uint8_t buf[512];

    if (disk_book_read_sectors(g_disk_book_sector, 1, buf) != 0)
        return ERR_IO;

    memcpy(&g_disk_book_sb, buf, sizeof(DiskBookSuperblock));

    if (g_disk_book_sb.magic != DISK_BOOK_SB_MAGIC) {
        if (disk_book_read_sectors(g_disk_book_backup_sector, 1, buf) != 0)
            return ERR_CORRUPTED;

        memcpy(&g_disk_book_sb, buf, sizeof(DiskBookSuperblock));
        if (g_disk_book_sb.magic != DISK_BOOK_SB_MAGIC)
            return ERR_CORRUPTED;

        disk_book_write_sectors(g_disk_book_sector, 1, buf);
    }
    return OK;
}

// Read entry from disk using direct sector I/O.
// Each entry is DISK_BOOK_ENTRY_SIZE (1024) bytes = DISK_BOOK_SECTORS_PER_ENTRY (2) sectors.
static int DiskBookReadEntry(uint32_t idx, DiskBookEntry* entry) {
    if (idx >= g_disk_book_capacity || !entry)
        return ERR_INVALID_ARGUMENT;

    uint8_t buf[DISK_BOOK_ENTRY_SIZE];
    uint64_t sector = g_disk_book_sb.start_sector + (uint64_t)idx * DISK_BOOK_SECTORS_PER_ENTRY;

    if (disk_book_read_sectors(sector, DISK_BOOK_SECTORS_PER_ENTRY, buf) != 0)
        return ERR_IO;

    memcpy(entry, buf, DISK_BOOK_ENTRY_SIZE);
    return OK;
}

// Write entry to disk using direct sector I/O.
static int DiskBookWriteEntry(uint32_t idx, const DiskBookEntry* entry) {
    if (idx >= g_disk_book_capacity || !entry)
        return ERR_INVALID_ARGUMENT;

    uint8_t buf[DISK_BOOK_ENTRY_SIZE];
    memset(buf, 0, DISK_BOOK_ENTRY_SIZE);
    memcpy(buf, entry, DISK_BOOK_ENTRY_SIZE);

    uint64_t sector = g_disk_book_sb.start_sector + (uint64_t)idx * DISK_BOOK_SECTORS_PER_ENTRY;
    if (disk_book_write_sectors(sector, DISK_BOOK_SECTORS_PER_ENTRY, buf) != 0)
        return ERR_IO;

    return OK;
}

// Maybe create checkpoint
static void DiskBookMaybeCheckpoint(void) {
    if (!g_disk_book_initialized)
        return;
    
    g_disk_book_entries_since_checkpoint++;

    if (g_disk_book_entries_since_checkpoint >= DISK_BOOK_CHECKPOINT_THRESH) {
        DiskBookCheckpoint();
        g_disk_book_entries_since_checkpoint = 0;
        g_disk_book_last_checkpoint_time = rtc_get_unix64();
        return;
    }
    
    uint64_t now = rtc_get_unix64();
    if (now - g_disk_book_last_checkpoint_time >= 5) {
        DiskBookCheckpoint();
        g_disk_book_entries_since_checkpoint = 0;
        g_disk_book_last_checkpoint_time = now;
    }
}

error_t DiskBookInit(uint32_t sector) {
    if (g_disk_book_initialized)
        return ERR_DISKBOOK_NOT_INITIALIZED;

    spinlock_init(&g_disk_book_lock);

    g_disk_book_sector        = sector;
    g_disk_book_backup_sector = sector + 1;
    g_disk_book_capacity      = DISK_BOOK_CAPACITY;

    // Attempt to load an existing journal before creating a fresh one.
    // If valid magic is found, the journal is intact and will be replayed.
    error_t read_err = DiskBookReadSuperblock();
    if (read_err == OK &&
        g_disk_book_sb.magic   == DISK_BOOK_SB_MAGIC &&
        g_disk_book_sb.version == DISK_BOOK_VERSION) {
        // Validate stored capacity against compiled constant
        uint32_t stored_cap = (uint32_t)(g_disk_book_sb.end_sector - g_disk_book_sb.start_sector);
        if (stored_cap == 0 || stored_cap > DISK_BOOK_CAPACITY) {
            // Capacity field is corrupt — treat as fresh
            goto fresh;
        }
        g_disk_book_capacity = stored_cap;
        debug_printf("[DiskBook] Existing journal: head=%u tail=%u seq=%u cap=%u\n",
                     g_disk_book_sb.head, g_disk_book_sb.tail,
                     g_disk_book_sb.next_sequence, g_disk_book_capacity);
    } else {
fresh:
        // No valid journal on disk — initialise a fresh one
        memset(&g_disk_book_sb, 0, sizeof(DiskBookSuperblock));
        g_disk_book_sb.magic          = DISK_BOOK_SB_MAGIC;
        g_disk_book_sb.version        = DISK_BOOK_VERSION;
        g_disk_book_sb.start_sector   = sector + 2;
        g_disk_book_sb.end_sector     = sector + 2 + DISK_BOOK_CAPACITY;
        g_disk_book_sb.head           = 0;
        g_disk_book_sb.tail           = 0;
        g_disk_book_sb.checkpoint_seq = 0;
        g_disk_book_sb.checkpoint_tail = 0;
        g_disk_book_sb.next_sequence  = 1;
        g_disk_book_sb.flags          = 0;

        error_t err = DiskBookWriteSuperblock();
        if (err != OK) {
            debug_printf("[DiskBook] Init failed: superblock write error\n");
            return ERR_DISKBOOK_WRITE_FAILED;
        }
        debug_printf("[DiskBook] Fresh journal: %u entries\n", g_disk_book_capacity);
    }

    g_disk_book_initialized = true;
    g_disk_book_init_time   = rtc_get_unix64();
    return OK;
}

error_t DiskBookReload(void) {
    if (!g_disk_book_initialized)
        return ERR_NOT_INITIALIZED;
    
    error_t err = DiskBookReadSuperblock();
    if (err != OK)
        return err;
    
    g_disk_book_capacity = (uint32_t)(g_disk_book_sb.end_sector - g_disk_book_sb.start_sector);
    
    debug_printf("[DiskBook] Reloaded: head=%u tail=%u checkpoint=%u\n",
                 g_disk_book_sb.head, g_disk_book_sb.tail, g_disk_book_sb.checkpoint_seq);
    return OK;
}

error_t DiskBookValidateAndReplay(void) {
    if (!g_disk_book_initialized)
        return ERR_NOT_INITIALIZED;
    
    debug_printf("[DiskBook] Validating and replaying...\n");
    
    uint32_t idx = g_disk_book_sb.tail;
    uint32_t applied = 0;
    
    if (g_disk_book_sb.checkpoint_seq > 0)
        idx = g_disk_book_sb.checkpoint_tail;
    
    while (idx != g_disk_book_sb.head) {
        DiskBookEntry entry;
        if (DiskBookReadEntry(idx, &entry) != OK)
            break;
        
        if (entry.magic != DISK_BOOK_MAGIC)
            break;
        
        uint32_t crc = DiskBookCrc32(entry.data, entry.data_size);
        if (crc != entry.data_crc32) {
            g_disk_book_crc_errors++;
            idx = (idx + 1) % g_disk_book_capacity;
            continue;
        }
        
        if (entry.state == DISK_BOOK_STATE_COMMITTED) {
            if (entry.type == DISK_BOOK_TYPE_DATA && entry.data_size > 0) {
                uint8_t buf[TAGFS_BLOCK_SIZE];
                memcpy(buf, entry.data, entry.data_size);
                if (tagfs_write_block(entry.disk_sector, buf) == OK)
                    applied++;
            } else if (entry.type == DISK_BOOK_TYPE_METADATA) {
                applied++;
            }
        }
        
        idx = (idx + 1) % g_disk_book_capacity;
    }
    
    g_disk_book_replay_count += applied;
    debug_printf("[DiskBook] Replay complete: %u entries applied\n", applied);
    return OK;
}

void DiskBookShutdown(void) {
    if (!g_disk_book_initialized)
        return;

    // DiskBookCheckpoint acquires g_disk_book_lock internally — do not call it
    // while already holding the lock or it will deadlock.
    DiskBookCheckpoint();

    spin_lock(&g_disk_book_lock);
    DiskBookWriteSuperblock();
    g_disk_book_initialized = false;
    spin_unlock(&g_disk_book_lock);

    debug_printf("[DiskBook] Shutdown complete\n");
}

error_t DiskBookBegin(DiskBookTxn* txn) {
    if (!txn || !g_disk_book_initialized)
        return ERR_INVALID_ARGUMENT;
    
    spin_lock(&g_disk_book_lock);
    
    if (!DiskBookHasSpace(3)) {
        spin_unlock(&g_disk_book_lock);
        debug_printf("[DiskBook] Journal full\n");
        return ERR_DISK_FULL;
    }
    
    txn->sequence = g_disk_book_sb.next_sequence++;
    txn->first_entry = DiskBookAlloc();
    txn->entry_count = 0;
    txn->active = 1;
    
    spin_unlock(&g_disk_book_lock);
    return OK;
}

error_t DiskBookLogData(DiskBookTxn* txn, uint32_t file_id, uint64_t block_offset,
                        uint32_t disk_sector, const void* data, uint16_t data_size) {
    if (!txn || !txn->active || !g_disk_book_initialized || !data || data_size == 0)
        return ERR_INVALID_ARGUMENT;
    
    if (data_size > DISK_BOOK_DATA_SIZE)
        return ERR_BUFFER_TOO_SMALL;
    
    spin_lock(&g_disk_book_lock);
    
    DiskBookEntry entry;
    memset(&entry, 0, sizeof(DiskBookEntry));
    entry.magic = DISK_BOOK_MAGIC;
    entry.sequence = txn->sequence;
    entry.type = DISK_BOOK_TYPE_DATA;
    entry.file_id = file_id;
    entry.block_offset = block_offset;
    entry.disk_sector = disk_sector;
    entry.data_size = data_size;
    entry.state = DISK_BOOK_STATE_PENDING;
    
    memcpy(entry.data, data, data_size);
    entry.data_crc32 = DiskBookCrc32(entry.data, data_size);
    
    uint32_t idx = DiskBookAlloc();
    txn->entry_count++;
    
    if (DiskBookWriteEntry(idx, &entry) != OK) {
        spin_unlock(&g_disk_book_lock);
        return ERR_IO;
    }
    
    g_disk_book_writes_total++;
    g_disk_book_bytes_logged += data_size;
    
    spin_unlock(&g_disk_book_lock);
    DiskBookMaybeCheckpoint();
    return OK;
}

error_t DiskBookLogMetadata(DiskBookTxn* txn, uint32_t file_id, uint32_t meta_sector,
                            const void* meta) {
    if (!txn || !txn->active || !g_disk_book_initialized || !meta)
        return ERR_INVALID_ARGUMENT;
    
    spin_lock(&g_disk_book_lock);
    
    DiskBookEntry entry;
    memset(&entry, 0, sizeof(DiskBookEntry));
    entry.magic = DISK_BOOK_MAGIC;
    entry.sequence = txn->sequence;
    entry.type = DISK_BOOK_TYPE_METADATA;
    entry.file_id = file_id;
    entry.disk_sector = meta_sector;
    entry.data_size = sizeof(TagFSMetadata);
    entry.state = DISK_BOOK_STATE_PENDING;
    
    memcpy(entry.data, meta, sizeof(TagFSMetadata));
    entry.data_crc32 = DiskBookCrc32(entry.data, entry.data_size);
    
    uint32_t idx = DiskBookAlloc();
    txn->entry_count++;
    
    if (DiskBookWriteEntry(idx, &entry) != OK) {
        spin_unlock(&g_disk_book_lock);
        return ERR_IO;
    }
    
    g_disk_book_writes_total++;
    g_disk_book_bytes_logged += entry.data_size;
    
    spin_unlock(&g_disk_book_lock);
    DiskBookMaybeCheckpoint();
    return OK;
}

error_t DiskBookCommit(DiskBookTxn* txn) {
    if (!txn || !txn->active || !g_disk_book_initialized)
        return ERR_INVALID_ARGUMENT;
    
    spin_lock(&g_disk_book_lock);
    
    DiskBookEntry entry;
    memset(&entry, 0, sizeof(DiskBookEntry));
    entry.magic = DISK_BOOK_MAGIC;
    entry.sequence = txn->sequence;
    entry.type = DISK_BOOK_TYPE_COMMIT;
    entry.state = DISK_BOOK_STATE_COMMITTED;
    entry.data_crc32 = DiskBookCrc32(entry.data, 0);
    
    if (DiskBookWriteEntry(DiskBookAlloc(), &entry) != OK) {
        spin_unlock(&g_disk_book_lock);
        return ERR_IO;
    }
    
    // Mark entries as committed
    uint32_t idx = txn->first_entry;
    uint32_t count = txn->entry_count;
    while (count--) {
        DiskBookEntry de;
        if (DiskBookReadEntry(idx, &de) == OK && de.type != DISK_BOOK_TYPE_COMMIT) {
            de.state = DISK_BOOK_STATE_COMMITTED;
            DiskBookWriteEntry(idx, &de);
        }
        idx = (idx + 1) % g_disk_book_capacity;
    }
    
    txn->active = 0;
    spin_unlock(&g_disk_book_lock);
    DiskBookMaybeCheckpoint();
    return OK;
}

void DiskBookAbort(DiskBookTxn* txn) {
    if (!txn || !g_disk_book_initialized)
        return;
    
    spin_lock(&g_disk_book_lock);
    
    uint32_t idx = txn->first_entry;
    uint32_t count = txn->entry_count;
    while (count--) {
        DiskBookEntry entry;
        if (DiskBookReadEntry(idx, &entry) == OK) {
            entry.state = DISK_BOOK_STATE_ABORTED;
            DiskBookWriteEntry(idx, &entry);
        }
        idx = (idx + 1) % g_disk_book_capacity;
    }

    txn->active = 0;
    spin_unlock(&g_disk_book_lock);
    debug_printf("[DiskBook] Transaction %u aborted\n", txn->sequence);
}

error_t DiskBookCheckpoint(void) {
    if (!g_disk_book_initialized)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_disk_book_lock);
    
    DiskBookEntry entry;
    memset(&entry, 0, sizeof(DiskBookEntry));
    entry.magic = DISK_BOOK_MAGIC;
    entry.sequence = g_disk_book_sb.next_sequence++;
    entry.type = DISK_BOOK_TYPE_CHECKPOINT;
    entry.state = DISK_BOOK_STATE_COMMITTED;
    entry.data_crc32 = DiskBookCrc32(entry.data, 0);
    
    if (DiskBookWriteEntry(DiskBookAlloc(), &entry) != OK) {
        spin_unlock(&g_disk_book_lock);
        return ERR_IO;
    }
    
    g_disk_book_sb.checkpoint_seq = entry.sequence;
    g_disk_book_sb.checkpoint_tail = g_disk_book_sb.tail;
    
    error_t err = DiskBookWriteSuperblock();
    spin_unlock(&g_disk_book_lock);
    
    if (err == OK)
        debug_printf("[DiskBook] Checkpoint: seq=%u\n", g_disk_book_sb.checkpoint_seq);
    
    return err;
}

error_t DiskBookGetStats(DiskBookStats* stats) {
    if (!stats)
        return ERR_INVALID_ARGUMENT;
    
    if (!g_disk_book_initialized) {
        memset(stats, 0, sizeof(DiskBookStats));
        return ERR_NOT_INITIALIZED;
    }
    
    spin_lock(&g_disk_book_lock);
    
    stats->total_entries = g_disk_book_capacity;
    stats->used_entries = DiskBookCount(g_disk_book_sb.tail, g_disk_book_sb.head);
    stats->free_entries = g_disk_book_capacity - stats->used_entries - 1;
    stats->transactions = g_disk_book_sb.next_sequence;
    stats->checkpoints = g_disk_book_sb.checkpoint_seq > 0 ? 1 : 0;
    stats->writes_total = g_disk_book_writes_total;
    stats->writes_failed = 0;
    stats->replay_count = g_disk_book_replay_count;
    stats->crc_errors = g_disk_book_crc_errors;
    stats->bytes_logged = g_disk_book_bytes_logged;
    stats->uptime_seconds = (uint32_t)(rtc_get_unix64() - g_disk_book_init_time);
    
    spin_unlock(&g_disk_book_lock);
    return OK;
}

error_t DiskBookPrintStats(void) {
    if (!g_disk_book_initialized)
        return ERR_NOT_INITIALIZED;
    
    DiskBookStats stats;
    DiskBookGetStats(&stats);
    
    debug_printf("\n=== DiskBook Statistics ===\n");
    debug_printf("Total:     %u\n", stats.total_entries);
    debug_printf("Used:      %u\n", stats.used_entries);
    debug_printf("Free:      %u\n", stats.free_entries);
    debug_printf("Writes:    %u\n", stats.writes_total);
    debug_printf("Replay:    %u\n", stats.replay_count);
    debug_printf("CRC errs:  %u\n", stats.crc_errors);
    debug_printf("Bytes:     %lu KB\n", (unsigned long)(stats.bytes_logged / 1024));
    debug_printf("===========================\n");
    
    return OK;
}

bool DiskBookIsInitialized(void) {
    return g_disk_book_initialized;
}

uint32_t DiskBookGetCheckpointSeq(void) {
    return g_disk_book_sb.checkpoint_seq;
}
