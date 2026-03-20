#ifndef DATA_ROLL_H
#define DATA_ROLL_H

#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"
#include "../tagfs.h"
#include "../../include/boxos_magic.h"

// ============================================================================
// DataRoll — Production Journaling for TagFS Data Blocks
// Unique to BoxOS: Snowball Pipeline-integrated write-ahead logging
// ============================================================================

#define DATA_ROLL_MAGIC           0x44524F4C  // 'DROL'
#define DATA_ROLL_SB_MAGIC        0x44525342  // 'DRSB'
#define DATA_ROLL_VERSION         1
#define DATA_ROLL_ENTRY_SIZE      1024        // 2 sectors (aligned)
#define DATA_ROLL_DATA_SIZE       988         // Payload within entry (was 992, -4 for CRC)
#define DATA_ROLL_SECTORS_PER_ENTRY 2

// Journal entry types (Snowball Deck opcodes)
#define DATA_ROLL_TYPE_DATA       0x01  // Data block payload
#define DATA_ROLL_TYPE_METADATA   0x02  // Metadata update
#define DATA_ROLL_TYPE_COMMIT     0x03  // Transaction commit marker
#define DATA_ROLL_TYPE_CHECKPOINT 0x04  // Checkpoint marker

// Compression flags (stored in Flags field)
#define DATA_ROLL_FLAG_COMPRESSED 0x01  // Data is RLE compressed
#define DATA_ROLL_FLAG_ZERO_FILL  0x02  // Data is all zeros (no storage needed)

// Entry states (stored in State field)
#define DATA_ROLL_STATE_PENDING   0x00  // Logged, not committed
#define DATA_ROLL_STATE_COMMITTED 0x01  // Committed, not applied
#define DATA_ROLL_STATE_APPLIED   0x02  // Applied to disk

// ============================================================================
// On-Disk Structures
// ============================================================================

typedef struct __packed {
    uint32_t Magic;          // 4 bytes
    uint32_t Version;        // 4 bytes
    uint64_t StartSector;    // 8 bytes
    uint64_t EndSector;      // 8 bytes
    uint32_t Head;           // 4 bytes (producer)
    uint32_t Tail;           // 4 bytes (consumer)
    uint32_t CheckpointSeq;  // 4 bytes
    uint32_t Flags;          // 4 bytes
    uint8_t  Uuid[16];       // 16 bytes
    uint8_t  Reserved[456];  // 456 bytes (was 448)
} DataRollSuperblock;

STATIC_ASSERT(sizeof(DataRollSuperblock) == 512, "DataRollSuperblock must be 512 bytes");

typedef struct __packed {
    uint32_t Magic;
    uint32_t Sequence;
    uint32_t Type;
    uint32_t FileId;
    uint64_t BlockOffset;
    uint32_t DiskSector;
    uint16_t DataSize;
    uint8_t  Flags;
    uint8_t  State;
    uint32_t DataCrc32;      // CRC32 of Data[] for integrity check
    uint8_t  Data[DATA_ROLL_DATA_SIZE];
} DataRollEntry;

STATIC_ASSERT(sizeof(DataRollEntry) == 1024, "DataRollEntry must be 1024 bytes");

// ============================================================================
// Transaction Handle
// ============================================================================

typedef struct {
    uint32_t Sequence;
    uint32_t FirstEntry;
    uint32_t EntryCount;
    uint8_t  Active;
    uint8_t  _pad[3];
} DataRollTxn;

// ============================================================================
// Checkpoint Configuration (Production Tuning)
// ============================================================================

#define DATA_ROLL_CHECKPOINT_INTERVAL_ENTRIES  256  // Checkpoint every N entries
#define DATA_ROLL_CHECKPOINT_INTERVAL_MS       5000  // Or every 5 seconds

// Performance tuning
#define DATA_ROLL_BATCH_SIZE                   16    // Max entries per batch write
#define DATA_ROLL_ASYNC_FLUSH_MS               100   // Async flush interval

// ============================================================================
// Public API
// ============================================================================

// Initialization
int DataRollInit(uint32_t SuperblockSector);
int DataRollReload(void);
int DataRollValidateAndReplay(void);
void DataRollShutdown(void);

// Transaction API (Snowball Pipeline integrated)
int DataRollBegin(DataRollTxn* Txn);
int DataRollLogData(DataRollTxn* Txn, uint32_t FileId, uint64_t BlockOffset,
                    uint32_t DiskSector, const void* Data, uint16_t DataSize);
int DataRollLogMetadata(DataRollTxn* Txn, uint32_t FileId, uint32_t MetaSector,
                        const TagFSMetadata* Meta);
int DataRollCommit(DataRollTxn* Txn);
void DataRollAbort(DataRollTxn* Txn);

// Checkpointing (reclaim space)
int DataRollCheckpoint(void);
int DataRollCompact(void);

// SSD Optimization: Trim discarded journal entries
int DataRollTrim(uint32_t FromEntry, uint32_t ToEntry);

// ============================================================================
// Statistics and Monitoring (Production Observability)
// ============================================================================

typedef struct {
    uint32_t TotalEntries;
    uint32_t UsedEntries;
    uint32_t FreeEntries;
    uint32_t Transactions;
    uint32_t Checkpoints;
    // Extended stats for production monitoring
    uint32_t WritesTotal;
    uint32_t WritesFailed;
    uint32_t ReplayCount;
    uint32_t CrcErrors;
    uint64_t BytesLogged;
    uint32_t UptimeSeconds;
} DataRollStats;

void DataRollGetStats(DataRollStats* Stats);

// Health check for production monitoring
typedef enum {
    DataRollHealthOk = 0,
    DataRollHealthDegraded = 1,  // High usage or errors
    DataRollHealthCritical = 2   // Near full or failing
} DataRollHealth;

DataRollHealth DataRollCheckHealth(void);

// Internal state access (for TagFS integration)
bool DataRollIsInitialized(void);

#endif // DATA_ROLL_H
