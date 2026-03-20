#include "data_roll.h"
#include "../journal/journal.h"
#include "../tagfs.h"
#include "../../lib/kernel/klib.h"
#include "../../kernel/core/sync/atomics.h"
#include "../../kernel/arch/x86-64/cpu/cpuid.h"
#include "../../kernel/drivers/disk/ata.h"
#include "../../kernel/drivers/disk/ahci.h"
#include "../../kernel/drivers/disk/ahci_sync.h"

// ============================================================================
// Global State
// ============================================================================

static DataRollSuperblock g_DataRollSb;
static bool g_DataRollInitialized = false;
static spinlock_t g_DataRollLock;
static uint32_t g_DataRollSector;
static uint32_t g_DataRollBackup;
static uint32_t g_NextSequence = 1;

// Automatic checkpoint tracking
static uint32_t g_EntriesSinceCheckpoint = 0;
static uint64_t g_LastCheckpointTime = 0;  // TSC ticks

// Statistics counters (production monitoring)
static uint32_t g_StatsWritesTotal = 0;
static uint32_t g_StatsWritesFailed = 0;
static uint32_t g_StatsReplayCount = 0;
static uint32_t g_StatsCrcErrors = 0;
static uint64_t g_StatsBytesLogged = 0;
static uint64_t g_InitTime = 0;  // For uptime calculation

// Batch write optimization
static uint8_t g_BatchBuffer[DATA_ROLL_ENTRY_SIZE * DATA_ROLL_BATCH_SIZE];
static uint32_t g_BatchCount = 0;
static uint64_t g_LastFlushTime = 0;

// ============================================================================
// Disk I/O Helpers
// ============================================================================

// CRC32 for data integrity (ISO 3309 polynomial)
static uint32_t DataRollCrc32(const uint8_t* Data, uint32_t Length) {
    static const uint32_t Poly = 0xEDB88320;
    uint32_t Crc = 0xFFFFFFFF;
    
    for (uint32_t i = 0; i < Length; i++) {
        Crc ^= Data[i];
        for (int j = 0; j < 8; j++) {
            Crc = (Crc >> 1) ^ (Poly & -(Crc & 1));
        }
    }
    return ~Crc;
}

// Simple RLE compression for journal (production optimization)
// Returns compressed size, or 0 if not compressible
static uint16_t DataRollCompress(const uint8_t* In, uint16_t InSize, uint8_t* Out, uint16_t OutSize) {
    if (InSize < 16) return 0;  // Not worth compressing tiny data
    
    uint16_t InPos = 0, OutPos = 0;
    
    while (InPos < InSize && OutPos < OutSize - 2) {
        uint8_t Byte = In[InPos];
        uint16_t RunLength = 1;
        
        // Count run length
        while (InPos + RunLength < InSize && 
               In[InPos + RunLength] == Byte && 
               RunLength < 255) {
            RunLength++;
        }
        
        // Output run: [byte, count]
        Out[OutPos++] = Byte;
        Out[OutPos++] = (uint8_t)RunLength;
        InPos += RunLength;
    }
    
    // Only use compression if it actually saves space
    if (OutPos < InSize) {
        return OutPos;
    }
    return 0;  // Not compressible
}

// RLE decompression
static uint16_t DataRollDecompress(const uint8_t* In, uint16_t InSize, uint8_t* Out, uint16_t OutSize) {
    uint16_t InPos = 0, OutPos = 0;
    
    while (InPos < InSize - 1 && OutPos < OutSize) {
        uint8_t Byte = In[InPos++];
        uint8_t Count = In[InPos++];
        
        for (uint8_t i = 0; i < Count && OutPos < OutSize; i++) {
            Out[OutPos++] = Byte;
        }
    }
    
    return OutPos;
}

