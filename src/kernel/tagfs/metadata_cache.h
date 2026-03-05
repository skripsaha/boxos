#ifndef METADATA_CACHE_H
#define METADATA_CACHE_H

#include "tagfs.h"

#define MCACHE_DEFAULT_CAPACITY    128
#define MCACHE_DEFAULT_HASH_SIZE   64

typedef struct MCacheNode {
    TagFSMetadata metadata;
    uint32_t file_id;           // 0 = empty slot
    bool dirty;                 // true = modified since last disk write
    struct MCacheNode* lru_prev;
    struct MCacheNode* lru_next;
    struct MCacheNode* hash_next;
} MCacheNode;

typedef struct MetadataLRU {
    MCacheNode* nodes;                    // Pre-allocated pool
    MCacheNode** hash;                    // Hash buckets (dynamically allocated)
    MCacheNode* lru_head;                 // Most recently used
    MCacheNode* lru_tail;                 // Least recently used
    MCacheNode* free_list;                // Unused nodes
    uint32_t count;
    uint32_t capacity;                    // Node pool size
    uint32_t hash_size;                   // Hash bucket count (power of 2)
    uint32_t dirty_count;                 // Number of dirty entries
} MetadataLRU;

// Initialize cache with configurable capacity and hash size
int mcache_init(MetadataLRU* cache);

// Get metadata for file_id. Loads from disk on cache miss, evicts LRU if full.
// Returns pointer to cached entry (valid until next cache operation).
TagFSMetadata* mcache_get(MetadataLRU* cache, uint32_t file_id);

// Insert or update an entry (promotes to MRU, marks clean)
void mcache_put(MetadataLRU* cache, uint32_t file_id, const TagFSMetadata* meta);

// Mark a cached entry as dirty (modified, needs flush)
void mcache_mark_dirty(MetadataLRU* cache, uint32_t file_id);

// Flush all dirty entries to disk. Returns 0 on success, -1 on error.
int mcache_flush(MetadataLRU* cache);

// Remove a specific entry from cache (flushes if dirty)
void mcache_invalidate(MetadataLRU* cache, uint32_t file_id);

// Free all cache resources (flushes dirty entries first)
void mcache_destroy(MetadataLRU* cache);

#endif // METADATA_CACHE_H
