#include "tag_registry.h"

static bool g_registry_dirty = false;

static uint32_t registry_hash(const char* key, const char* value, uint32_t bucket_count) {
    uint32_t hash = 5381;
    while (*key) {
        hash = ((hash << 5) + hash) + (uint8_t)(*key++);
    }
    if (value) {
        hash = ((hash << 5) + hash) + ':';
        while (*value) {
            hash = ((hash << 5) + hash) + (uint8_t)(*value++);
        }
    }
    return hash % bucket_count;
}

static uint32_t key_hash(const char* key, uint32_t bucket_count) {
    uint32_t hash = 5381;
    while (*key) {
        hash = ((hash << 5) + hash) + (uint8_t)(*key++);
    }
    return hash % bucket_count;
}

static int values_equal(const char* a, const char* b) {
    if (a == NULL && b == NULL) return 1;
    if (a == NULL || b == NULL) return 0;
    return strcmp(a, b) == 0;
}

static char* copy_string(const char* src) {
    if (!src) return NULL;
    size_t len = strlen(src) + 1;
    char* dst = kmalloc(len);
    if (!dst) return NULL;
    memcpy(dst, src, len);
    return dst;
}

static void add_to_key_group(TagKeyGroup* group, uint16_t tag_id) {
    if (group->count >= group->capacity) {
        uint32_t new_cap = group->capacity * 2;
        uint16_t* new_ids = kmalloc(sizeof(uint16_t) * new_cap);
        if (!new_ids) {
            debug_printf("[TagRegistry] add_to_key_group: grow alloc failed\n");
            return;
        }
        memcpy(new_ids, group->tag_ids, sizeof(uint16_t) * group->count);
        kfree(group->tag_ids);
        group->tag_ids = new_ids;
        group->capacity = new_cap;
    }
    group->tag_ids[group->count++] = tag_id;
}

static TagKeyGroup* find_or_create_key_group(TagRegistry* reg, const char* key) {
    uint32_t bucket = key_hash(key, reg->key_bucket_count);
    TagKeyGroup* group = reg->key_buckets[bucket];

    while (group) {
        if (strcmp(group->key, key) == 0) return group;
        group = group->next;
    }

    TagKeyGroup* new_group = kmalloc(sizeof(TagKeyGroup));
    if (!new_group) {
        debug_printf("[TagRegistry] find_or_create_key_group: group alloc failed\n");
        return NULL;
    }

    new_group->key = copy_string(key);
    if (!new_group->key) {
        kfree(new_group);
        debug_printf("[TagRegistry] find_or_create_key_group: key copy failed\n");
        return NULL;
    }

    new_group->tag_ids = kmalloc(sizeof(uint16_t) * 8);
    if (!new_group->tag_ids) {
        kfree(new_group->key);
        kfree(new_group);
        debug_printf("[TagRegistry] find_or_create_key_group: tag_ids alloc failed\n");
        return NULL;
    }

    new_group->count    = 0;
    new_group->capacity = 8;
    new_group->next     = reg->key_buckets[bucket];
    reg->key_buckets[bucket] = new_group;

    return new_group;
}

static uint16_t lookup_unlocked(TagRegistry* reg, const char* key, const char* value) {
    uint32_t bucket = registry_hash(key, value, reg->bucket_count);
    TagRegistryNode* node = reg->buckets[bucket];

    while (node) {
        TagRegistryEntry* entry = reg->by_id[node->tag_id];
        if (strcmp(entry->key, key) == 0 && values_equal(entry->value, value)) {
            return node->tag_id;
        }
        node = node->next;
    }

    return TAGFS_INVALID_TAG_ID;
}

// Grow by_id array to accommodate at least `needed_id`
static int ensure_by_id_capacity(TagRegistry* reg, uint16_t needed_id) {
    while (needed_id >= reg->by_id_capacity) {
        uint32_t new_cap = reg->by_id_capacity * 2;
        TagRegistryEntry** new_by_id = kmalloc(sizeof(TagRegistryEntry*) * new_cap);
        if (!new_by_id) {
            debug_printf("[TagRegistry] ensure_by_id_capacity: alloc failed\n");
            return -1;
        }
        memcpy(new_by_id, reg->by_id, sizeof(TagRegistryEntry*) * reg->by_id_capacity);
        memset(new_by_id + reg->by_id_capacity, 0,
               sizeof(TagRegistryEntry*) * (new_cap - reg->by_id_capacity));
        kfree(reg->by_id);
        reg->by_id = new_by_id;
        reg->by_id_capacity = new_cap;
    }
    return 0;
}

