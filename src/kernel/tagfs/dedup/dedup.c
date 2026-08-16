#include "dedup.h"
#include "../tagfs.h"
#include "../../lib/kernel/klib.h"
#include "../../lib/kernel/slab.h"
#include "../../../kernel/drivers/timer/rtc.h"
#include "../box_hash/box_hash.h"

// Global state
static DedupState g_dedup_state;

// Compute BoxHash for block (secure mode)
static DedupHash DedupComputeHash(const uint8_t *data, uint32_t size) {
    return BoxHashContent(data, size, &g_dedup_state.hash_ctx);
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

// Block numbers are dense small integers, so the low bits are the whole key.
static uint32_t DedupBlockBucket(uint32_t physical_block) {
    return physical_block & (DEDUP_HASH_BUCKETS - 1);
}

// Compute tag context hash
static uint32_t DedupComputeTagContext(uint32_t tag_context) {
    return tag_context * 0x9e3779b9;
}

static DedupEntry *DedupAllocEntry(void) {
    DedupEntry *e = slab_alloc(sizeof(DedupEntry));
    if (e) memset(e, 0, sizeof(DedupEntry));
    return e;
}

static void DedupFreeEntry(DedupEntry *entry) {
    if (!entry) return;
    slab_free(entry);
}

// ----------------------------------------------------------------------------
// Chain maintenance. Every one of these runs under g_dedup_state.lock, and an
// entry is in both chains or in neither — there is no half-linked state.
// ----------------------------------------------------------------------------

static DedupEntry *DedupFindByBlockLocked(uint32_t physical_block) {
    DedupEntry *e = g_dedup_state.block_table[DedupBlockBucket(physical_block)];
    while (e) {
        if (e->physical_block == physical_block)
            return e;
        e = e->block_next;
    }
    return NULL;
}

static void DedupLinkHashLocked(DedupEntry *entry) {
    uint32_t b = DedupHashBucket(&entry->hash);
    entry->next = g_dedup_state.hash_table[b];
    g_dedup_state.hash_table[b] = entry;
}

static void DedupUnlinkHashLocked(DedupEntry *entry) {
    DedupEntry **pp = &g_dedup_state.hash_table[DedupHashBucket(&entry->hash)];
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->next;
            entry->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void DedupUnlinkBlockLocked(DedupEntry *entry) {
    DedupEntry **pp = &g_dedup_state.block_table[DedupBlockBucket(entry->physical_block)];
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->block_next;
            entry->block_next = NULL;
            return;
        }
        pp = &(*pp)->block_next;
    }
}

