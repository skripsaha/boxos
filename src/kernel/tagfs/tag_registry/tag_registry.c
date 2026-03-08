#include "tag_registry.h"

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

static uint16_t intern_unlocked(TagRegistry* reg, const char* key, const char* value) {
    uint16_t existing = lookup_unlocked(reg, key, value);
    if (existing != TAGFS_INVALID_TAG_ID) {
        return existing;
    }

    if (reg->next_id > TAGFS_MAX_TAG_ID) {
        debug_printf("[TagRegistry] intern: registry full (max %u)\n", TAGFS_MAX_TAG_ID);
        return TAGFS_INVALID_TAG_ID;
    }

    if (reg->next_id >= reg->by_id_capacity) {
        uint32_t new_cap = reg->by_id_capacity * 2;
        TagRegistryEntry** new_by_id = kmalloc(sizeof(TagRegistryEntry*) * new_cap);
        if (!new_by_id) {
            debug_printf("[TagRegistry] intern: by_id grow alloc failed\n");
            return TAGFS_INVALID_TAG_ID;
        }
        memcpy(new_by_id, reg->by_id, sizeof(TagRegistryEntry*) * reg->by_id_capacity);
        memset(new_by_id + reg->by_id_capacity, 0,
               sizeof(TagRegistryEntry*) * (new_cap - reg->by_id_capacity));
        kfree(reg->by_id);
        reg->by_id = new_by_id;
        reg->by_id_capacity = new_cap;
    }

    TagRegistryEntry* entry = kmalloc(sizeof(TagRegistryEntry));
    if (!entry) {
        debug_printf("[TagRegistry] intern: entry alloc failed\n");
        return TAGFS_INVALID_TAG_ID;
    }

    entry->key = copy_string(key);
    if (!entry->key) {
        kfree(entry);
        debug_printf("[TagRegistry] intern: key copy failed\n");
        return TAGFS_INVALID_TAG_ID;
    }

    entry->value = copy_string(value);
    if (value && !entry->value) {
        kfree(entry->key);
        kfree(entry);
        debug_printf("[TagRegistry] intern: value copy failed\n");
        return TAGFS_INVALID_TAG_ID;
    }

    entry->flags  = (value != NULL) ? 1 : 0;
    entry->tag_id = reg->next_id;

    TagRegistryNode* node = kmalloc(sizeof(TagRegistryNode));
    if (!node) {
        kfree(entry->value);
        kfree(entry->key);
        kfree(entry);
        debug_printf("[TagRegistry] intern: node alloc failed\n");
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

int tag_registry_flush(TagRegistry* reg) {
    // TODO: write registry blocks to disk
    (void)reg;
    return 0;
}

int tag_registry_load(TagRegistry* reg, uint32_t first_block) {
    // TODO: read registry blocks from disk and intern all entries
    (void)reg;
    (void)first_block;
    return 0;
}