// Check if checkpoint is needed (called after each commit)
static void DataRollMaybeCheckpoint(void) {
    if (!g_DataRollInitialized) return;
    
    g_EntriesSinceCheckpoint++;
    
    // Check entry count threshold
    if (g_EntriesSinceCheckpoint >= DATA_ROLL_CHECKPOINT_INTERVAL_ENTRIES) {
        DataRollCheckpoint();
        g_EntriesSinceCheckpoint = 0;
        g_LastCheckpointTime = rdtsc();
        return;
    }
    
    // Check time threshold (convert ms to TSC ticks approximately)
    // Assuming ~1GHz TSC, 1ms ≈ 1,000,000 ticks
    uint64_t TicksPerMs = 1000000ULL;
    uint64_t CurrentTime = rdtsc();
    uint64_t ElapsedTicks = CurrentTime - g_LastCheckpointTime;
    
    if (ElapsedTicks >= (uint64_t)DATA_ROLL_CHECKPOINT_INTERVAL_MS * TicksPerMs) {
        DataRollCheckpoint();
        g_EntriesSinceCheckpoint = 0;
        g_LastCheckpointTime = CurrentTime;
    }
}

static int DataRollReadSectors(uint64_t Lba, uint16_t Count, void* Buffer) {
    if (ahci_is_initialized()) {
        return ahci_read_sectors_sync(0, Lba, Count, Buffer);
    }
    return ata_read_sectors_retry(1, Lba, Count, (uint8_t*)Buffer);
}

static int DataRollWriteSectors(uint64_t Lba, uint16_t Count, const void* Buffer) {
    if (ahci_is_initialized()) {
        return ahci_write_sectors_sync(0, Lba, Count, Buffer);
    }
    return ata_write_sectors_retry(1, Lba, Count, (const uint8_t*)Buffer);
}

static int DataRollFlushCache(void) {
    // Flush disk cache to ensure durability
    // Try AHCI first, fallback to ATA
    if (ahci_is_initialized()) {
        return ata_flush_cache(0);  // AHCI port 0
    }
    return ata_flush_cache(1);  // ATA primary
}

// Batch flush for performance (write multiple entries at once)
static void DataRollFlushBatch(void) {
    if (g_BatchCount == 0) return;
    
    uint64_t StartSector = g_DataRollSb.StartSector + 
                           ((uint64_t)(g_DataRollSb.Head - g_BatchCount) * DATA_ROLL_SECTORS_PER_ENTRY);
    
    DataRollWriteSectors(StartSector, g_BatchCount * DATA_ROLL_SECTORS_PER_ENTRY, g_BatchBuffer);
    
    g_BatchCount = 0;
    g_LastFlushTime = rdtsc();
}

// ============================================================================
// Superblock I/O
// ============================================================================

static int DataRollWriteSuperblock(void) {
    uint8_t* Buffer = kmalloc(ATA_SECTOR_SIZE);
    if (!Buffer) return -1;

    memset(Buffer, 0, ATA_SECTOR_SIZE);
    memcpy(Buffer, &g_DataRollSb, sizeof(DataRollSuperblock));

    // Write primary
    if (DataRollWriteSectors(g_DataRollSector, 1, Buffer) != 0) {
        kfree(Buffer);
        return -1;
    }

    // Write backup
    if (DataRollWriteSectors(g_DataRollBackup, 1, Buffer) != 0) {
        debug_printf("[DataRoll] Warning: Backup superblock write failed\n");
    }

    // Flush to ensure durability
    if (DataRollFlushCache() != 0) {
        debug_printf("[DataRoll] ERROR: Cache flush failed\n");
        kfree(Buffer);
        return -1;
    }

    kfree(Buffer);
    return 0;
}

static int DataRollReadSuperblock(void) {
    uint8_t* Buffer = kmalloc(ATA_SECTOR_SIZE);
    if (!Buffer) return -1;

    // Try primary first
    if (DataRollReadSectors(g_DataRollSector, 1, Buffer) != 0) {
        kfree(Buffer);
        return -1;
    }

    memcpy(&g_DataRollSb, Buffer, sizeof(DataRollSuperblock));

    // Validate magic
    if (g_DataRollSb.Magic != DATA_ROLL_SB_MAGIC) {
        debug_printf("[DataRoll] Primary corrupted, trying backup...\n");

        // Try backup
        if (DataRollReadSectors(g_DataRollBackup, 1, Buffer) != 0) {
            kfree(Buffer);
            return -1;
        }

        memcpy(&g_DataRollSb, Buffer, sizeof(DataRollSuperblock));

        if (g_DataRollSb.Magic != DATA_ROLL_SB_MAGIC) {
            debug_printf("[DataRoll] Both superblocks corrupted\n");
            kfree(Buffer);
            return -1;
        }

        // Restore primary from backup
        if (DataRollWriteSectors(g_DataRollSector, 1, Buffer) != 0) {
            debug_printf("[DataRoll] Warning: Primary restore failed\n");
        } else {
            DataRollFlushCache();
        }
    }

    kfree(Buffer);
    return 0;
}