// Remove an entry from the index entirely. The single place entries die, so
// the two chains and the two counters cannot drift apart.
static void DedupEvictLocked(DedupEntry *entry) {
    DedupUnlinkHashLocked(entry);
    DedupUnlinkBlockLocked(entry);
    DedupFreeEntry(entry);
    if (g_dedup_state.entry_count > 0)
        g_dedup_state.entry_count--;
    if (g_dedup_state.stats.unique_blocks > 0)
        g_dedup_state.stats.unique_blocks--;
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

    // Deterministic per-volume hash seed (fs_uuid) — content keys are stable
    // across reboots (the old per-boot RTC salt broke cross-mount dedup).
    BoxHashInit(&g_dedup_state.hash_ctx, tagfs_get_state()->superblock.fs_uuid, 16);

    g_dedup_state.hash_buckets = DEDUP_HASH_BUCKETS;
    debug_printf("[Dedup] Allocating hash table (%u buckets)...\n", DEDUP_HASH_BUCKETS);
    g_dedup_state.hash_table = kmalloc(sizeof(DedupEntry*) * DEDUP_HASH_BUCKETS);
    if (!g_dedup_state.hash_table) {
        debug_printf("[Dedup] FAILED: hash table allocation\n");
        return ERR_NO_MEMORY;
    }
    memset(g_dedup_state.hash_table, 0, sizeof(DedupEntry*) * DEDUP_HASH_BUCKETS);

    g_dedup_state.block_table = kmalloc(sizeof(DedupEntry*) * DEDUP_HASH_BUCKETS);
    if (!g_dedup_state.block_table) {
        debug_printf("[Dedup] FAILED: block table allocation\n");
        kfree(g_dedup_state.hash_table);
        g_dedup_state.hash_table = NULL;
        return ERR_NO_MEMORY;
    }
    memset(g_dedup_state.block_table, 0, sizeof(DedupEntry*) * DEDUP_HASH_BUCKETS);

    g_dedup_state.entry_count = 0;
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
    
    for (uint32_t i = 0; i < g_dedup_state.hash_buckets; i++) {
        DedupEntry *e = g_dedup_state.hash_table[i];
        while (e) {
            DedupEntry *next = e->next;
            slab_free(e);
            e = next;
        }
        g_dedup_state.hash_table[i] = NULL;
    }

    if (g_dedup_state.hash_table) {
        kfree(g_dedup_state.hash_table);
        g_dedup_state.hash_table = NULL;
    }

    /* The entries themselves were freed above — walking the hash chains reaches
     * every one of them, because an entry is in both chains or in neither. */
    if (g_dedup_state.block_table) {
        kfree(g_dedup_state.block_table);
        g_dedup_state.block_table = NULL;
    }

    g_dedup_state.entry_count = 0;

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
    
    /* Hashed outside the lock: hash_ctx is written once, before `initialized`
     * goes true, and never again. Holding the one global dedup lock across a
     * 4 KB BoxHash would serialize every core that writes a block. */
    DedupHash hash = DedupComputeHash(block_data, TAGFS_BLOCK_SIZE);
    uint32_t bucket = DedupHashBucket(&hash);

    spin_lock(&g_dedup_state.lock);

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

    DedupHash hash = DedupComputeHash(block_data, TAGFS_BLOCK_SIZE);   /* see Check */
    bool gc_tried = false;

    for (;;) {
        spin_lock(&g_dedup_state.lock);

        /* Already indexed: this is the same block saying what it holds NOW.
         * Re-key it instead of adding a second entry — two entries for one
         * block would leave the older content hash pointing at bytes that no
         * longer match, and a later lookup would hand that block to a file
         * whose content merely used to live there. */
        DedupEntry *entry = DedupFindByBlockLocked(physical_block);
        if (entry) {
            if (!DedupHashEqual(&entry->hash, &hash)) {
                DedupUnlinkHashLocked(entry);
                entry->hash = hash;
                DedupLinkHashLocked(entry);
            }
            entry->tag_context = DedupComputeTagContext(tag_context);
            entry->last_access = rtc_get_unix64();
            spin_unlock(&g_dedup_state.lock);
            return OK;
        }

        DedupEntry *new_entry = DedupAllocEntry();
        if (!new_entry) {
            /* Out of entries. GC takes this same lock, so drop it first — and
             * re-run the whole body afterwards rather than assuming the block
             * is still unindexed, because another core had the lock in
             * between. */
            spin_unlock(&g_dedup_state.lock);
            if (gc_tried)
                return ERR_NO_MEMORY;
            gc_tried = true;
            TagFS_DedupGC();
            continue;
        }

        new_entry->hash = hash;
        new_entry->physical_block = physical_block;
        new_entry->ref_count = 1;
        new_entry->create_time = rtc_get_unix64();
        new_entry->last_access = new_entry->create_time;
        new_entry->tag_context = DedupComputeTagContext(tag_context);
        DedupLinkHashLocked(new_entry);
        new_entry->block_next = g_dedup_state.block_table[DedupBlockBucket(physical_block)];
        g_dedup_state.block_table[DedupBlockBucket(physical_block)] = new_entry;

        g_dedup_state.entry_count++;
        g_dedup_state.stats.unique_blocks++;
        g_dedup_state.stats.total_blocks++;

        spin_unlock(&g_dedup_state.lock);
        return OK;
    }
}