// Insert a tag at a specific ID (used by load to preserve disk IDs)
static uint16_t intern_with_id_unlocked(TagRegistry* reg, uint16_t tag_id,
                                         const char* key, const char* value) {
    // If this exact tag already exists, return existing
    uint16_t existing = lookup_unlocked(reg, key, value);
    if (existing != TAGFS_INVALID_TAG_ID) return existing;

    if (tag_id > TAGFS_MAX_TAG_ID) return TAGFS_INVALID_TAG_ID;

    if (ensure_by_id_capacity(reg, tag_id) != 0) return TAGFS_INVALID_TAG_ID;

    // Slot already occupied — collision (shouldn't happen on clean load)
    if (reg->by_id[tag_id]) {
        debug_printf("[TagRegistry] intern_with_id: slot %u already used\n", tag_id);
        return TAGFS_INVALID_TAG_ID;
    }

    TagRegistryEntry* entry = kmalloc(sizeof(TagRegistryEntry));
    if (!entry) return TAGFS_INVALID_TAG_ID;

    entry->key = copy_string(key);
    if (!entry->key) { kfree(entry); return TAGFS_INVALID_TAG_ID; }

    entry->value = copy_string(value);
    if (value && !entry->value) { kfree(entry->key); kfree(entry); return TAGFS_INVALID_TAG_ID; }

    entry->flags  = (value != NULL) ? 1 : 0;
    entry->tag_id = tag_id;

    TagRegistryNode* node = kmalloc(sizeof(TagRegistryNode));
    if (!node) { kfree(entry->value); kfree(entry->key); kfree(entry); return TAGFS_INVALID_TAG_ID; }

    uint32_t bucket = registry_hash(key, value, reg->bucket_count);
    node->tag_id = tag_id;
    node->next   = reg->buckets[bucket];
    reg->buckets[bucket] = node;

    reg->by_id[tag_id] = entry;

    TagKeyGroup* group = find_or_create_key_group(reg, key);
    if (group) add_to_key_group(group, tag_id);

    // Keep next_id above all known IDs
    if (tag_id >= reg->next_id) reg->next_id = tag_id + 1;
    reg->total_tags++;
    g_registry_dirty = true;

    return tag_id;
}