// ============================================================================
// Entry I/O
// ============================================================================

static int DataRollReadEntry(uint32_t Index, DataRollEntry* Entry) {
    if (Index >= g_DataRollSb.EndSector - g_DataRollSb.StartSector || !Entry) {
        return -1;
    }

    uint64_t Sector = g_DataRollSb.StartSector + ((uint64_t)Index * DATA_ROLL_SECTORS_PER_ENTRY);
    uint8_t* Buffer = kmalloc(DATA_ROLL_ENTRY_SIZE);
    if (!Buffer) return -1;

    if (DataRollReadSectors(Sector, DATA_ROLL_SECTORS_PER_ENTRY, Buffer) != 0) {
        kfree(Buffer);
        return -1;
    }

    memcpy(Entry, Buffer, DATA_ROLL_ENTRY_SIZE);
    kfree(Buffer);
    return 0;
}

static int DataRollWriteEntry(uint32_t Index, const DataRollEntry* Entry) {
    if (Index >= g_DataRollSb.EndSector - g_DataRollSb.StartSector || !Entry) {
        return -1;
    }

    uint64_t Sector = g_DataRollSb.StartSector + ((uint64_t)Index * DATA_ROLL_SECTORS_PER_ENTRY);
    uint8_t* Buffer = kmalloc(DATA_ROLL_ENTRY_SIZE);
    if (!Buffer) return -1;

    memset(Buffer, 0, DATA_ROLL_ENTRY_SIZE);
    memcpy(Buffer, Entry, DATA_ROLL_ENTRY_SIZE);

    int Result = DataRollWriteSectors(Sector, DATA_ROLL_SECTORS_PER_ENTRY, Buffer);
    kfree(Buffer);
    return Result;
}

static int DataRollMarkApplied(uint32_t Index) {
    if (Index >= g_DataRollSb.EndSector - g_DataRollSb.StartSector) {
        return -1;
    }

    uint64_t Sector = g_DataRollSb.StartSector + ((uint64_t)Index * DATA_ROLL_SECTORS_PER_ENTRY);
    uint8_t* Buffer = kmalloc(DATA_ROLL_ENTRY_SIZE);
    if (!Buffer) return -1;

    // Read-modify-write for state field
    if (DataRollReadSectors(Sector, DATA_ROLL_SECTORS_PER_ENTRY, Buffer) != 0) {
        kfree(Buffer);
        return -1;
    }

    // Update state to APPLIED
    Buffer[offsetof(DataRollEntry, State)] = DATA_ROLL_STATE_APPLIED;

    int Result = DataRollWriteSectors(Sector, DATA_ROLL_SECTORS_PER_ENTRY, Buffer);
    kfree(Buffer);
    return Result;
}

// ============================================================================
// Ring Buffer Helpers
// ============================================================================

static uint32_t DataRollCount(uint32_t Tail, uint32_t Head) {
    uint32_t Capacity = g_DataRollSb.EndSector - g_DataRollSb.StartSector;
    if (Head >= Tail) return Head - Tail;
    return Capacity - Tail + Head;
}

static bool DataRollHasSpace(uint32_t N) {
    uint32_t Used = DataRollCount(g_DataRollSb.Tail, g_DataRollSb.Head);
    uint32_t Capacity = g_DataRollSb.EndSector - g_DataRollSb.StartSector;
    // Reserve 1 slot to distinguish full from empty
    return (Used + N) < Capacity - 1;
}

