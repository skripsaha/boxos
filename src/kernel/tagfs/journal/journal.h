#ifndef TAGFS_JOURNAL_H
#define TAGFS_JOURNAL_H

#include "ktypes.h"
#include "tagfs.h"
#include "boxos_magic.h"
#include "boxos_limits.h"

#define JOURNAL_VERSION        2

// Compile-time defaults — runtime values come from journal superblock / tagfs superblock
#define JOURNAL_SUPERBLOCK_SECTOR_DEFAULT  2060  // 2059(backup SB) + 1
#define JOURNAL_SUPERBLOCK_BACKUP_DEFAULT  2061  // journal SB + 1
#define JOURNAL_ENTRIES_START_DEFAULT      2062  // journal backup + 1
#define JOURNAL_ENTRY_SECTORS              2     // Structural constant: sizeof(JournalEntry)/512

// Journal entry types
#define JTYPE_METADATA        1
#define JTYPE_SUPERBLOCK      2
#define JTYPE_COMMIT          3   // Commit record — marks transaction as atomically committed

typedef struct __packed {
    uint32_t magic;
    uint32_t version;
    uint32_t start_sector;
    uint32_t entry_count;
    uint32_t head;
    uint32_t tail;
    uint32_t commit_seq;
    uint8_t  reserved[484];
} JournalSuperblock;

STATIC_ASSERT(sizeof(JournalSuperblock) == 512, "JournalSuperblock must be 512 bytes");

typedef struct __packed {
    uint32_t magic;
    uint32_t seq;         // Transaction sequence — all entries in same txn share this
    uint32_t type;        // JTYPE_METADATA, JTYPE_SUPERBLOCK, JTYPE_COMMIT
    uint32_t file_id;
    uint32_t sector;      // Target sector for data entries; entry_count for COMMIT
    uint8_t  committed;   // 1 = entry data is valid (written completely)
    uint8_t  replayed;    // 1 = entry has been applied to target sector
    uint8_t  reserved[2];
    uint8_t  data[1000];  // Payload for data entries; unused for COMMIT
} JournalEntry;

STATIC_ASSERT(sizeof(JournalEntry) == 1024, "JournalEntry must be 1024 bytes (2 sectors)");

// Compound transaction handle — supports multiple entries per transaction
typedef struct {
    uint32_t seq;          // Transaction sequence number
    uint32_t first_idx;    // First entry index in ring buffer
    uint32_t entry_count;  // Number of data entries logged so far
    bool     active;       // Transaction in progress
} JournalTxn;

int journal_init(uint32_t superblock_sector);
int journal_reload(void);
int journal_validate_and_replay(void);

// Compound transaction API
int journal_begin(JournalTxn* txn);
int journal_log_metadata(JournalTxn* txn, uint32_t file_id, uint32_t target_sector, const TagFSMetadata* meta);
int journal_log_superblock(JournalTxn* txn, uint32_t target_sector, const TagFSSuperblock* sb);
int journal_commit(JournalTxn* txn);
void journal_abort(JournalTxn* txn);

#endif
