#include "dedup.h"
#include "../tagfs.h"
#include "../../lib/kernel/klib.h"
#include "../../../kernel/drivers/timer/rtc.h"
#include "../box_hash/box_hash.h"

// Global state
static DedupState g_dedup_state;

// Compute BoxHash for block (secure mode)
static DedupHash DedupComputeHash(const uint8_t *data, uint32_t size) {
    return BoxHashComputeSecure(data, size, &g_dedup_state.hash_ctx);
}

// Compare hashes
static bool DedupHashEqual(const DedupHash *a, const DedupHash *b) {
    return BoxHashEqual(a, b);
}

// Get hash bucket index
static uint32_t DedupHashBucket(const DedupHash *hash) {
    uint32_t h;
    memcpy(&h, hash->bytes, sizeof(uint32_t));
    return h & (DEDUP_HASH_BUCKETS - 1);
}

// Compute tag context hash
static uint32_t DedupComputeTagContext(uint32_t tag_context) {
    return tag_context * 0x9e3779b9;
}

// Allocate entry from pool - O(1) using free list
static DedupEntry* DedupAllocEntry(void) {
    if (g_dedup_state.pool_free == 0)
        return NULL;

    // O(1) allocation from free list
    if (g_dedup_state.free_list) {
        DedupEntry *entry = g_dedup_state.free_list;
        g_dedup_state.free_list = entry->next;
        entry->next = NULL;
        g_dedup_state.pool_free--;
        return entry;
    }

    // Fallback: scan pool if free list is empty (shouldn't happen if pool_free > 0)
    for (uint32_t i = 0; i < g_dedup_state.pool_capacity; i++) {
        if (g_dedup_state.entry_pool[i].physical_block == 0) {
            g_dedup_state.pool_free--;
            return &g_dedup_state.entry_pool[i];
        }
    }
    return NULL;
}

// Free entry to pool - O(1) using free list
static void DedupFreeEntry(DedupEntry *entry) {
    if (!entry)
        return;
    entry->physical_block = 0;
    entry->ref_count = 0;
    entry->next = g_dedup_state.free_list;
    g_dedup_state.free_list = entry;
    g_dedup_state.pool_free++;
}

error_t TagFS_DedupCompress(const uint8_t *in_data, uint16_t in_size, uint8_t *out_data, uint16_t *out_size) {
    if (!in_data || !out_data || !out_size || in_size < 16)
        return ERR_INVALID_ARGUMENT;
    
    uint16_t in_pos = 0, out_pos = 0;
    
    while (in_pos < in_size && out_pos < in_size - 2) {
        uint8_t byte = in_data[in_pos];
        uint16_t run_len = 1;
        
        while (in_pos + run_len < in_size && in_data[in_pos + run_len] == byte && run_len < 255)
            run_len++;
        
        out_data[out_pos++] = byte;
        out_data[out_pos++] = (uint8_t)run_len;
        in_pos += run_len;
    }
    
    if (out_pos < in_size) {
        *out_size = out_pos;
        return OK;
    }
    return ERR_BUFFER_TOO_SMALL;
}

error_t TagFS_DedupDecompress(const uint8_t *in_data, uint16_t in_size, uint8_t *out_data, uint16_t *out_size) {
    if (!in_data || !out_data || !out_size)
        return ERR_INVALID_ARGUMENT;
    
    uint16_t in_pos = 0, out_pos = 0;
    
    while (in_pos < in_size - 1 && out_pos < *out_size) {
        uint8_t byte = in_data[in_pos++];
        uint8_t count = in_data[in_pos++];
        
        for (uint8_t i = 0; i < count && out_pos < *out_size; i++)
            out_data[out_pos++] = byte;
    }
    
    *out_size = out_pos;
    return OK;
}