error_t TagFS_DedupAddRef(uint32_t physical_block) {
    if (physical_block == 0)
        return ERR_INVALID_ARGUMENT;

    if (!g_dedup_state.initialized)
        return ERR_NOT_INITIALIZED;

    spin_lock(&g_dedup_state.lock);

    DedupEntry *entry = DedupFindByBlockLocked(physical_block);
    if (!entry) {
        spin_unlock(&g_dedup_state.lock);
        return ERR_OBJECT_NOT_FOUND;
    }

    /* At the cap the caller must allocate its own block instead of sharing:
     * one more reference than the count can hold would be one the release
     * path could never balance, and the block would never come back. */
    if (entry->ref_count >= DEDUP_MAX_REFS) {
        spin_unlock(&g_dedup_state.lock);
        return ERR_QUOTA_EXCEEDED;
    }

    /* The hit itself is already counted by Check, which is what found the
     * block worth sharing; counting it again here would double every saving
     * the stats report. */
    entry->ref_count++;
    entry->last_access = rtc_get_unix64();

    spin_unlock(&g_dedup_state.lock);
    return OK;
}

error_t TagFS_DedupUnregister(uint32_t physical_block, bool *may_reclaim) {
    /* Default the answer to "yes" on every path that does not know better:
     * an index that is absent, off, or ignorant of this block has no claim on
     * it, and a caller that cannot free such a block would leak it. */
    if (may_reclaim)
        *may_reclaim = true;

    if (physical_block == 0)
        return ERR_INVALID_ARGUMENT;

    if (!g_dedup_state.initialized)
        return ERR_NOT_INITIALIZED;

    spin_lock(&g_dedup_state.lock);

    DedupEntry *entry = DedupFindByBlockLocked(physical_block);
    if (!entry) {
        spin_unlock(&g_dedup_state.lock);
        return ERR_OBJECT_NOT_FOUND;
    }

    if (entry->ref_count > 1) {
        entry->ref_count--;
        entry->last_access = rtc_get_unix64();
        if (may_reclaim)
            *may_reclaim = false;   /* another file still reads these bytes */
    } else {
        DedupEvictLocked(entry);
    }

    spin_unlock(&g_dedup_state.lock);
    return OK;
}

error_t TagFS_DedupAllocBlock(const uint8_t *block_data, uint32_t *allocated_block, int *is_duplicate, uint32_t tag_context) {
    if (!block_data || !allocated_block || !is_duplicate)
        return ERR_INVALID_ARGUMENT;
    
    uint32_t existing_block;
    bool dup;
    error_t err = TagFS_DedupCheck(block_data, &existing_block, &dup);
    
    if (err == OK && dup) {
        /* A second file is about to point at a block it did not write. That is
         * the one and only place a new reference is born, so it is the one and
         * only place the count grows. If it cannot grow, do not share: fall
         * through and give this caller a block of its own. */
        if (TagFS_DedupAddRef(existing_block) == OK) {
            *allocated_block = existing_block;
            *is_duplicate = 1;
            return OK;
        }
    }
    
    *is_duplicate = 0;
    /* Deliberately outside the dedup lock. tagfs_alloc_blocks takes the TagFS
     * state lock, and the free path takes the dedup lock while holding it —
     * nesting them the other way round here would close the cycle. */
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
    
    /* What GC may take, and why it is not "ref_count == 0".
     *
     * No entry is ever at zero: it is born at one and evicted at its last
     * release, so the old condition could never fire and this pass could never
     * free anything — dead code standing where the memory bound was supposed
     * to be. What it can honestly take is a SINGLY-referenced entry gone cold:
     * that entry is pure cache, and losing it costs at most a future dedup hit.
     *
     * A shared entry (ref_count > 1) is NOT cache. It is the only record that a
     * block has more than one owner, and the free path reads it to decide
     * whether the block may go back to the allocator. Evict that and the next
     * delete hands away bytes another file is still reading. */
    for (uint32_t i = 0; i < g_dedup_state.hash_buckets; i++) {
        DedupEntry *entry = g_dedup_state.hash_table[i];
        while (entry) {
            DedupEntry *next = entry->next;
            if (entry->ref_count <= 1 && (now - entry->last_access) > threshold) {
                DedupEvictLocked(entry);
                freed++;
            }
            entry = next;
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
