#include "metadata_cache.h"
#include "klib.h"

// Defined in tagfs.c
extern int tagfs_read_metadata(uint32_t file_id, TagFSMetadata* metadata);

static inline uint32_t mcache_hash(uint32_t file_id) {
    return file_id & (MCACHE_HASH_SIZE - 1);
}

int mcache_init(MetadataLRU* cache) {
    if (!cache) return -1;

    cache->nodes = kmalloc(sizeof(MCacheNode) * MCACHE_CAPACITY);
    if (!cache->nodes) return -1;

    memset(cache->nodes, 0, sizeof(MCacheNode) * MCACHE_CAPACITY);
    memset(cache->hash, 0, sizeof(cache->hash));

    cache->lru_head = NULL;
    cache->lru_tail = NULL;
    cache->count = 0;

    // Build free list (reuse lru_next for chaining)
    cache->free_list = &cache->nodes[0];
    for (uint32_t i = 0; i < MCACHE_CAPACITY - 1; i++) {
        cache->nodes[i].lru_next = &cache->nodes[i + 1];
    }
    cache->nodes[MCACHE_CAPACITY - 1].lru_next = NULL;

    return 0;
}

// Remove node from LRU doubly-linked list (does NOT touch hash chain)
static void lru_remove(MetadataLRU* cache, MCacheNode* node) {
    if (node->lru_prev) {
        node->lru_prev->lru_next = node->lru_next;
    } else {
        cache->lru_head = node->lru_next;
    }
    if (node->lru_next) {
        node->lru_next->lru_prev = node->lru_prev;
    } else {
        cache->lru_tail = node->lru_prev;
    }
    node->lru_prev = NULL;
    node->lru_next = NULL;
}

// Insert node at head of LRU list (MRU position)
static void lru_push_front(MetadataLRU* cache, MCacheNode* node) {
    node->lru_prev = NULL;
    node->lru_next = cache->lru_head;
    if (cache->lru_head) {
        cache->lru_head->lru_prev = node;
    }
    cache->lru_head = node;
    if (!cache->lru_tail) {
        cache->lru_tail = node;
    }
}

// Promote node to MRU position
static void lru_promote(MetadataLRU* cache, MCacheNode* node) {
    if (cache->lru_head == node) return;  // already MRU
    lru_remove(cache, node);
    lru_push_front(cache, node);
}

// Find node in hash table by file_id
static MCacheNode* hash_find(MetadataLRU* cache, uint32_t file_id) {
    uint32_t h = mcache_hash(file_id);
    MCacheNode* node = cache->hash[h];
    while (node) {
        if (node->file_id == file_id) return node;
        node = node->hash_next;
    }
    return NULL;
}

// Remove node from its hash chain
static void hash_remove(MetadataLRU* cache, MCacheNode* node) {
    uint32_t h = mcache_hash(node->file_id);
    MCacheNode** pp = &cache->hash[h];
    while (*pp) {
        if (*pp == node) {
            *pp = node->hash_next;
            node->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

// Insert node into hash table
static void hash_insert(MetadataLRU* cache, MCacheNode* node) {
    uint32_t h = mcache_hash(node->file_id);
    node->hash_next = cache->hash[h];
    cache->hash[h] = node;
}

// Get a free node. Evicts LRU if no free nodes available.
static MCacheNode* alloc_node(MetadataLRU* cache) {
    if (cache->free_list) {
        MCacheNode* node = cache->free_list;
        cache->free_list = node->lru_next;
        node->lru_next = NULL;
        node->lru_prev = NULL;
        node->hash_next = NULL;
        node->file_id = 0;
        return node;
    }

    // Evict LRU tail
    MCacheNode* victim = cache->lru_tail;
    if (!victim) return NULL;

    lru_remove(cache, victim);
    hash_remove(cache, victim);
    cache->count--;

    victim->file_id = 0;
    victim->hash_next = NULL;
    return victim;
}

TagFSMetadata* mcache_get(MetadataLRU* cache, uint32_t file_id) {
    if (!cache || file_id == 0) return NULL;

    // Cache hit
    MCacheNode* node = hash_find(cache, file_id);
    if (node) {
        lru_promote(cache, node);
        return &node->metadata;
    }

    // Cache miss — load from disk
    MCacheNode* new_node = alloc_node(cache);
    if (!new_node) return NULL;

    if (tagfs_read_metadata(file_id, &new_node->metadata) != 0) {
        // Return node to free list
        new_node->lru_next = cache->free_list;
        cache->free_list = new_node;
        return NULL;
    }

    new_node->file_id = file_id;
    hash_insert(cache, new_node);
    lru_push_front(cache, new_node);
    cache->count++;

    return &new_node->metadata;
}

void mcache_put(MetadataLRU* cache, uint32_t file_id, const TagFSMetadata* meta) {
    if (!cache || file_id == 0 || !meta) return;

    MCacheNode* node = hash_find(cache, file_id);
    if (node) {
        // Avoid memcpy to self (undefined behavior)
        if (&node->metadata != meta) {
            memcpy(&node->metadata, meta, sizeof(TagFSMetadata));
        }
        lru_promote(cache, node);
        return;
    }

    // New entry
    node = alloc_node(cache);
    if (!node) return;

    node->file_id = file_id;
    memcpy(&node->metadata, meta, sizeof(TagFSMetadata));
    hash_insert(cache, node);
    lru_push_front(cache, node);
    cache->count++;
}

void mcache_invalidate(MetadataLRU* cache, uint32_t file_id) {
    if (!cache || file_id == 0) return;

    MCacheNode* node = hash_find(cache, file_id);
    if (!node) return;

    lru_remove(cache, node);
    hash_remove(cache, node);
    cache->count--;

    // Return to free list
    node->file_id = 0;
    node->lru_prev = NULL;
    node->hash_next = NULL;
    node->lru_next = cache->free_list;
    cache->free_list = node;
}

void mcache_destroy(MetadataLRU* cache) {
    if (!cache) return;
    if (cache->nodes) {
        kfree(cache->nodes);
        cache->nodes = NULL;
    }
    memset(cache->hash, 0, sizeof(cache->hash));
    cache->lru_head = NULL;
    cache->lru_tail = NULL;
    cache->free_list = NULL;
    cache->count = 0;
}