error_t TagFS_DedupInit(void) {
    if (g_dedup_state.initialized)
        return ERR_ALREADY_INITIALIZED;

    memset(&g_dedup_state, 0, sizeof(DedupState));
    spinlock_init(&g_dedup_state.lock);

    // Initialize unique hash context for this filesystem mount
    BoxHashInit(&g_dedup_state.hash_ctx);

    g_dedup_state.hash_buckets = DEDUP_HASH_BUCKETS;
    debug_printf("[Dedup] Allocating hash table (%u buckets)...\n", DEDUP_HASH_BUCKETS);
    g_dedup_state.hash_table = kmalloc(sizeof(DedupEntry*) * DEDUP_HASH_BUCKETS);
    if (!g_dedup_state.hash_table) {
        debug_printf("[Dedup] FAILED: hash table allocation\n");
        return ERR_NO_MEMORY;
    }
    memset(g_dedup_state.hash_table, 0, sizeof(DedupEntry*) * DEDUP_HASH_BUCKETS);

    g_dedup_state.pool_capacity = DEDUP_MAX_ENTRIES;
    debug_printf("[Dedup] Allocating entry pool (%u entries)...\n", DEDUP_MAX_ENTRIES);
    g_dedup_state.entry_pool = kmalloc(sizeof(DedupEntry) * DEDUP_MAX_ENTRIES);
    if (!g_dedup_state.entry_pool) {
        debug_printf("[Dedup] FAILED: entry pool allocation\n");
        kfree(g_dedup_state.hash_table);
        return ERR_NO_MEMORY;
    }
    memset(g_dedup_state.entry_pool, 0, sizeof(DedupEntry) * DEDUP_MAX_ENTRIES);
    g_dedup_state.pool_free = DEDUP_MAX_ENTRIES;

    // Initialize O(1) free list
    g_dedup_state.free_list = NULL;
    for (uint32_t i = 0; i < DEDUP_MAX_ENTRIES; i++) {
        g_dedup_state.entry_pool[i].next = g_dedup_state.free_list;
        g_dedup_state.free_list = &g_dedup_state.entry_pool[i];
    }

    g_dedup_state.gc_interval_entries = 1000;
    g_dedup_state.magic = DEDUP_MAGIC;
    g_dedup_state.version = DEDUP_VERSION;
    g_dedup_state.initialized = true;

    debug_printf("[Dedup] Initialized: %u buckets, BoxHash 256-bit secure\n", DEDUP_HASH_BUCKETS);
    return OK;
}

void TagFS_DedupShutdown(void) {
    if (!g_dedup_state.initialized)
        return;
    
    spin_lock(&g_dedup_state.lock);
    
    for (uint32_t i = 0; i < DEDUP_HASH_BUCKETS; i++) {
        DedupEntry *entry = g_dedup_state.hash_table[i];
        while (entry) {
            DedupEntry *next = entry->next;
            entry = next;
        }
        g_dedup_state.hash_table[i] = NULL;
    }
    
    if (g_dedup_state.entry_pool) {
        kfree(g_dedup_state.entry_pool);
        g_dedup_state.entry_pool = NULL;
    }
    if (g_dedup_state.hash_table) {
        kfree(g_dedup_state.hash_table);
        g_dedup_state.hash_table = NULL;
    }
    
    g_dedup_state.initialized = false;
    spin_unlock(&g_dedup_state.lock);
    debug_printf("[Dedup] Shutdown complete\n");
}

error_t TagFS_DedupCheck(const uint8_t *block_data, uint32_t *existing_block, bool *is_duplicate) {
    if (!block_data || !existing_block || !is_duplicate)
        return ERR_INVALID_ARGUMENT;
    
    if (!g_dedup_state.initialized) {
        *is_duplicate = false;
        return ERR_NOT_INITIALIZED;
    }
    
    spin_lock(&g_dedup_state.lock);
    
    DedupHash hash = DedupComputeHash(block_data, TAGFS_BLOCK_SIZE);
    uint32_t bucket = DedupHashBucket(&hash);
    
    DedupEntry *entry = g_dedup_state.hash_table[bucket];
    while (entry) {
        if (DedupHashEqual(&entry->hash, &hash)) {
            *existing_block = entry->physical_block;
            *is_duplicate = true;
            g_dedup_state.stats.duplicate_blocks++;
            g_dedup_state.stats.bytes_saved += TAGFS_BLOCK_SIZE;
            entry->last_access = rtc_get_unix64();
            spin_unlock(&g_dedup_state.lock);
            return OK;
        }
        entry = entry->next;
    }
    
    *is_duplicate = false;
    spin_unlock(&g_dedup_state.lock);
    return ERR_OBJECT_NOT_FOUND;
}

