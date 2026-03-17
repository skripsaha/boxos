#include "journal.h"
#include "klib.h"
#include "ata.h"

static JournalSuperblock g_journal_sb;
static bool g_journal_initialized = false;
static bool g_txn_active = false;  // Only one transaction at a time
static spinlock_t g_journal_lock;

static uint32_t g_journal_sector;   // Primary journal superblock sector
static uint32_t g_journal_backup;   // Backup journal superblock sector

// ---------------------------------------------------------------------------
// Superblock I/O
// ---------------------------------------------------------------------------

static int journal_write_superblock(void) {
    uint8_t* buffer = kmalloc(ATA_SECTOR_SIZE);
    if (!buffer) return -1;

    memset(buffer, 0, ATA_SECTOR_SIZE);
    memcpy(buffer, &g_journal_sb, sizeof(JournalSuperblock));

    if (ata_write_sectors(1, g_journal_sector, 1, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    if (ata_write_sectors(1, g_journal_backup, 1, buffer) != 0) {
        debug_printf("[Journal] Warning: Backup superblock write failed\n");
    }

    if (ata_flush_cache(1) != 0) {
        debug_printf("[Journal] ERROR: Cache flush failed after superblock write\n");
        kfree(buffer);
        return -1;
    }

    kfree(buffer);
    return 0;
}

static int journal_read_superblock(void) {
    uint8_t* buffer = kmalloc(ATA_SECTOR_SIZE);
    if (!buffer) return -1;

    if (ata_read_sectors(1, g_journal_sector, 1, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    memcpy(&g_journal_sb, buffer, sizeof(JournalSuperblock));

    if (g_journal_sb.magic != JOURNAL_MAGIC) {
        debug_printf("[Journal] Primary corrupted, trying backup...\n");

        if (ata_read_sectors(1, g_journal_backup, 1, buffer) != 0) {
            kfree(buffer);
            return -1;
        }

        memcpy(&g_journal_sb, buffer, sizeof(JournalSuperblock));

        if (g_journal_sb.magic != JOURNAL_MAGIC) {
            debug_printf("[Journal] Both superblocks corrupted\n");
            kfree(buffer);
            return -1;
        }

        if (ata_write_sectors(1, g_journal_sector, 1, buffer) != 0) {
            debug_printf("[Journal] Warning: Primary restore failed\n");
        } else {
            ata_flush_cache(1);
        }
    }

    kfree(buffer);
    return 0;
}

// ---------------------------------------------------------------------------
// Entry I/O
// ---------------------------------------------------------------------------

static int journal_read_entry(uint32_t index, JournalEntry* entry) {
    if (index >= g_journal_sb.entry_count || !entry) return -1;

    uint32_t sector = g_journal_sb.start_sector + (index * JOURNAL_ENTRY_SECTORS);
    uint8_t* buffer = kmalloc(sizeof(JournalEntry));
    if (!buffer) return -1;

    if (ata_read_sectors(1, sector, JOURNAL_ENTRY_SECTORS, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    memcpy(entry, buffer, sizeof(JournalEntry));
    kfree(buffer);
    return 0;
}

static int journal_write_entry(uint32_t index, const JournalEntry* entry) {
    if (index >= g_journal_sb.entry_count || !entry) return -1;

    uint32_t sector = g_journal_sb.start_sector + (index * JOURNAL_ENTRY_SECTORS);
    uint8_t* buffer = kmalloc(sizeof(JournalEntry));
    if (!buffer) return -1;

    memset(buffer, 0, sizeof(JournalEntry));
    memcpy(buffer, entry, sizeof(JournalEntry));

    int result = ata_write_sectors(1, sector, JOURNAL_ENTRY_SECTORS, buffer);
    kfree(buffer);
    return result;
}

static int journal_mark_replayed(uint32_t index) {
    if (index >= g_journal_sb.entry_count) return -1;

    uint32_t sector = g_journal_sb.start_sector + (index * JOURNAL_ENTRY_SECTORS);
    uint32_t replayed_offset = offsetof(JournalEntry, replayed);

    uint8_t* buffer = kmalloc(sizeof(JournalEntry));
    if (!buffer) return -1;

    if (ata_read_sectors(1, sector, JOURNAL_ENTRY_SECTORS, buffer) != 0) {
        kfree(buffer);
        return -1;
    }

    buffer[replayed_offset] = 1;

    int result = ata_write_sectors(1, sector, JOURNAL_ENTRY_SECTORS, buffer);
    kfree(buffer);
    return result;
}

// ---------------------------------------------------------------------------
// Ring buffer helpers
// ---------------------------------------------------------------------------

// Count entries between tail and head in circular ring
static uint32_t ring_count(uint32_t tail, uint32_t head, uint32_t size) {
    if (head >= tail) return head - tail;
    return size - tail + head;
}

// Check if ring has space for n more entries
static bool ring_has_space(uint32_t n) {
    uint32_t used = ring_count(g_journal_sb.tail, g_journal_sb.head, g_journal_sb.entry_count);
    // Reserve 1 slot to distinguish full from empty
    return (used + n) < g_journal_sb.entry_count;
}

// Allocate the next ring slot, advance head
static uint32_t ring_alloc(void) {
    uint32_t idx = g_journal_sb.head;
    g_journal_sb.head = (g_journal_sb.head + 1) % g_journal_sb.entry_count;
    return idx;
}

// ---------------------------------------------------------------------------
// Compaction: reclaim fully-replayed entries
// ---------------------------------------------------------------------------

static int journal_compact(void) {
    uint32_t idx = g_journal_sb.tail;
    while (idx != g_journal_sb.head) {
        JournalEntry entry;
        if (journal_read_entry(idx, &entry) != 0) break;

        // Stop at first non-replayed entry
        if (entry.magic == JOURNAL_ENTRY_MAGIC && entry.committed == 1 && entry.replayed == 0)
            break;

        idx = (idx + 1) % g_journal_sb.entry_count;
    }

    if (idx != g_journal_sb.tail) {
        g_journal_sb.tail = idx;
        return journal_write_superblock();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Init / Reload
// ---------------------------------------------------------------------------

int journal_reload(void) {
    g_journal_initialized = false;
    g_txn_active = false;
    return journal_init(g_journal_sector);
}

int journal_init(uint32_t superblock_sector) {
    if (g_journal_initialized) return 0;

    spinlock_init(&g_journal_lock);
    g_journal_sector = superblock_sector;
    g_journal_backup = superblock_sector + 1;

    debug_printf("[Journal] Initializing (sb_sector=%u)...\n", g_journal_sector);

    if (journal_read_superblock() != 0) {
        debug_printf("[Journal] Creating new journal...\n");

        memset(&g_journal_sb, 0, sizeof(JournalSuperblock));
        g_journal_sb.magic = JOURNAL_MAGIC;
        g_journal_sb.version = JOURNAL_VERSION;
        g_journal_sb.start_sector = g_journal_backup + 1;
        g_journal_sb.entry_count = JOURNAL_ENTRY_COUNT;
        g_journal_sb.head = 0;
        g_journal_sb.tail = 0;
        g_journal_sb.commit_seq = 1;

        if (journal_write_superblock() != 0) {
            debug_printf("[Journal] Failed to write superblock\n");
            return -1;
        }
    }

    debug_printf("[Journal] Initialized (head=%u, tail=%u, seq=%u, entries_start=%u)\n",
                 g_journal_sb.head, g_journal_sb.tail, g_journal_sb.commit_seq,
                 g_journal_sb.start_sector);

    g_journal_initialized = true;
    g_txn_active = false;
    return 0;
}

// ---------------------------------------------------------------------------
// Replay — seq-based compound transaction recovery
//
// Algorithm:
//   1. First pass: collect set of committed transaction seqs
//      (entries with type == JTYPE_COMMIT && committed == 1)
//   2. Second pass: apply all data entries whose seq is in the committed set
//      and whose replayed == 0
//   3. Entries whose seq has no COMMIT record are incomplete transactions
//      — skip them (effectively rolled back)
// ---------------------------------------------------------------------------

int journal_validate_and_replay(void) {
    if (!g_journal_initialized) {
        debug_printf("[Journal] ERROR: Journal not initialized\n");
        return -1;
    }

    debug_printf("[Journal] Validating journal structure...\n");

    // Validate superblock pointers
    if (g_journal_sb.head >= g_journal_sb.entry_count ||
        g_journal_sb.tail >= g_journal_sb.entry_count) {
        debug_printf("[Journal] ERROR: Corrupted superblock (head=%u, tail=%u, max=%u)\n",
                     g_journal_sb.head, g_journal_sb.tail, g_journal_sb.entry_count);
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

    // Each transaction needs at least 2 entries (BEGIN+COMMIT), so max
    // committed transactions = entry_count / 2.  Dynamically allocate.
    uint32_t max_committed = g_journal_sb.entry_count / 2;
    if (max_committed == 0) max_committed = 1;

    uint32_t* committed_seqs = kmalloc(sizeof(uint32_t) * max_committed);
    if (!committed_seqs) {
        debug_printf("[Journal] ERROR: kmalloc failed for committed_seqs (%u)\n", max_committed);
        return -1;
    }
    uint32_t num_committed = 0;

    // --- Pass 1: Find committed transaction sequences ---
    uint32_t index = g_journal_sb.tail;
    while (index != g_journal_sb.head) {
        JournalEntry entry;
        if (journal_read_entry(index, &entry) != 0) {
            debug_printf("[Journal] ERROR: Failed to read entry %u\n", index);
            kfree(committed_seqs);
            return -1;
        }

        if (entry.magic == JOURNAL_ENTRY_MAGIC &&
            entry.type == JTYPE_COMMIT &&
            entry.committed == 1) {
            if (num_committed < max_committed) {
                committed_seqs[num_committed++] = entry.seq;
            }
        }

        index = (index + 1) % g_journal_sb.entry_count;
    }

    if (num_committed == 0) {
        debug_printf("[Journal] No committed transactions found — all incomplete, rolling back\n");
        kfree(committed_seqs);
        g_journal_sb.tail = g_journal_sb.head;
        if (journal_write_superblock() != 0) {
            debug_printf("[Journal] ERROR: Failed to truncate journal\n");
            return -1;
        }
        return 0;
    }

    // --- Pass 2: Apply committed data entries ---
    uint32_t replayed_count = 0;
    uint32_t skipped_count = 0;

    index = g_journal_sb.tail;
    while (index != g_journal_sb.head) {
        JournalEntry entry;
        if (journal_read_entry(index, &entry) != 0) {
            debug_printf("[Journal] ERROR: Failed to read entry %u\n", index);
            kfree(committed_seqs);
            return -1;
        }

        // Skip non-data entries and already-replayed entries
        if (entry.magic != JOURNAL_ENTRY_MAGIC ||
            entry.committed != 1 ||
            entry.replayed == 1 ||
            entry.type == JTYPE_COMMIT) {
            if (entry.magic == JOURNAL_ENTRY_MAGIC && entry.committed == 1 &&
                entry.replayed == 0 && entry.type != JTYPE_COMMIT) {
                // Data entry but not committed txn — check if its seq is committed
                bool seq_committed = false;
                for (uint32_t s = 0; s < num_committed; s++) {
                    if (committed_seqs[s] == entry.seq) {
                        seq_committed = true;
                        break;
                    }
                }
                if (!seq_committed) {
                    debug_printf("[Journal] INFO: Entry %u (seq=%u) — incomplete txn, skipping\n",
                                 index, entry.seq);
                    skipped_count++;
                }
                // If seq_committed, fall through below won't happen because we already
                // checked type != JTYPE_COMMIT; re-check below
            }
            index = (index + 1) % g_journal_sb.entry_count;
            continue;
        }

        // Check if this entry's transaction was committed
        bool seq_committed = false;
        for (uint32_t s = 0; s < num_committed; s++) {
            if (committed_seqs[s] == entry.seq) {
                seq_committed = true;
                break;
            }
        }

        if (!seq_committed) {
            debug_printf("[Journal] INFO: Entry %u (seq=%u) — incomplete txn, skipping\n",
                         index, entry.seq);
            skipped_count++;
            index = (index + 1) % g_journal_sb.entry_count;
            continue;
        }

        // Safety: skip entries targeting sector 0 (bootloader area — invalid target)
        if (entry.sector == 0) {
            debug_printf("[Journal] WARNING: Skipping replay of entry %u with sector=0\n", index);
            journal_mark_replayed(index);
            skipped_count++;
            index = (index + 1) % g_journal_sb.entry_count;
            continue;
        }

        // Apply this entry
        uint8_t* buffer = kmalloc(ATA_SECTOR_SIZE);
        if (!buffer) {
            debug_printf("[Journal] ERROR: Memory allocation failed\n");
            kfree(committed_seqs);
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
            index = (index + 1) % g_journal_sb.entry_count;
            continue;
        }

        if (ata_write_sectors(1, entry.sector, 1, buffer) != 0) {
            debug_printf("[Journal] ERROR: Failed to write data sector %u for entry %u\n",
                         entry.sector, index);
            kfree(buffer);
            kfree(committed_seqs);
            return -1;
        }

        kfree(buffer);

        if (journal_mark_replayed(index) != 0) {
            debug_printf("[Journal] ERROR: Failed to mark entry %u as replayed\n", index);
            kfree(committed_seqs);
            return -1;
        }

        replayed_count++;
        index = (index + 1) % g_journal_sb.entry_count;
    }

    debug_printf("[Journal] Replay complete: %u applied, %u skipped\n",
                 replayed_count, skipped_count);

    if (replayed_count > 0) {
        if (ata_flush_cache(1) != 0) {
            debug_printf("[Journal] ERROR: Batch flush failed after replay\n");
            kfree(committed_seqs);
            return -1;
        }
    }

    // Mark all COMMIT entries as replayed too
    index = g_journal_sb.tail;
    while (index != g_journal_sb.head) {
        JournalEntry entry;
        if (journal_read_entry(index, &entry) == 0 &&
            entry.magic == JOURNAL_ENTRY_MAGIC &&
            entry.type == JTYPE_COMMIT &&
            entry.replayed == 0) {
            journal_mark_replayed(index);
        }
        index = (index + 1) % g_journal_sb.entry_count;
    }

    // Done with committed_seqs
    kfree(committed_seqs);

    // Truncate journal
    g_journal_sb.tail = g_journal_sb.head;
    if (journal_write_superblock() != 0) {
        debug_printf("[Journal] ERROR: Failed to truncate journal after replay\n");
        return -1;
    }

    debug_printf("[Journal] Journal truncated successfully\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Compound Transaction API
// ---------------------------------------------------------------------------

int journal_begin(JournalTxn* txn) {
    if (!g_journal_initialized || !txn) return -1;

    spin_lock(&g_journal_lock);

    if (g_txn_active) {
        debug_printf("[Journal] ERROR: Transaction already in progress\n");
        spin_unlock(&g_journal_lock);
        return -1;
    }

    // Try to compact old entries if ring is getting full
    if (!ring_has_space(4)) {
        journal_compact();
    }

    // Need at least 2 slots (1 data + 1 commit)
    if (!ring_has_space(2)) {
        debug_printf("[Journal] ERROR: Journal full, cannot begin transaction\n");
        spin_unlock(&g_journal_lock);
        return -1;
    }

    txn->seq = g_journal_sb.commit_seq;
    txn->first_idx = g_journal_sb.head;
    txn->entry_count = 0;
    txn->active = true;
    g_txn_active = true;

    spin_unlock(&g_journal_lock);
    return 0;
}

int journal_log_metadata(JournalTxn* txn, uint32_t file_id, uint32_t target_sector,
                         const TagFSMetadata* meta) {
    if (!g_journal_initialized || !txn || !txn->active || !meta) return -1;

    spin_lock(&g_journal_lock);

    // Need space for this entry — try compaction if full
    if (!ring_has_space(1)) {
        journal_compact();
        if (!ring_has_space(1)) {
            debug_printf("[Journal] ERROR: Journal full during log_metadata (even after compact)\n");
            spin_unlock(&g_journal_lock);
            return -1;
        }
    }

    uint32_t idx = ring_alloc();

    JournalEntry entry;
    memset(&entry, 0, sizeof(JournalEntry));
    entry.magic = JOURNAL_ENTRY_MAGIC;
    entry.seq = txn->seq;
    entry.type = JTYPE_METADATA;
    entry.file_id = file_id;
    entry.sector = target_sector;
    entry.committed = 1;
    entry.replayed = 0;
    memcpy(entry.data, meta, sizeof(TagFSMetadata));

    if (journal_write_entry(idx, &entry) != 0) {
        g_journal_sb.head = txn->first_idx;
        spin_unlock(&g_journal_lock);
        return -1;
    }

    txn->entry_count++;
    spin_unlock(&g_journal_lock);
    return 0;
}

int journal_log_superblock(JournalTxn* txn, uint32_t target_sector, const TagFSSuperblock* sb) {
    if (!g_journal_initialized || !txn || !txn->active || !sb) return -1;

    spin_lock(&g_journal_lock);

    if (!ring_has_space(1)) {
        journal_compact();
        if (!ring_has_space(1)) {
            debug_printf("[Journal] ERROR: Journal full during log_superblock (even after compact)\n");
            spin_unlock(&g_journal_lock);
            return -1;
        }
    }

    uint32_t idx = ring_alloc();

    JournalEntry entry;
    memset(&entry, 0, sizeof(JournalEntry));
    entry.magic = JOURNAL_ENTRY_MAGIC;
    entry.seq = txn->seq;
    entry.type = JTYPE_SUPERBLOCK;
    entry.file_id = 0;
    entry.sector = target_sector;
    entry.committed = 1;
    entry.replayed = 0;
    memcpy(entry.data, sb, sizeof(TagFSSuperblock));

    if (journal_write_entry(idx, &entry) != 0) {
        g_journal_sb.head = txn->first_idx;
        spin_unlock(&g_journal_lock);
        return -1;
    }

    txn->entry_count++;
    spin_unlock(&g_journal_lock);
    return 0;
}

int journal_commit(JournalTxn* txn) {
    if (!g_journal_initialized || !txn || !txn->active) return -1;

    spin_lock(&g_journal_lock);

    if (txn->entry_count == 0) {
        debug_printf("[Journal] WARNING: Committing empty transaction\n");
        txn->active = false;
        g_txn_active = false;
        spin_unlock(&g_journal_lock);
        return 0;
    }

    // All error paths jump here to release lock
    int commit_result = -1;

    // --- Step 1: Write COMMIT record ---
    if (!ring_has_space(1)) {
        debug_printf("[Journal] ERROR: No space for commit record\n");
        g_journal_sb.head = txn->first_idx;
        goto commit_done;
    }

    uint32_t commit_idx = ring_alloc();

    JournalEntry commit_entry;
    memset(&commit_entry, 0, sizeof(JournalEntry));
    commit_entry.magic = JOURNAL_ENTRY_MAGIC;
    commit_entry.seq = txn->seq;
    commit_entry.type = JTYPE_COMMIT;
    commit_entry.file_id = 0;
    commit_entry.sector = txn->entry_count;
    commit_entry.committed = 1;
    commit_entry.replayed = 0;

    if (journal_write_entry(commit_idx, &commit_entry) != 0) {
        debug_printf("[Journal] ERROR: Failed to write commit record\n");
        g_journal_sb.head = txn->first_idx;
        goto commit_done;
    }

    // --- Step 2: Flush to ensure commit record is durable ---
    if (ata_flush_cache(1) != 0) {
        debug_printf("[Journal] ERROR: Flush failed after commit record\n");
        goto commit_done;
    }

    // --- Step 3: Apply all data entries to their target sectors ---
    {
        uint32_t idx = txn->first_idx;
        for (uint32_t i = 0; i < txn->entry_count; i++) {
            JournalEntry entry;
            if (journal_read_entry(idx, &entry) != 0) {
                debug_printf("[Journal] ERROR: Failed to read entry %u during apply\n", idx);
                goto commit_done;
            }

            // Safety: skip entries targeting sector 0 (bootloader area)
            if (entry.sector == 0) {
                debug_printf("[Journal] WARNING: Skipping entry %u with sector=0 (invalid target)\n", idx);
                journal_mark_replayed(idx);
                idx = (idx + 1) % g_journal_sb.entry_count;
                continue;
            }

            uint8_t* buffer = kmalloc(ATA_SECTOR_SIZE);
            if (!buffer) goto commit_done;

            memset(buffer, 0, ATA_SECTOR_SIZE);

            if (entry.type == JTYPE_METADATA) {
                memcpy(buffer, entry.data, sizeof(TagFSMetadata));
            } else if (entry.type == JTYPE_SUPERBLOCK) {
                memcpy(buffer, entry.data, sizeof(TagFSSuperblock));
            }

            int result = ata_write_sectors(1, entry.sector, 1, buffer);
            kfree(buffer);

            if (result != 0) {
                debug_printf("[Journal] ERROR: Failed to apply entry %u to sector %u\n",
                             idx, entry.sector);
                goto commit_done;
            }

            journal_mark_replayed(idx);
            idx = (idx + 1) % g_journal_sb.entry_count;
        }
    }

    // Mark commit record as replayed
    journal_mark_replayed(commit_idx);

    // --- Step 4: Flush applied data ---
    ata_flush_cache(1);

    // --- Step 5: Advance tail and sequence, persist superblock ---
    g_journal_sb.tail = (commit_idx + 1) % g_journal_sb.entry_count;
    g_journal_sb.commit_seq++;
    commit_result = journal_write_superblock();

commit_done:
    txn->active = false;
    g_txn_active = false;
    spin_unlock(&g_journal_lock);
    return commit_result;
}

void journal_abort(JournalTxn* txn) {
    if (!g_journal_initialized || !txn || !txn->active) return;

    spin_lock(&g_journal_lock);

    // Rollback head to before this transaction
    g_journal_sb.head = txn->first_idx;
    journal_write_superblock();

    txn->active = false;
    g_txn_active = false;

    spin_unlock(&g_journal_lock);
}
