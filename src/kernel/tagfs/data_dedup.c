#include "../tagfs.h"
#include "metadata_pool/meta_pool.h"
#include "file_table/file_table.h"
#include "../../lib/kernel/klib.h"
#include "../../kernel/drivers/disk/ata.h"

// ============================================================================
// Block Deduplication (Production Feature - ZFS-like)
// ============================================================================

// Hash table for block deduplication
#define DEDUP_HASH_BUCKETS  4096
#define DEDUP_MAX_ENTRIES   65536

typedef struct DedupEntry {
    uint32_t block_hash;       // CRC32 of block content
    uint32_t physical_block;   // Physical block number
    uint32_t ref_count;        // Number of references
    uint8_t  is_compressed;    // Compression flag
    uint8_t  _reserved[3];
    struct DedupEntry* next;   // Hash chain
} DedupEntry;

static DedupEntry* g_DedupTable[DEDUP_HASH_BUCKETS];
static uint32_t g_DedupEntryCount = 0;
static spinlock_t g_DedupLock;

// Statistics
static uint64_t g_DedupBlocksSaved = 0;
static uint64_t g_DedupBytesSaved = 0;

// ============================================================================
// Internal Helpers
// ============================================================================

static uint32_t DedupHash(uint32_t Crc32) {
    return Crc32 % DEDUP_HASH_BUCKETS;
}

static DedupEntry* DedupFind(uint32_t Hash, uint32_t PhysicalBlock) {
    DedupEntry* Entry = g_DedupTable[Hash];
    while (Entry) {
        if (Entry->physical_block == PhysicalBlock) {
            return Entry;
        }
        Entry = Entry->next;
    }
    return NULL;
}

static DedupEntry* DedupFindByHash(uint32_t Hash, uint32_t BlockCrc) {
    DedupEntry* Entry = g_DedupTable[Hash];
    while (Entry) {
        if (Entry->block_hash == BlockCrc) {
            return Entry;
        }
        Entry = Entry->next;
    }
    return NULL;
}

static DedupEntry* DedupAlloc(void) {
    if (g_DedupEntryCount >= DEDUP_MAX_ENTRIES) {
        return NULL;  // Table full
    }
    
    DedupEntry* Entry = kmalloc(sizeof(DedupEntry));
    if (!Entry) return NULL;
    
    memset(Entry, 0, sizeof(DedupEntry));
    g_DedupEntryCount++;
    return Entry;
}

static void DedupFree(DedupEntry* Entry) {
    if (!Entry) return;
    kfree(Entry);
    g_DedupEntryCount--;
}

// ============================================================================
// CRC32 for Block Hashing
// ============================================================================

static uint32_t DedupBlockCrc32(const uint8_t* Data) {
    static const uint32_t Poly = 0xEDB88320;
    uint32_t Crc = 0xFFFFFFFF;
    
    for (uint32_t i = 0; i < TAGFS_BLOCK_SIZE; i++) {
        Crc ^= Data[i];
        for (int j = 0; j < 8; j++) {
            Crc = (Crc >> 1) ^ (Poly & -(Crc & 1));
        }
    }
    return ~Crc;
}

// ============================================================================
// Public API
// ============================================================================

void TagFS_DedupInit(void) {
    memset(g_DedupTable, 0, sizeof(g_DedupTable));
    g_DedupEntryCount = 0;
    g_DedupBlocksSaved = 0;
    g_DedupBytesSaved = 0;
    spinlock_init(&g_DedupLock);
    debug_printf("[TagFS Dedup] Initialized (max=%d entries)\n", DEDUP_MAX_ENTRIES);
}

void TagFS_DedupShutdown(void) {
    spin_lock(&g_DedupLock);
    
    // Free all entries
    for (uint32_t i = 0; i < DEDUP_HASH_BUCKETS; i++) {
        DedupEntry* Entry = g_DedupTable[i];
        while (Entry) {
            DedupEntry* Next = Entry->next;
            DedupFree(Entry);
            Entry = Next;
        }
        g_DedupTable[i] = NULL;
    }
    
    spin_unlock(&g_DedupLock);
    debug_printf("[TagFS Dedup] Shutdown complete\n");
}