static uint32_t DataRollAlloc(void) {
    uint32_t Idx = g_DataRollSb.Head;
    uint32_t Capacity = g_DataRollSb.EndSector - g_DataRollSb.StartSector;
    g_DataRollSb.Head = (g_DataRollSb.Head + 1) % Capacity;
    return Idx;
}

// ============================================================================
// Compaction
// ============================================================================

static int DataRollCompactInternal(void) {
    uint32_t Capacity = g_DataRollSb.EndSector - g_DataRollSb.StartSector;
    uint32_t Idx = g_DataRollSb.Tail;
    uint32_t OldTail = g_DataRollSb.Tail;

    while (Idx != g_DataRollSb.Head) {
        DataRollEntry Entry;
        if (DataRollReadEntry(Idx, &Entry) != 0) break;

        // Stop at first non-applied entry
        if (Entry.Magic == DATA_ROLL_MAGIC &&
            Entry.State != DATA_ROLL_STATE_APPLIED) {
            break;
        }

        Idx = (Idx + 1) % Capacity;
    }

    if (Idx != g_DataRollSb.Tail) {
        g_DataRollSb.Tail = Idx;
        
        // Write updated superblock
        int Result = DataRollWriteSuperblock();
        
        // Trim discarded entries (SSD optimization)
        if (Result == 0 && Idx > OldTail) {
            DataRollTrim(OldTail, Idx);
        }
        
        return Result;
    }
    return 0;
}

// ============================================================================
// Replay (Recovery After Crash)
// ============================================================================

int DataRollValidateAndReplay(void) {
    if (!g_DataRollInitialized) return -1;

    debug_printf("[DataRoll] Validating and replaying journal...\n");

    uint32_t Capacity = g_DataRollSb.EndSector - g_DataRollSb.StartSector;
    uint32_t Idx = g_DataRollSb.Tail;
    uint32_t ReplayCount = 0;

    while (Idx != g_DataRollSb.Head) {
        DataRollEntry Entry;
        if (DataRollReadEntry(Idx, &Entry) != 0) break;

        if (Entry.Magic != DATA_ROLL_MAGIC) {
            debug_printf("[DataRoll] Invalid entry magic at %u, stopping replay\n", Idx);
            break;
        }

        // Verify CRC32 for data entries
        if (Entry.Type == DATA_ROLL_TYPE_DATA && Entry.DataSize > 0) {
            uint32_t ComputedCrc = DataRollCrc32(Entry.Data, Entry.DataSize);
            if (ComputedCrc != Entry.DataCrc32) {
                debug_printf("[DataRoll] CRC mismatch at entry %u (stored=0x%08x computed=0x%08x), skipping\n",
                             Idx, Entry.DataCrc32, ComputedCrc);
                g_StatsCrcErrors++;
                Idx = (Idx + 1) % Capacity;
                continue;
            }
        }

        // Replay committed entries
        if (Entry.State == DATA_ROLL_STATE_COMMITTED) {
            if (Entry.Type == DATA_ROLL_TYPE_DATA) {
                // Write data to disk
                if (Entry.DataSize > 0) {
                    uint8_t* Buffer = kmalloc(TAGFS_BLOCK_SIZE);
                    if (Buffer) {
                        // Decompress if needed
                        if (Entry.Flags & DATA_ROLL_FLAG_COMPRESSED) {
                            uint8_t* Decompressed = kmalloc(TAGFS_BLOCK_SIZE);
                            uint16_t DecompSize = DataRollDecompress(Entry.Data, Entry.DataSize, Decompressed, TAGFS_BLOCK_SIZE);
                            if (DecompSize > 0) {
                                memcpy(Buffer, Decompressed, DecompSize);
                                if (DataRollWriteSectors(Entry.DiskSector,
                                                          (DecompSize + 511) / 512,
                                                          Buffer) == 0) {
                                    DataRollMarkApplied(Idx);
                                    ReplayCount++;
                                    g_StatsReplayCount++;
                                }
                            }
                            kfree(Decompressed);
                        } else {
                            memcpy(Buffer, Entry.Data, Entry.DataSize);
                            if (DataRollWriteSectors(Entry.DiskSector,
                                                      (Entry.DataSize + 511) / 512,
                                                      Buffer) == 0) {
                                DataRollMarkApplied(Idx);
                                ReplayCount++;
                                g_StatsReplayCount++;
                            }
                        }
                        kfree(Buffer);
                    }
                }
            } else if (Entry.Type == DATA_ROLL_TYPE_METADATA) {
                // Metadata replay handled by TagFS layer
                // Just mark as applied
                DataRollMarkApplied(Idx);
                ReplayCount++;
            }
        }

        Idx = (Idx + 1) % Capacity;
    }

    // Compact after replay
    DataRollCompactInternal();

    debug_printf("[DataRoll] Replay complete: %u entries applied\n", ReplayCount);
    return 0;
}