static uint16_t intern_unlocked(TagRegistry* reg, const char* key, const char* value) {
    uint16_t existing = lookup_unlocked(reg, key, value);
    if (existing != TAGFS_INVALID_TAG_ID) {
        return existing;
    }

    if (reg->next_id > TAGFS_MAX_TAG_ID) {
        debug_printf("[TagRegistry] intern: registry full (max %u)\n", TAGFS_MAX_TAG_ID);
        return TAGFS_INVALID_TAG_ID;
    }

    if (ensure_by_id_capacity(reg, reg->next_id) != 0) return TAGFS_INVALID_TAG_ID;

    TagRegistryEntry* entry = kmalloc(sizeof(TagRegistryEntry));
    if (!entry) {
        debug_printf("[TagRegistry] intern: entry alloc failed\n");
        return TAGFS_INVALID_TAG_ID;
    }

    entry->key = copy_string(key);
    if (!entry->key) {
        kfree(entry);
        return TAGFS_INVALID_TAG_ID;
    }

    entry->value = copy_string(value);
    if (value && !entry->value) {
        kfree(entry->key);
        kfree(entry);
        return TAGFS_INVALID_TAG_ID;
    }

    entry->flags  = (value != NULL) ? 1 : 0;
    entry->tag_id = reg->next_id;

    TagRegistryNode* node = kmalloc(sizeof(TagRegistryNode));
    if (!node) {
        kfree(entry->value);
        kfree(entry->key);
        kfree(entry);
        return TAGFS_INVALID_TAG_ID;
    }

    uint32_t bucket = registry_hash(key, value, reg->bucket_count);

    node->tag_id = entry->tag_id;
    node->next   = reg->buckets[bucket];
    reg->buckets[bucket] = node;

    reg->by_id[entry->tag_id] = entry;

    TagKeyGroup* group = find_or_create_key_group(reg, key);
    if (group) {
        add_to_key_group(group, entry->tag_id);
    }

    uint16_t assigned_id = reg->next_id;
    reg->next_id++;
    reg->total_tags++;
    g_registry_dirty = true;

    debug_printf("[TagRegistry] interned tag_id=%u key='%s' value='%s'\n",
                 assigned_id, key, value ? value : "(null)");

    return assigned_id;
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

int tag_registry_init(TagRegistry* reg) {
    reg->buckets = kmalloc(sizeof(TagRegistryNode*) * TAGFS_REG_BUCKETS);
    if (!reg->buckets) {
        debug_printf("[TagRegistry] init: buckets alloc failed\n");
        return -1;
    }
    memset(reg->buckets, 0, sizeof(TagRegistryNode*) * TAGFS_REG_BUCKETS);

    reg->by_id = kmalloc(sizeof(TagRegistryEntry*) * 64);
    if (!reg->by_id) {
        kfree(reg->buckets);
        debug_printf("[TagRegistry] init: by_id alloc failed\n");
        return -1;
    }
    memset(reg->by_id, 0, sizeof(TagRegistryEntry*) * 64);

    reg->key_buckets = kmalloc(sizeof(TagKeyGroup*) * TAGFS_KEY_BUCKETS);
    if (!reg->key_buckets) {
        kfree(reg->by_id);
        kfree(reg->buckets);
        debug_printf("[TagRegistry] init: key_buckets alloc failed\n");
        return -1;
    }
    memset(reg->key_buckets, 0, sizeof(TagKeyGroup*) * TAGFS_KEY_BUCKETS);

    reg->bucket_count     = TAGFS_REG_BUCKETS;
    reg->key_bucket_count = TAGFS_KEY_BUCKETS;
    reg->by_id_capacity   = 64;
    reg->total_tags       = 0;
    reg->next_id          = 0;

    spinlock_init(&reg->lock);

    debug_printf("[TagRegistry] initialized: %u reg buckets, %u key buckets\n",
                 TAGFS_REG_BUCKETS, TAGFS_KEY_BUCKETS);
    return 0;
}

void tag_registry_destroy(TagRegistry* reg) {
    if (!reg) return;

    for (uint32_t i = 0; i < reg->bucket_count; i++) {
        TagRegistryNode* node = reg->buckets[i];
        while (node) {
            TagRegistryNode* next = node->next;
            kfree(node);
            node = next;
        }
    }
    kfree(reg->buckets);
    reg->buckets = NULL;

    for (uint32_t i = 0; i < reg->next_id; i++) {
        TagRegistryEntry* entry = reg->by_id[i];
        if (entry) {
            kfree(entry->key);
            if (entry->value) kfree(entry->value);
            kfree(entry);
        }
    }
    kfree(reg->by_id);
    reg->by_id = NULL;

    for (uint32_t i = 0; i < reg->key_bucket_count; i++) {
        TagKeyGroup* group = reg->key_buckets[i];
        while (group) {
            TagKeyGroup* next = group->next;
            kfree(group->key);
            kfree(group->tag_ids);
            kfree(group);
            group = next;
        }
    }
    kfree(reg->key_buckets);
    reg->key_buckets = NULL;

    debug_printf("[TagRegistry] destroyed\n");
}

uint16_t tag_registry_intern(TagRegistry* reg, const char* key, const char* value) {
    if (!reg || !key) return TAGFS_INVALID_TAG_ID;
    spin_lock(&reg->lock);
    uint16_t id = intern_unlocked(reg, key, value);
    spin_unlock(&reg->lock);
    return id;
}

uint16_t tag_registry_lookup(TagRegistry* reg, const char* key, const char* value) {
    if (!reg || !key) return TAGFS_INVALID_TAG_ID;
    spin_lock(&reg->lock);
    uint16_t id = lookup_unlocked(reg, key, value);
    spin_unlock(&reg->lock);
    return id;
}

const char* tag_registry_key(TagRegistry* reg, uint16_t tag_id) {
    if (!reg) return NULL;
    spin_lock(&reg->lock);
    const char* result = NULL;
    if (tag_id < reg->next_id && reg->by_id[tag_id]) {
        result = reg->by_id[tag_id]->key;
    }
    spin_unlock(&reg->lock);
    return result;
}

const char* tag_registry_value(TagRegistry* reg, uint16_t tag_id) {
    if (!reg) return NULL;
    spin_lock(&reg->lock);
    const char* result = NULL;
    if (tag_id < reg->next_id && reg->by_id[tag_id]) {
        result = reg->by_id[tag_id]->value;
    }
    spin_unlock(&reg->lock);
    return result;
}

TagKeyGroup* tag_registry_key_group(TagRegistry* reg, const char* key) {
    if (!reg || !key) return NULL;
    spin_lock(&reg->lock);
    uint32_t bucket = key_hash(key, reg->key_bucket_count);
    TagKeyGroup* group = reg->key_buckets[bucket];
    while (group) {
        if (strcmp(group->key, key) == 0) break;
        group = group->next;
    }
    spin_unlock(&reg->lock);
    return group;
}

bool tag_registry_is_dirty(void) {
    return g_registry_dirty;
}

int tag_registry_flush(TagRegistry* reg) {
    if (!reg) return -1;

    TagFSState* state = tagfs_get_state();
    if (!state) return -1;

    uint32_t current_block = state->superblock.tag_registry_block;

    spin_lock(&reg->lock);

    TagRegistryBlock blk;
    memset(&blk, 0, sizeof(blk));
    blk.magic       = TAGFS_REGISTRY_MAGIC;
    blk.next_block  = 0;
    blk.entry_count = 0;
    blk.used_bytes  = 0;

    uint32_t data_offset = 0;

    for (uint32_t i = 0; i < reg->next_id; i++) {
        TagRegistryEntry* entry = reg->by_id[i];
        if (!entry) continue;

        uint8_t  key_len   = (uint8_t)strlen(entry->key);
        uint16_t value_len = entry->value ? (uint16_t)strlen(entry->value) : 0;

        uint32_t record_size = sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint8_t)
                             + sizeof(uint16_t) + key_len + value_len;

        if (data_offset + record_size > TAGFS_REGISTRY_DATA_SIZE) {
            uint32_t new_block;
            int alloc_result = tagfs_alloc_blocks(1, &new_block);
            if (alloc_result != 0) {
                spin_unlock(&reg->lock);
                return -1;
            }

            blk.next_block = new_block;
            int write_result = tagfs_write_block(current_block, &blk);
            if (write_result != 0) {
                spin_unlock(&reg->lock);
                return -1;
            }

            current_block = new_block;
            memset(&blk, 0, sizeof(blk));
            blk.magic      = TAGFS_REGISTRY_MAGIC;
            blk.next_block = 0;
            blk.entry_count = 0;
            blk.used_bytes  = 0;
            data_offset     = 0;
        }

        uint8_t* p = blk.data + data_offset;

        *((uint16_t*)p) = entry->tag_id;
        p += sizeof(uint16_t);

        *p++ = entry->flags;
        *p++ = key_len;

        *((uint16_t*)p) = value_len;
        p += sizeof(uint16_t);

        memcpy(p, entry->key, key_len);
        p += key_len;

        if (value_len > 0) {
            memcpy(p, entry->value, value_len);
        }

        data_offset += record_size;
        blk.entry_count++;
        blk.used_bytes = (uint16_t)data_offset;
    }

    int write_result = tagfs_write_block(current_block, &blk);

    if (write_result == 0) {
        g_registry_dirty = false;
    }

    spin_unlock(&reg->lock);
    return write_result;
}