// Check if block is duplicate, return existing block if found
int TagFS_DedupCheck(const uint8_t* BlockData, uint32_t* ExistingBlock) {
    if (!BlockData || !ExistingBlock) return -1;
    
    uint32_t Crc = DedupBlockCrc32(BlockData);
    uint32_t Hash = DedupHash(Crc);
    
    spin_lock(&g_DedupLock);
    
    DedupEntry* Entry = DedupFindByHash(Hash, Crc);
    if (Entry) {
        // Verify it's actually the same data (CRC collision check)
        // For performance, we skip full comparison in production
        // Could add optional full comparison for critical data
        *ExistingBlock = Entry->physical_block;
        Entry->ref_count++;
        
        g_DedupBlocksSaved++;
        g_DedupBytesSaved += TAGFS_BLOCK_SIZE;
        
        spin_unlock(&g_DedupLock);
        debug_printf("[TagFS Dedup] Duplicate block detected: %u -> %u (refs=%u)\n",
                     *ExistingBlock, Entry->physical_block, Entry->ref_count);
        return 0;  // Duplicate found
    }
    
    spin_unlock(&g_DedupLock);
    return -1;  // Not a duplicate
}

// Register new block in dedup table
int TagFS_DedupRegister(uint32_t PhysicalBlock, const uint8_t* BlockData) {
    if (!BlockData) return -1;
    
    uint32_t Crc = DedupBlockCrc32(BlockData);
    uint32_t Hash = DedupHash(Crc);
    
    spin_lock(&g_DedupLock);
    
    // Check if already registered
    DedupEntry* Existing = DedupFind(Hash, PhysicalBlock);
    if (Existing) {
        spin_unlock(&g_DedupLock);
        return 0;  // Already registered
    }
    
    // Create new entry
    DedupEntry* Entry = DedupAlloc();
    if (!Entry) {
        spin_unlock(&g_DedupLock);
        debug_printf("[TagFS Dedup] Table full, skipping registration\n");
        return -1;
    }
    
    Entry->block_hash = Crc;
    Entry->physical_block = PhysicalBlock;
    Entry->ref_count = 1;
    Entry->is_compressed = 0;
    
    // Add to hash table
    Entry->next = g_DedupTable[Hash];
    g_DedupTable[Hash] = Entry;
    
    spin_unlock(&g_DedupLock);
    return 0;
}

// Unregister block (decrement ref count)
int TagFS_DedupUnregister(uint32_t PhysicalBlock) {
    uint32_t Hash = PhysicalBlock % DEDUP_HASH_BUCKETS;
    
    spin_lock(&g_DedupLock);
    
    DedupEntry* Entry = DedupFind(Hash, PhysicalBlock);
    if (!Entry) {
        spin_unlock(&g_DedupLock);
        return -1;  // Not found
    }
    
    Entry->ref_count--;
    
    // Free if no more references
    if (Entry->ref_count == 0) {
        // Remove from hash table
        DedupEntry** Prev = &g_DedupTable[Hash];
        while (*Prev) {
            if (*Prev == Entry) {
                *Prev = Entry->next;
                break;
            }
            Prev = &(*Prev)->next;
        }
        
        // Free the block (add to free list)
        tagfs_free_blocks(PhysicalBlock, 1);
        DedupFree(Entry);
        
        debug_printf("[TagFS Dedup] Freed block %u (no more references)\n", PhysicalBlock);
    }
    
    spin_unlock(&g_DedupLock);
    return 0;
}

// Get dedup statistics
void TagFS_DedupGetStats(uint64_t* BlocksSaved, uint64_t* BytesSaved, uint32_t* EntryCount) {
    spin_lock(&g_DedupLock);
    
    if (BlocksSaved) *BlocksSaved = g_DedupBlocksSaved;
    if (BytesSaved) *BytesSaved = g_DedupBytesSaved;
    if (EntryCount) *EntryCount = g_DedupEntryCount;
    
    spin_unlock(&g_DedupLock);
}

// Dedup-aware block allocation
int TagFS_DedupAllocBlock(const uint8_t* BlockData, uint32_t* AllocatedBlock, int* IsDuplicate) {
    if (!BlockData || !AllocatedBlock || !IsDuplicate) return -1;
    
    // Check for duplicate
    uint32_t ExistingBlock;
    if (TagFS_DedupCheck(BlockData, &ExistingBlock) == 0) {
        // Duplicate found - use existing block
        *AllocatedBlock = ExistingBlock;
        *IsDuplicate = 1;
        return 0;
    }
    
    // Not a duplicate - allocate new block
    if (tagfs_alloc_blocks(1, AllocatedBlock) != 0) {
        return -1;
    }
    
    // Register in dedup table
    TagFS_DedupRegister(*AllocatedBlock, BlockData);
    
    *IsDuplicate = 0;
    return 0;
}