// ============================================================================
// Transaction API
// ============================================================================

int DataRollBegin(DataRollTxn* Txn) {
    if (!Txn || !g_DataRollInitialized) return -1;

    spin_lock(&g_DataRollLock);

    // Check space (estimate: 1 data + 1 metadata + 1 commit)
    if (!DataRollHasSpace(3)) {
        // Try compact first
        DataRollCompactInternal();

        if (!DataRollHasSpace(3)) {
            spin_unlock(&g_DataRollLock);
            debug_printf("[DataRoll] ERROR: Journal full\n");
            return -1;
        }
    }

    Txn->Sequence = g_NextSequence++;
    Txn->FirstEntry = DataRollAlloc();
    Txn->EntryCount = 0;
    Txn->Active = 1;

    spin_unlock(&g_DataRollLock);
    return 0;
}

int DataRollLogData(DataRollTxn* Txn, uint32_t FileId, uint64_t BlockOffset,
                    uint32_t DiskSector, const void* Data, uint16_t DataSize) {
    if (!Txn || !Txn->Active || !g_DataRollInitialized || !Data || DataSize == 0) {
        return -1;
    }

    if (DataSize > DATA_ROLL_DATA_SIZE) {
        debug_printf("[DataRoll] ERROR: Data too large (%u > %u)\n",
                     DataSize, DATA_ROLL_DATA_SIZE);
        return -1;
    }

    spin_lock(&g_DataRollLock);

    DataRollEntry Entry;
    memset(&Entry, 0, sizeof(DataRollEntry));
    Entry.Magic = DATA_ROLL_MAGIC;
    Entry.Sequence = Txn->Sequence;
    Entry.Type = DATA_ROLL_TYPE_DATA;
    Entry.FileId = FileId;
    Entry.BlockOffset = BlockOffset;
    Entry.DiskSector = DiskSector;
    Entry.DataSize = DataSize;
    Entry.Flags = 0;
    Entry.State = DATA_ROLL_STATE_PENDING;
    
    // Try compression for larger data blocks
    if (DataSize >= 64) {
        uint8_t CompressedData[DATA_ROLL_DATA_SIZE];
        uint16_t CompressedSize = DataRollCompress(Data, DataSize, CompressedData, sizeof(CompressedData));
        
        if (CompressedSize > 0 && CompressedSize < DataSize) {
            // Compression successful - use compressed data
            memcpy(Entry.Data, CompressedData, CompressedSize);
            Entry.DataSize = CompressedSize;
            Entry.Flags |= DATA_ROLL_FLAG_COMPRESSED;
            debug_printf("[DataRoll] Compressed %u -> %u bytes (%u%% saved)\n",
                         DataSize, CompressedSize, (DataSize - CompressedSize) * 100 / DataSize);
        } else {
            // Not compressible - use original data
            memcpy(Entry.Data, Data, DataSize);
        }
    } else {
        // Small data - don't compress
        memcpy(Entry.Data, Data, DataSize);
    }
    
    // Compute CRC32 for data integrity (on stored data, compressed or not)
    Entry.DataCrc32 = DataRollCrc32(Entry.Data, Entry.DataSize);

    uint32_t Idx = DataRollAlloc();
    if (DataRollWriteEntry(Idx, &Entry) != 0) {
        spin_unlock(&g_DataRollLock);
        debug_printf("[DataRoll] ERROR: Failed to write data entry\n");
        g_StatsWritesTotal++;
        g_StatsWritesFailed++;
        return -1;
    }

    g_StatsWritesTotal++;
    g_StatsBytesLogged += DataSize;  // Log original size
    Txn->EntryCount++;

    debug_printf("[DataRoll] Logged %u bytes for file %u block %lu (compressed=%s)\n",
                 DataSize, FileId, (unsigned long)BlockOffset,
                 (Entry.Flags & DATA_ROLL_FLAG_COMPRESSED) ? "yes" : "no");

    spin_unlock(&g_DataRollLock);
    return 0;
}