error_t TagFS_DedupRegister(uint32_t physical_block, const uint8_t *block_data, uint32_t tag_context) {
    if (!block_data || physical_block == 0)
        return ERR_INVALID_ARGUMENT;
    
    if (!g_dedup_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_dedup_state.lock);
    
    DedupHash hash = DedupComputeHash(block_data, TAGFS_BLOCK_SIZE);
    uint32_t bucket = DedupHashBucket(&hash);
    
    DedupEntry *entry = g_dedup_state.hash_table[bucket];
    while (entry) {
        if (entry->physical_block == physical_block) {
            spin_unlock(&g_dedup_state.lock);
            return OK;
        }
        entry = entry->next;
    }
    
    DedupEntry *new_entry = DedupAllocEntry();
    if (!new_entry) {
        // Pool exhausted — release lock before calling GC (GC acquires its own lock)
        spin_unlock(&g_dedup_state.lock);
        TagFS_DedupGC();
        spin_lock(&g_dedup_state.lock);
        new_entry = DedupAllocEntry();
        if (!new_entry) {
            spin_unlock(&g_dedup_state.lock);
            return ERR_NO_MEMORY;
        }
    }
    
    new_entry->hash = hash;
    new_entry->physical_block = physical_block;
    new_entry->ref_count = 1;
    new_entry->create_time = rtc_get_unix64();
    new_entry->last_access = new_entry->create_time;
    new_entry->tag_context = DedupComputeTagContext(tag_context);
    new_entry->next = g_dedup_state.hash_table[bucket];
    g_dedup_state.hash_table[bucket] = new_entry;
    
    g_dedup_state.entry_count++;
    g_dedup_state.stats.unique_blocks++;
    g_dedup_state.stats.total_blocks++;
    
    spin_unlock(&g_dedup_state.lock);
    return OK;
}

error_t TagFS_DedupUnregister(uint32_t physical_block) {
    if (physical_block == 0)
        return ERR_INVALID_ARGUMENT;
    
    if (!g_dedup_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_dedup_state.lock);
    
    for (uint32_t i = 0; i < DEDUP_HASH_BUCKETS; i++) {
        DedupEntry **pp = &g_dedup_state.hash_table[i];
        while (*pp) {
            DedupEntry *entry = *pp;
            if (entry->physical_block == physical_block) {
                if (entry->ref_count > 1) {
                    entry->ref_count--;
                } else {
                    *pp = entry->next;
                    DedupFreeEntry(entry);
                    g_dedup_state.entry_count--;
                    g_dedup_state.stats.unique_blocks--;
                }
                spin_unlock(&g_dedup_state.lock);
                return OK;
            }
            pp = &entry->next;
        }
    }
    
    spin_unlock(&g_dedup_state.lock);
    return ERR_OBJECT_NOT_FOUND;
}

error_t TagFS_DedupAllocBlock(const uint8_t *block_data, uint32_t *allocated_block, int *is_duplicate, uint32_t tag_context) {
    if (!block_data || !allocated_block || !is_duplicate)
        return ERR_INVALID_ARGUMENT;
    
    uint32_t existing_block;
    bool dup;
    error_t err = TagFS_DedupCheck(block_data, &existing_block, &dup);
    
    if (err == OK && dup) {
        *allocated_block = existing_block;
        *is_duplicate = 1;
        TagFS_DedupRegister(existing_block, block_data, tag_context);
        return OK;
    }
    
    *is_duplicate = 0;
    err = tagfs_alloc_blocks(1, allocated_block);
    if (err != OK)
        return err;
    
    TagFS_DedupRegister(*allocated_block, block_data, tag_context);
    return OK;
}

