#ifndef TAGFS_JOURNAL_H
#define TAGFS_JOURNAL_H

#include "ktypes.h"
#include "tagfs.h"
#include "boxos_magic.h"
#include "boxos_limits.h"

#define JOURNAL_VERSION        1

#define JOURNAL_SUPERBLOCK_SECTOR      2059
#define JOURNAL_SUPERBLOCK_BACKUP      2060
#define JOURNAL_ENTRIES_START          2061
#define JOURNAL_ENTRY_SECTORS          2     // Each entry is 1024 bytes (2 sectors)
#define JOURNAL_ENTRIES_END            (JOURNAL_ENTRIES_START + (JOURNAL_ENTRY_COUNT * JOURNAL_ENTRY_SECTORS))

#define JTYPE_METADATA        1
#define JTYPE_SUPERBLOCK      2

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
    uint32_t seq;
    uint32_t type;
    uint32_t file_id;
    uint32_t sector;
    uint8_t  committed;
    uint8_t  replayed;
    uint8_t  reserved[2];
    uint8_t  data[1000];  // Increased to fit TagFSMetadata (512 bytes) and TagFSSuperblock (512 bytes)
} JournalEntry;

STATIC_ASSERT(sizeof(JournalEntry) == 1024, "JournalEntry must be 1024 bytes (2 sectors)");

int journal_init(void);
int journal_reload(void);
int journal_replay(void);
int journal_validate_and_replay(void);
int journal_begin(uint32_t* txn_id);
int journal_log_metadata(uint32_t txn_id, uint32_t file_id, const TagFSMetadata* meta);
int journal_log_superblock(uint32_t txn_id, const TagFSSuperblock* sb);
int journal_commit(uint32_t txn_id);
void journal_abort(uint32_t txn_id);

#endif