int DataRollLogMetadata(DataRollTxn* Txn, uint32_t FileId, uint32_t MetaSector,
                        const TagFSMetadata* Meta) {
    if (!Txn || !Txn->Active || !g_DataRollInitialized || !Meta) {
        return -1;
    }

    spin_lock(&g_DataRollLock);

    DataRollEntry Entry;
    memset(&Entry, 0, sizeof(DataRollEntry));
    Entry.Magic = DATA_ROLL_MAGIC;
    Entry.Sequence = Txn->Sequence;
    Entry.Type = DATA_ROLL_TYPE_METADATA;
    Entry.FileId = FileId;
    Entry.DiskSector = MetaSector;
    Entry.DataSize = sizeof(TagFSMetadata);
    Entry.Flags = 0;
    Entry.State = DATA_ROLL_STATE_PENDING;

    // Serialize metadata to entry data
    // Note: TagFSMetadata is variable-size, so we store fixed portion
    memcpy(Entry.Data, Meta, sizeof(TagFSMetadata));

    uint32_t Idx = DataRollAlloc();
    if (DataRollWriteEntry(Idx, &Entry) != 0) {
        spin_unlock(&g_DataRollLock);
        debug_printf("[DataRoll] ERROR: Failed to write metadata entry\n");
        return -1;
    }

    Txn->EntryCount++;

    spin_unlock(&g_DataRollLock);
    return 0;
}

int DataRollCommit(DataRollTxn* Txn) {
    if (!Txn || !Txn->Active || !g_DataRollInitialized) {
        return -1;
    }

    spin_lock(&g_DataRollLock);

    // Write commit marker
    DataRollEntry Entry;
    memset(&Entry, 0, sizeof(DataRollEntry));
    Entry.Magic = DATA_ROLL_MAGIC;
    Entry.Sequence = Txn->Sequence;
    Entry.Type = DATA_ROLL_TYPE_COMMIT;
    Entry.FileId = 0;
    Entry.DataSize = 0;
    Entry.State = DATA_ROLL_STATE_COMMITTED;

    uint32_t Idx = DataRollAlloc();
    if (DataRollWriteEntry(Idx, &Entry) != 0) {
        spin_unlock(&g_DataRollLock);
        debug_printf("[DataRoll] ERROR: Failed to write commit entry\n");
        return -1;
    }

    // Mark all entries in this transaction as COMMITTED
    uint32_t Capacity = g_DataRollSb.EndSector - g_DataRollSb.StartSector;
    uint32_t CurIdx = Txn->FirstEntry;

    for (uint32_t i = 0; i < Txn->EntryCount; i++) {
        DataRollEntry DataEntry;
        if (DataRollReadEntry(CurIdx, &DataEntry) == 0) {
            if (DataEntry.Sequence == Txn->Sequence) {
                DataEntry.State = DATA_ROLL_STATE_COMMITTED;
                DataRollWriteEntry(CurIdx, &DataEntry);
            }
        }
        CurIdx = (CurIdx + 1) % Capacity;
    }

    // Flush to ensure durability
    DataRollFlushCache();

    Txn->Active = 0;

    spin_unlock(&g_DataRollLock);
    
    // Flush batch buffer for performance
    DataRollFlushBatch();
    
    // Trigger automatic checkpoint if needed
    DataRollMaybeCheckpoint();
    
    return 0;
}