int tag_registry_load(TagRegistry* reg, uint32_t first_block) {
    if (!reg) return -1;

    spin_lock(&reg->lock);

    uint32_t block_num = first_block;

    /* Read at least the first block (block 0 is valid in our layout) */
    while (1) {
        TagRegistryBlock blk;
        int read_result = tagfs_read_block(block_num, &blk);
        if (read_result != 0) {
            spin_unlock(&reg->lock);
            return -1;
        }

        if (blk.magic != TAGFS_REGISTRY_MAGIC) {
            spin_unlock(&reg->lock);
            return -1;
        }

        uint32_t offset = 0;
        for (uint16_t e = 0; e < blk.entry_count; e++) {
            if (offset + sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t)
                    > TAGFS_REGISTRY_DATA_SIZE) {
                break;
            }

            uint8_t* p = blk.data + offset;

            uint16_t tag_id;
            memcpy(&tag_id, p, sizeof(uint16_t)); p += sizeof(uint16_t);
            uint8_t  flags    = *p++;
            uint8_t  key_len  = *p++;
            uint16_t value_len;
            memcpy(&value_len, p, sizeof(uint16_t)); p += sizeof(uint16_t);

            uint32_t record_size = sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint8_t)
                                 + sizeof(uint16_t) + key_len + value_len;

            if (offset + record_size > TAGFS_REGISTRY_DATA_SIZE) break;

            char key[256];  // uint8_t key_len always fits with null terminator
            memcpy(key, p, key_len);
            key[key_len] = '\0';
            p += key_len;

            char* value_ptr = NULL;
            char value[4096];
            if (value_len > 0) {
                if (value_len >= sizeof(value)) {
                    offset += record_size;
                    continue;
                }
                memcpy(value, p, value_len);
                value[value_len] = '\0';
                value_ptr = value;
            }

            // Use the stored disk tag_id to preserve ID mapping across reboots
            uint16_t new_id = intern_with_id_unlocked(reg, tag_id, key, value_ptr);
            if (new_id != TAGFS_INVALID_TAG_ID && reg->by_id[new_id]) {
                reg->by_id[new_id]->flags = flags;
            }

            offset += record_size;
        }

        block_num = blk.next_block;
        if (block_num == 0) break;  /* end of chain */
    }

    spin_unlock(&reg->lock);
    return 0;
}