error_t TagFS_DedupFindByTag(uint32_t tag_context, uint32_t *blocks, uint32_t max_blocks, uint32_t *count) {
    if (!blocks || !count || max_blocks == 0)
        return ERR_INVALID_ARGUMENT;
    
    if (!g_dedup_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_dedup_state.lock);
    
    uint32_t found = 0;
    uint32_t tag_hash = DedupComputeTagContext(tag_context);
    
    for (uint32_t i = 0; i < DEDUP_HASH_BUCKETS && found < max_blocks; i++) {
        DedupEntry *entry = g_dedup_state.hash_table[i];
        while (entry && found < max_blocks) {
            if (entry->tag_context == tag_hash)
                blocks[found++] = entry->physical_block;
            entry = entry->next;
        }
    }
    
    *count = found;
    spin_unlock(&g_dedup_state.lock);
    return OK;
}

error_t TagFS_DedupGetTagStats(uint32_t tag_context, uint64_t *blocks, uint64_t *bytes) {
    if (!blocks || !bytes)
        return ERR_INVALID_ARGUMENT;
    
    if (!g_dedup_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_dedup_state.lock);
    
    uint64_t block_count = 0;
    uint32_t tag_hash = DedupComputeTagContext(tag_context);
    
    for (uint32_t i = 0; i < DEDUP_HASH_BUCKETS; i++) {
        DedupEntry *entry = g_dedup_state.hash_table[i];
        while (entry) {
            if (entry->tag_context == tag_hash)
                block_count++;
            entry = entry->next;
        }
    }
    
    *blocks = block_count;
    *bytes = block_count * TAGFS_BLOCK_SIZE;
    spin_unlock(&g_dedup_state.lock);
    return OK;
}

error_t TagFS_DedupGC(void) {
    if (!g_dedup_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    spin_lock(&g_dedup_state.lock);
    
    uint64_t now = rtc_get_unix64();
    uint32_t freed = 0;
    uint64_t threshold = DEDUP_GC_THRESHOLD_SECS;
    
    for (uint32_t i = 0; i < DEDUP_HASH_BUCKETS; i++) {
        DedupEntry **pp = &g_dedup_state.hash_table[i];
        while (*pp) {
            DedupEntry *entry = *pp;
            if (entry->ref_count == 0 && (now - entry->last_access) > threshold) {
                *pp = entry->next;
                DedupFreeEntry(entry);
                freed++;
                g_dedup_state.entry_count--;
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    
    g_dedup_state.stats.gc_runs++;
    g_dedup_state.stats.entries_freed += freed;
    g_dedup_state.last_gc_time = now;
    
    spin_unlock(&g_dedup_state.lock);
    
    if (freed > 0)
        debug_printf("[Dedup] GC freed %u entries\n", freed);
    
    return OK;
}

error_t TagFS_DedupGetStats(DedupStats *stats) {
    if (!stats)
        return ERR_INVALID_ARGUMENT;
    
    if (!g_dedup_state.initialized) {
        memset(stats, 0, sizeof(DedupStats));
        return ERR_NOT_INITIALIZED;
    }
    
    spin_lock(&g_dedup_state.lock);
    memcpy(stats, &g_dedup_state.stats, sizeof(DedupStats));
    
    if (stats->total_blocks > 0)
        stats->dedup_ratio = (stats->duplicate_blocks * 100) / stats->total_blocks;
    
    spin_unlock(&g_dedup_state.lock);
    return OK;
}

error_t TagFS_DedupPrintStats(void) {
    if (!g_dedup_state.initialized)
        return ERR_NOT_INITIALIZED;
    
    DedupStats stats;
    TagFS_DedupGetStats(&stats);
    
    debug_printf("\n=== Dedup Statistics ===\n");
    debug_printf("Total:     %lu\n", (unsigned long)stats.total_blocks);
    debug_printf("Unique:    %lu\n", (unsigned long)stats.unique_blocks);
    debug_printf("Duplicate: %lu\n", (unsigned long)stats.duplicate_blocks);
    debug_printf("Ratio:     %lu%%\n", (unsigned long)stats.dedup_ratio);
    debug_printf("Saved:     %lu KB\n", (unsigned long)(stats.bytes_saved / 1024));
    debug_printf("GC runs:   %u\n", stats.gc_runs);
    debug_printf("========================\n");
    
    return OK;
}

bool TagFS_DedupIsInitialized(void) {
    return g_dedup_state.initialized;
}

DedupHash TagFS_DedupComputeHash(const uint8_t *data, uint32_t size) {
    return DedupComputeHash(data, size);
}