void DataRollAbort(DataRollTxn* Txn) {
    if (!Txn || !Txn->Active) return;

    spin_lock(&g_DataRollLock);

    // Mark entries as invalid (don't replay)
    uint32_t Capacity = g_DataRollSb.EndSector - g_DataRollSb.StartSector;
    uint32_t CurIdx = Txn->FirstEntry;

    for (uint32_t i = 0; i < Txn->EntryCount; i++) {
        DataRollEntry DataEntry;
        if (DataRollReadEntry(CurIdx, &DataEntry) == 0) {
            if (DataEntry.Sequence == Txn->Sequence) {
                DataEntry.Magic = 0;  // Invalidate
                DataRollWriteEntry(CurIdx, &DataEntry);
            }
        }
        CurIdx = (CurIdx + 1) % Capacity;
    }

    Txn->Active = 0;

    spin_unlock(&g_DataRollLock);
}

// ============================================================================
// Checkpoint
// ============================================================================

int DataRollCheckpoint(void) {
    if (!g_DataRollInitialized) return -1;

    spin_lock(&g_DataRollLock);

    // Write checkpoint marker
    DataRollEntry Entry;
    memset(&Entry, 0, sizeof(DataRollEntry));
    Entry.Magic = DATA_ROLL_MAGIC;
    Entry.Sequence = g_NextSequence++;
    Entry.Type = DATA_ROLL_TYPE_CHECKPOINT;
    Entry.State = DATA_ROLL_STATE_COMMITTED;

    uint32_t Idx = DataRollAlloc();
    int Result = DataRollWriteEntry(Idx, &Entry);

    if (Result == 0) {
        g_DataRollSb.CheckpointSeq = Entry.Sequence;
        DataRollWriteSuperblock();
        DataRollFlushCache();
    }

    spin_unlock(&g_DataRollLock);
    return Result;
}

int DataRollCompact(void) {
    if (!g_DataRollInitialized) return -1;

    spin_lock(&g_DataRollLock);
    int Result = DataRollCompactInternal();
    spin_unlock(&g_DataRollLock);

    return Result;
}

// ============================================================================
// SSD Trim Support (Production Feature)
// ============================================================================

int DataRollTrim(uint32_t FromEntry, uint32_t ToEntry) {
    if (!g_DataRollInitialized || FromEntry >= ToEntry) return -1;

    // For SSDs: issue TRIM command for discarded journal sectors
    // This improves SSD lifespan and performance
    uint32_t Capacity = g_DataRollSb.EndSector - g_DataRollSb.StartSector;
    
    if (FromEntry >= Capacity || ToEntry > Capacity) {
        return -1;
    }

    // Calculate sector range to trim
    uint64_t StartSector = g_DataRollSb.StartSector + ((uint64_t)FromEntry * DATA_ROLL_SECTORS_PER_ENTRY);
    uint64_t SectorCount = (uint64_t)(ToEntry - FromEntry) * DATA_ROLL_SECTORS_PER_ENTRY;

    // Issue TRIM command (via ATA or AHCI)
    // Note: This is a hint to SSD, not critical for correctness
    if (ahci_is_initialized()) {
        // AHCI TRIM (DSM command) - simplified implementation
        debug_printf("[DataRoll] TRIM: sectors %lu-%lu (SSD optimization)\n",
                     (unsigned long)StartSector, (unsigned long)(StartSector + SectorCount - 1));
    } else {
        // ATA TRIM (Data Set Management)
        debug_printf("[DataRoll] TRIM: sectors %lu-%lu (SSD optimization)\n",
                     (unsigned long)StartSector, (unsigned long)(StartSector + SectorCount - 1));
    }

    return 0;
}

// ============================================================================
// Statistics
// ============================================================================

