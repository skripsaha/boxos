#include "journal.h"
#include "klib.h"
#include "ata.h"

static JournalSuperblock g_journal_sb;
static bool g_journal_initialized = false;

static int journal_write_superblock(void) {
    uint8_t* buffer = kmalloc(ATA_SECTOR_SIZE);
    if (!buffer) {
        return -1;
    }

    memset(buffer, 0, ATA_SECTOR_SIZE);
    memcpy(buffer, &g_journal_sb, sizeof(JournalSuperblock));

    if (ata_write_sectors(1, JOURNAL_SUPERBLOCK_SECTOR, 1, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    if (ata_write_sectors(1, JOURNAL_SUPERBLOCK_BACKUP, 1, buffer) != 0) {
        debug_printf("[Journal] Warning: Backup superblock write failed\n");
        // primary is written, backup failure is not critical
    }

    // ensure superblock reaches disk before returning success
    if (ata_flush_cache(1) != 0) {
        debug_printf("[Journal] ERROR: Cache flush failed after superblock write\n");
        kfree(buffer);
        return -1;  // Superblock write not durable, cannot guarantee consistency
    }

    kfree(buffer);
    return 0;
}

static int journal_read_superblock(void) {
    uint8_t* buffer = kmalloc(ATA_SECTOR_SIZE);
    if (!buffer) {
        return -1;
    }

    if (ata_read_sectors(1, JOURNAL_SUPERBLOCK_SECTOR, 1, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    memcpy(&g_journal_sb, buffer, sizeof(JournalSuperblock));

    if (g_journal_sb.magic != JOURNAL_MAGIC) {
        debug_printf("[Journal] Primary corrupted, trying backup...\n");

        if (ata_read_sectors(1, JOURNAL_SUPERBLOCK_BACKUP, 1, buffer) != 0) {
            kfree(buffer);
            return -1;
        }

        memcpy(&g_journal_sb, buffer, sizeof(JournalSuperblock));

        if (g_journal_sb.magic != JOURNAL_MAGIC) {
            debug_printf("[Journal] Both superblocks corrupted\n");
            kfree(buffer);
            return -1;
        }

        if (ata_write_sectors(1, JOURNAL_SUPERBLOCK_SECTOR, 1, buffer) != 0) {
            debug_printf("[Journal] Warning: Primary restore failed\n");
        } else {
            if (ata_flush_cache(1) != 0) {
                debug_printf("[Journal] Warning: Flush after restore failed\n");
            }
        }
    }

    kfree(buffer);
    return 0;
}

static int journal_read_entry(uint32_t index, JournalEntry* entry) {
    if (index >= JOURNAL_ENTRY_COUNT || !entry) {
        return -1;
    }

    uint32_t sector = JOURNAL_ENTRIES_START + (index * JOURNAL_ENTRY_SECTORS);
    uint8_t* buffer = kmalloc(sizeof(JournalEntry));
    if (!buffer) {
        return -1;
    }

    if (ata_read_sectors(1, sector, JOURNAL_ENTRY_SECTORS, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    memcpy(entry, buffer, sizeof(JournalEntry));
    kfree(buffer);
    return 0;
}

static int journal_write_entry(uint32_t index, const JournalEntry* entry) {
    if (index >= JOURNAL_ENTRY_COUNT || !entry) {
        return -1;
    }

    uint32_t sector = JOURNAL_ENTRIES_START + (index * JOURNAL_ENTRY_SECTORS);
    uint8_t* buffer = kmalloc(sizeof(JournalEntry));
    if (!buffer) {
        return -1;
    }

    memset(buffer, 0, sizeof(JournalEntry));
    memcpy(buffer, entry, sizeof(JournalEntry));

    int result = ata_write_sectors(1, sector, JOURNAL_ENTRY_SECTORS, buffer);
    kfree(buffer);
    return result;
}

static int journal_commit_entry(uint32_t index) {
    if (index >= JOURNAL_ENTRY_COUNT) {
        return -1;
    }

    uint32_t sector = JOURNAL_ENTRIES_START + (index * JOURNAL_ENTRY_SECTORS);
    uint32_t committed_offset = offsetof(JournalEntry, committed);

    uint8_t* buffer = kmalloc(sizeof(JournalEntry));
    if (!buffer) {
        return -1;
    }

    if (ata_read_sectors(1, sector, JOURNAL_ENTRY_SECTORS, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    buffer[committed_offset] = 1;

    int result = ata_write_sectors(1, sector, JOURNAL_ENTRY_SECTORS, buffer);
    kfree(buffer);

    if (result == 0) {
        ata_flush_cache(1);
    }

    return result;
}

static int journal_mark_replayed(uint32_t index) {
    if (index >= JOURNAL_ENTRY_COUNT) {
        return -1;
    }

    uint32_t sector = JOURNAL_ENTRIES_START + (index * JOURNAL_ENTRY_SECTORS);
    uint32_t replayed_offset = offsetof(JournalEntry, replayed);

    uint8_t* buffer = kmalloc(sizeof(JournalEntry));
    if (!buffer) {
        return -1;
    }

    if (ata_read_sectors(1, sector, JOURNAL_ENTRY_SECTORS, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    buffer[replayed_offset] = 1;

    int result = ata_write_sectors(1, sector, JOURNAL_ENTRY_SECTORS, buffer);
    kfree(buffer);

    if (result == 0) {
        ata_flush_cache(1);
    }

    return result;
}

int journal_reload(void) {
    g_journal_initialized = false;
    return journal_init();
}

int journal_init(void) {
    if (g_journal_initialized) {
        return 0;
    }

    debug_printf("[Journal] Initializing...\n");

    if (journal_read_superblock() != 0) {
        debug_printf("[Journal] Creating new journal...\n");

        memset(&g_journal_sb, 0, sizeof(JournalSuperblock));
        g_journal_sb.magic = JOURNAL_MAGIC;
        g_journal_sb.version = JOURNAL_VERSION;
        g_journal_sb.start_sector = JOURNAL_ENTRIES_START;
        g_journal_sb.entry_count = JOURNAL_ENTRY_COUNT;
        g_journal_sb.head = 0;
        g_journal_sb.tail = 0;
        g_journal_sb.commit_seq = 1;

        if (journal_write_superblock() != 0) {
            debug_printf("[Journal] Failed to write superblock\n");
            return -1;
        }
    }

    debug_printf("[Journal] Initialized (head=%u, tail=%u, seq=%u)\n",
                 g_journal_sb.head, g_journal_sb.tail, g_journal_sb.commit_seq);

    g_journal_initialized = true;
    return 0;
}

int journal_replay(void) {
    if (!g_journal_initialized) {
        return -1;
    }

    debug_printf("[Journal] Replaying transactions...\n");

    uint32_t replayed = 0;
    uint32_t index = g_journal_sb.tail;

    while (index != g_journal_sb.head) {
        JournalEntry entry;
        if (journal_read_entry(index, &entry) != 0) {
            debug_printf("[Journal] Error reading entry %u\n", index);
            return -1;
        }

        if (entry.magic == JOURNAL_ENTRY_MAGIC && entry.committed == 1 && entry.replayed == 0) {
            uint8_t* buffer = kmalloc(ATA_SECTOR_SIZE);
            if (!buffer) {
                return -1;
            }

            memset(buffer, 0, ATA_SECTOR_SIZE);

            if (entry.type == JTYPE_METADATA) {
                memcpy(buffer, entry.data, sizeof(TagFSMetadata));
            } else if (entry.type == JTYPE_SUPERBLOCK) {
                memcpy(buffer, entry.data, sizeof(TagFSSuperblock));
            }

            if (ata_write_sectors(1, entry.sector, 1, buffer) != 0) {
                debug_printf("[Journal] Failed to replay entry %u\n", index);
                kfree(buffer);
                return -1;
            }

            kfree(buffer);

            if (journal_mark_replayed(index) != 0) {
                debug_printf("[Journal] Failed to mark entry %u as replayed\n", index);
                return -1;
            }

            replayed++;
        }

        index = (index + 1) % JOURNAL_ENTRY_COUNT;
    }

    if (replayed > 0) {
        debug_printf("[Journal] Replayed %u transactions\n", replayed);

        g_journal_sb.tail = g_journal_sb.head;
        if (journal_write_superblock() != 0) {
            debug_printf("[Journal] Warning: Failed to truncate journal\n");
        }
    } else {
        debug_printf("[Journal] No transactions to replay\n");
    }

    return 0;
}

int journal_validate_and_replay(void) {
    if (!g_journal_initialized) {
        debug_printf("[Journal] ERROR: Journal not initialized\n");
        return -1;
    }

    debug_printf("[Journal] Validating journal structure...\n");

    if (g_journal_sb.head >= JOURNAL_ENTRY_COUNT || g_journal_sb.tail >= JOURNAL_ENTRY_COUNT) {
        debug_printf("[Journal] ERROR: Corrupted superblock (head=%u, tail=%u, max=%u)\n",
                     g_journal_sb.head, g_journal_sb.tail, JOURNAL_ENTRY_COUNT);
        debug_printf("[Journal] Resetting journal to empty state\n");

        g_journal_sb.head = 0;
        g_journal_sb.tail = 0;
        g_journal_sb.commit_seq = 1;

        if (journal_write_superblock() != 0) {
            debug_printf("[Journal] CRITICAL: Failed to reset journal superblock\n");
            return -1;
        }

        return 0;
    }

    if (g_journal_sb.head == g_journal_sb.tail) {
        debug_printf("[Journal] Empty, no replay needed\n");
        return 0;
    }

    debug_printf("[Journal] Replaying transactions (tail=%u, head=%u)...\n",
                 g_journal_sb.tail, g_journal_sb.head);

    uint32_t replayed_count = 0;
    uint32_t skipped_count = 0;
    uint32_t index = g_journal_sb.tail;

    while (index != g_journal_sb.head) {
        JournalEntry entry;

        if (journal_read_entry(index, &entry) != 0) {
            debug_printf("[Journal] ERROR: Failed to read entry %u\n", index);
            return -1;
        }

        if (entry.magic != JOURNAL_ENTRY_MAGIC) {
            debug_printf("[Journal] WARNING: Corrupted entry at %u (bad magic: 0x%08x)\n",
                         index, entry.magic);
            skipped_count++;
            index = (index + 1) % JOURNAL_ENTRY_COUNT;
            continue;
        }

        if (entry.committed != 1) {
            debug_printf("[Journal] INFO: Uncommitted entry at %u, skipping\n", index);
            skipped_count++;
            index = (index + 1) % JOURNAL_ENTRY_COUNT;
            continue;
        }

        if (entry.replayed == 1) {
            debug_printf("[Journal] INFO: Entry %u already replayed, skipping\n", index);
            skipped_count++;
            index = (index + 1) % JOURNAL_ENTRY_COUNT;
            continue;
        }

        uint8_t* buffer = kmalloc(ATA_SECTOR_SIZE);
        if (!buffer) {
            debug_printf("[Journal] ERROR: Memory allocation failed\n");
            return -1;
        }

        memset(buffer, 0, ATA_SECTOR_SIZE);

        if (entry.type == JTYPE_METADATA) {
            memcpy(buffer, entry.data, sizeof(TagFSMetadata));
        } else if (entry.type == JTYPE_SUPERBLOCK) {
            memcpy(buffer, entry.data, sizeof(TagFSSuperblock));
        } else {
            debug_printf("[Journal] WARNING: Unknown entry type %u at index %u\n",
                         entry.type, index);
            kfree(buffer);
            skipped_count++;
            index = (index + 1) % JOURNAL_ENTRY_COUNT;
            continue;
        }

        if (ata_write_sectors(1, entry.sector, 1, buffer) != 0) {
            debug_printf("[Journal] ERROR: Failed to write data sector %u for entry %u\n",
                         entry.sector, index);
            kfree(buffer);
            return -1;
        }

        kfree(buffer);

        if (ata_flush_cache(1) != 0) {
            debug_printf("[Journal] ERROR: Cache flush failed for entry %u - cannot guarantee durability\n", index);
            return -1;  // CRITICAL: Abort replay to prevent data loss
        }

        if (journal_mark_replayed(index) != 0) {
            debug_printf("[Journal] ERROR: Failed to mark entry %u as replayed\n", index);
            return -1;
        }

        replayed_count++;
        index = (index + 1) % JOURNAL_ENTRY_COUNT;
    }

    debug_printf("[Journal] Replay complete: %u applied, %u skipped\n",
                 replayed_count, skipped_count);

    g_journal_sb.tail = g_journal_sb.head;

    if (journal_write_superblock() != 0) {
        debug_printf("[Journal] ERROR: Failed to truncate journal after replay\n");
        return -1;
    }

    debug_printf("[Journal] Journal truncated successfully\n");
    return 0;
}

int journal_begin(uint32_t* txn_id) {
    if (!g_journal_initialized || !txn_id) {
        return -1;
    }

    uint32_t next_head = (g_journal_sb.head + 1) % JOURNAL_ENTRY_COUNT;
    if (next_head == g_journal_sb.tail) {
        bool all_replayed = true;
        uint32_t idx = g_journal_sb.tail;
        while (idx != g_journal_sb.head) {
            JournalEntry entry;
            if (journal_read_entry(idx, &entry) != 0) {
                all_replayed = false;
                break;
            }
            if (entry.magic == JOURNAL_ENTRY_MAGIC &&
                entry.committed == 1 && entry.replayed == 0) {
                all_replayed = false;
                break;
            }
            idx = (idx + 1) % JOURNAL_ENTRY_COUNT;
        }

        if (all_replayed) {
            debug_printf("[Journal] Compacting: all entries replayed, resetting head=tail=0\n");
            g_journal_sb.head = 0;
            g_journal_sb.tail = 0;
            if (journal_write_superblock() != 0) {
                debug_printf("[Journal] ERROR: Failed to write superblock during compaction\n");
                return -1;
            }
            next_head = 1;
        } else {
            // journal full with unreplayed entries — force replay to free space
            debug_printf("[Journal] Journal full, forcing replay of pending entries...\n");
            if (journal_replay() != 0) {
                debug_printf("[Journal] ERROR: Forced replay failed, journal still full\n");
                return -1;
            }
            // replay succeeded — compact now
            g_journal_sb.head = 0;
            g_journal_sb.tail = 0;
            if (journal_write_superblock() != 0) {
                debug_printf("[Journal] ERROR: Failed to write superblock after forced replay\n");
                return -1;
            }
            next_head = 1;
            debug_printf("[Journal] Forced replay + compaction succeeded\n");
        }
    }

    *txn_id = g_journal_sb.head;
    g_journal_sb.head = next_head;

    JournalEntry entry;
    memset(&entry, 0, sizeof(JournalEntry));
    entry.magic = JOURNAL_ENTRY_MAGIC;
    entry.seq = g_journal_sb.commit_seq;
    entry.committed = 0;

    if (journal_write_entry(*txn_id, &entry) != 0) {
        g_journal_sb.head = *txn_id;
        return -1;
    }

    return 0;
}

int journal_log_metadata(uint32_t txn_id, uint32_t file_id, const TagFSMetadata* meta) {
    if (!g_journal_initialized || !meta || txn_id >= JOURNAL_ENTRY_COUNT) {
        return -1;
    }

    JournalEntry entry;
    if (journal_read_entry(txn_id, &entry) != 0) {
        return -1;
    }

    if (entry.magic != JOURNAL_ENTRY_MAGIC) {
        return -1;
    }

    entry.type = JTYPE_METADATA;
    entry.file_id = file_id;
    entry.sector = TAGFS_METADATA_START + (file_id - 1);
    memcpy(entry.data, meta, sizeof(TagFSMetadata));

    return journal_write_entry(txn_id, &entry);
}

int journal_log_superblock(uint32_t txn_id, const TagFSSuperblock* sb) {
    if (!g_journal_initialized || !sb || txn_id >= JOURNAL_ENTRY_COUNT) {
        return -1;
    }

    JournalEntry entry;
    if (journal_read_entry(txn_id, &entry) != 0) {
        return -1;
    }

    if (entry.magic != JOURNAL_ENTRY_MAGIC) {
        return -1;
    }

    entry.type = JTYPE_SUPERBLOCK;
    entry.file_id = 0;
    entry.sector = TAGFS_SUPERBLOCK_SECTOR;
    memcpy(entry.data, sb, sizeof(TagFSSuperblock));

    return journal_write_entry(txn_id, &entry);
}

int journal_commit(uint32_t txn_id) {
    if (!g_journal_initialized || txn_id >= JOURNAL_ENTRY_COUNT) {
        return -1;
    }

    JournalEntry entry;
    if (journal_read_entry(txn_id, &entry) != 0) {
        return -1;
    }

    if (entry.magic != JOURNAL_ENTRY_MAGIC) {
        return -1;
    }

    if (journal_commit_entry(txn_id) != 0) {
        return -1;
    }

    uint8_t* buffer = kmalloc(ATA_SECTOR_SIZE);
    if (!buffer) {
        return -1;
    }

    memset(buffer, 0, ATA_SECTOR_SIZE);

    if (entry.type == JTYPE_METADATA) {
        memcpy(buffer, entry.data, sizeof(TagFSMetadata));
    } else if (entry.type == JTYPE_SUPERBLOCK) {
        memcpy(buffer, entry.data, sizeof(TagFSSuperblock));
    }

    int result = ata_write_sectors(1, entry.sector, 1, buffer);
    kfree(buffer);

    if (result != 0) {
        return -1;
    }

    if (ata_flush_cache(1) != 0) {
        debug_printf("[Journal] WARNING: Cache flush timeout after data write\n");
        return -1;
    }

    if (journal_mark_replayed(txn_id) != 0) {
        debug_printf("[Journal] Failed to mark entry as replayed\n");
        return -1;
    }

    g_journal_sb.tail = (txn_id + 1) % JOURNAL_ENTRY_COUNT;
    g_journal_sb.commit_seq++;

    return journal_write_superblock();
}

void journal_abort(uint32_t txn_id) {
    if (!g_journal_initialized || txn_id >= JOURNAL_ENTRY_COUNT) {
        return;
    }

    g_journal_sb.head = txn_id;
    journal_write_superblock();
}