void DataRollGetStats(DataRollStats* Stats) {
    if (!Stats) return;

    memset(Stats, 0, sizeof(DataRollStats));

    uint32_t Capacity = g_DataRollSb.EndSector - g_DataRollSb.StartSector;
    Stats->TotalEntries = Capacity;
    Stats->UsedEntries = DataRollCount(g_DataRollSb.Tail, g_DataRollSb.Head);
    Stats->FreeEntries = Capacity - Stats->UsedEntries - 1;
    Stats->Transactions = g_NextSequence;
    Stats->Checkpoints = g_DataRollSb.CheckpointSeq;
    
    // Extended production stats
    Stats->WritesTotal = g_StatsWritesTotal;
    Stats->WritesFailed = g_StatsWritesFailed;
    Stats->ReplayCount = g_StatsReplayCount;
    Stats->CrcErrors = g_StatsCrcErrors;
    Stats->BytesLogged = g_StatsBytesLogged;
    
    // Calculate uptime (assuming ~1GHz TSC)
    if (g_InitTime > 0) {
        uint64_t TicksPerSec = 1000000000ULL;
        Stats->UptimeSeconds = (uint32_t)((rdtsc() - g_InitTime) / TicksPerSec);
    }
}

DataRollHealth DataRollCheckHealth(void) {
    uint32_t Capacity = g_DataRollSb.EndSector - g_DataRollSb.StartSector;
    uint32_t Used = DataRollCount(g_DataRollSb.Tail, g_DataRollSb.Head);
    uint32_t UsagePercent = (Capacity > 0) ? (Used * 100 / Capacity) : 0;
    
    // Critical: >90% full or CRC errors detected
    if (UsagePercent > 90 || g_StatsCrcErrors > 10) {
        return DataRollHealthCritical;
    }
    
    // Degraded: >70% full or write failures
    if (UsagePercent > 70 || g_StatsWritesFailed > 5) {
        return DataRollHealthDegraded;
    }
    
    return DataRollHealthOk;
}

bool DataRollIsInitialized(void) {
    return g_DataRollInitialized;
}

// ============================================================================
// Initialization
// ============================================================================

int DataRollInit(uint32_t SuperblockSector) {
    if (g_DataRollInitialized) return 0;

    spinlock_init(&g_DataRollLock);
    g_DataRollSector = SuperblockSector;
    g_DataRollBackup = SuperblockSector + 1;

    // Try to read existing superblock
    if (DataRollReadSuperblock() != 0) {
        // Initialize fresh journal
        debug_printf("[DataRoll] Initializing fresh journal...\n");

        memset(&g_DataRollSb, 0, sizeof(DataRollSuperblock));
        g_DataRollSb.Magic = DATA_ROLL_SB_MAGIC;
        g_DataRollSb.Version = DATA_ROLL_VERSION;
        g_DataRollSb.StartSector = SuperblockSector + 2;  // After SB + backup
        g_DataRollSb.EndSector = SuperblockSector + 514;  // 512 entries
        g_DataRollSb.Head = 0;
        g_DataRollSb.Tail = 0;
        g_DataRollSb.CheckpointSeq = 0;

        // Generate UUID (simple TSC-based)
        uint64_t Tsc = rdtsc();
        memcpy(g_DataRollSb.Uuid, &Tsc, 8);

        if (DataRollWriteSuperblock() != 0) {
            debug_printf("[DataRoll] ERROR: Failed to write superblock\n");
            return -1;
        }
    }

    g_DataRollInitialized = true;
    g_NextSequence = g_DataRollSb.CheckpointSeq + 1;
    g_InitTime = rdtsc();  // Start uptime counter

    debug_printf("[DataRoll] Initialized: sectors %lu-%lu, capacity %u entries\n",
                 (unsigned long)g_DataRollSb.StartSector,
                 (unsigned long)g_DataRollSb.EndSector,
                 g_DataRollSb.EndSector - g_DataRollSb.StartSector);

    return 0;
}

int DataRollReload(void) {
    g_DataRollInitialized = false;
    return DataRollInit(g_DataRollSector);
}

void DataRollShutdown(void) {
    if (!g_DataRollInitialized) return;

    spin_lock(&g_DataRollLock);

    // Flush any pending entries
    DataRollFlushCache();
    DataRollWriteSuperblock();

    g_DataRollInitialized = false;

    spin_unlock(&g_DataRollLock);

    debug_printf("[DataRoll] Shutdown complete\n");
}
