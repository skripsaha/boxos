#include "tag_registry.h"

static bool g_registry_dirty = false;

/*
 * ‼ THE BLOCKS THIS REGISTRY IS WRITTEN INTO ARE THE ONES IT ALREADY OWNS.
 *
 * The flush used to call tagfs_alloc_blocks for every continuation of every
 * flush, so a registry longer than one block handed itself a fresh block each
 * time it was written and left the previous one marked used with nothing
 * pointing at it — a block gone per sync, for the life of the boot, and the
 * mount's additive repair keeps every one of them. The file table had exactly
 * this defect and was cured exactly this way: keep the chain the load walked,
 * and allocate only when the registry has genuinely outgrown it.
 *
 * It mattered the moment mkfs started chaining the registry instead of dying
 * at 416 tags, because until then no volume was born with a second block.
 */
static uint32_t* g_chain     = NULL;
static uint32_t  g_chain_len = 0;
static uint32_t  g_chain_cap = 0;

static void chain_release(void) {
    if (g_chain) kfree(g_chain);
    g_chain     = NULL;
    g_chain_len = 0;
    g_chain_cap = 0;
}

static int chain_append(uint32_t block) {
    if (g_chain_len == g_chain_cap) {
        uint32_t cap = g_chain_cap ? g_chain_cap * 2 : 8;
        uint32_t* grown = kmalloc(sizeof(uint32_t) * cap);
        if (!grown) return -1;
        if (g_chain && g_chain_len)
            memcpy(grown, g_chain, sizeof(uint32_t) * g_chain_len);
        if (g_chain) kfree(g_chain);
        g_chain     = grown;
        g_chain_cap = cap;
    }
    g_chain[g_chain_len++] = block;
    return 0;
}

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

    entry->flags  = (value != NULL) ? TAGFS_TAG_FLAG_HAS_VALUE : 0;
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

    entry->flags  = (value != NULL) ? TAGFS_TAG_FLAG_HAS_VALUE : 0;
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

    chain_release();

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

// Stamp an entry as a reserved-vocabulary (system) tag. Idempotent OR of the
// SYSTEM bit; deliberately does NOT raise g_registry_dirty — the bit is
// re-derived from TagFsReservedKeys at every mount, so it must never be the
// reason for a disk flush.
void tag_registry_mark_system(TagRegistry* reg, uint16_t tag_id) {
    if (!reg || tag_id == TAGFS_INVALID_TAG_ID) return;
    spin_lock(&reg->lock);
    if (tag_id < reg->next_id && reg->by_id[tag_id])
        reg->by_id[tag_id]->flags |= TAGFS_TAG_FLAG_SYSTEM;
    spin_unlock(&reg->lock);
}

bool tag_registry_is_system(TagRegistry* reg, uint16_t tag_id) {
    if (!reg || tag_id == TAGFS_INVALID_TAG_ID) return false;
    spin_lock(&reg->lock);
    bool is_sys = false;
    if (tag_id < reg->next_id && reg->by_id[tag_id])
        is_sys = (reg->by_id[tag_id]->flags & TAGFS_TAG_FLAG_SYSTEM) != 0;
    spin_unlock(&reg->lock);
    return is_sys;
}

bool tag_registry_is_dirty(void) {
    return g_registry_dirty;
}

/* BSS-resident scratch — TagRegistryBlock is exactly 4 KiB. Held
 * under reg->lock for the duration of any load/flush, so a single
 * shared buffer is mutually exclusive across both callers. Avoids
 * 4 KiB+local-frame stack usage that exceeded the
 * -Wstack-usage=8192 threshold and would compete with nested IRQ
 * frames on per-cpu stacks. */
static TagRegistryBlock g_tag_registry_load_blk;

int tag_registry_flush(TagRegistry* reg) {
    if (!reg) return -1;

    TagFSState* state = tagfs_get_state();
    if (!state) return -1;

    spin_lock(&reg->lock);

    /* The chain the load walked. It is empty only if nothing ever loaded this
     * registry, which a mount does not allow — but the head is the one block
     * whose place the Deed states, so it can always be recovered. The Deed
     * states it in blocks of the whole volume; what the registry reads and
     * writes is a block of the data run. tagfs.c owns that conversion — see
     * data_block_of() there. */
    if (g_chain_len == 0 &&
        chain_append(state->layout.tag_registry_block -
                     state->layout.data_block) != 0) {
        spin_unlock(&reg->lock);
        return -1;
    }

    uint32_t link          = 0;
    uint32_t current_block = g_chain[0];

    /* Shares the BSS scratch slot with tag_registry_load (both hold
     * reg->lock for their full duration → mutually exclusive). Keeps
     * the 4 KiB block off the kernel stack. */
    TagRegistryBlock *blkp = &g_tag_registry_load_blk;
    memset(blkp, 0, sizeof(*blkp));
    blkp->magic       = TAGFS_REGISTRY_MAGIC;
    blkp->next_block  = 0;
    blkp->entry_count = 0;
    blkp->used_bytes  = 0;

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
            if (link + 1 < g_chain_len) {
                new_block = g_chain[link + 1];
            } else {
                if (tagfs_alloc_blocks(1, &new_block) != 0) {
                    spin_unlock(&reg->lock);
                    return -1;
                }
                if (chain_append(new_block) != 0) {
                    spin_unlock(&reg->lock);
                    return -1;
                }
            }

            blkp->next_block = new_block;
            int write_result = tagfs_write_block(current_block, blkp);
            if (write_result != 0) {
                spin_unlock(&reg->lock);
                return -1;
            }

            link++;
            current_block = new_block;
            memset(blkp, 0, sizeof(*blkp));
            blkp->magic      = TAGFS_REGISTRY_MAGIC;
            blkp->next_block = 0;
            blkp->entry_count = 0;
            blkp->used_bytes  = 0;
            data_offset     = 0;
        }

        uint8_t* p = blkp->data + data_offset;

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
        blkp->entry_count++;
        blkp->used_bytes = (uint16_t)data_offset;
    }

    int write_result = tagfs_write_block(current_block, blkp);

    if (write_result == 0) {
        g_registry_dirty = false;
    }

    spin_unlock(&reg->lock);
    return write_result;
}

int tag_registry_load(TagRegistry* reg, uint32_t first_block) {
    if (!reg) return -1;

    /*
     * ‼ THE ONLY BOUND ON THIS WALK IS A TERMINATION PROOF, NOT A LENGTH.
     *
     * The same rule the metadata pool and the file table are already under. A
     * chain of distinct blocks cannot be longer than the data run it lives in,
     * so a walk that takes that many hops has come back to a block it has
     * already been to: the hop count proves termination AND catches the cycle,
     * and no visited-set can refuse one hop sooner.
     *
     * It had neither bound. A next_block pointing outside the volume was
     * followed off the end of it — nothing under tagfs_read_block bounds a
     * block number — and a next_block pointing back into the chain span for
     * ever with reg->lock held, which is a machine that never finishes
     * mounting rather than one that says what is wrong. Both are reachable on
     * a volume whose registry block took a bad sector, and both became
     * reachable in practice the day mkfs started chaining the registry.
     *
     * The registry is NOT read out of layout.tag_registry_blocks for the
     * reason stated over that field in create_tagfs: a number written once
     * when the volume was made is not a length that ordinary use keeps true.
     */
    TagFSState* fs  = tagfs_get_state();
    uint32_t    run = fs ? fs->layout.data_blocks : 0;
    if (run == 0) {
        kprintf("[TagRegistry] this volume states a data run of no blocks at "
                "all, so there is nowhere for a tag registry to be — it is not "
                "mounted\n");
        return -1;
    }

    spin_lock(&reg->lock);
    chain_release();

    uint32_t block_num = first_block;
    uint32_t hops      = 0;

    /* Read at least the first block (block 0 is valid in our layout) */
    while (1) {
        if (block_num >= run) {
            kprintf("[TagRegistry] the chain points at block %u and this "
                    "volume's data run is %u blocks — that is outside the "
                    "volume, so it is not mounted\n", block_num, run);
            spin_unlock(&reg->lock);
            return -1;
        }

        TagRegistryBlock *blkp = &g_tag_registry_load_blk;
        int read_result = tagfs_read_block(block_num, blkp);
        if (read_result != 0) {
            spin_unlock(&reg->lock);
            return -1;
        }

        if (blkp->magic != TAGFS_REGISTRY_MAGIC) {
            spin_unlock(&reg->lock);
            return -1;
        }

        /* Kept so a flush writes back into the blocks this registry already
         * owns instead of taking a fresh one for every continuation. */
        if (chain_append(block_num) != 0) {
            kprintf("[TagRegistry] there is no memory to hold this volume's "
                    "tag registry chain at %u blocks — it is not mounted\n",
                    g_chain_len + 1);
            chain_release();
            spin_unlock(&reg->lock);
            return -1;
        }

        uint32_t offset = 0;
        for (uint16_t e = 0; e < blkp->entry_count; e++) {
            if (offset + sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t)
                    > TAGFS_REGISTRY_DATA_SIZE) {
                break;
            }

            uint8_t* p = blkp->data + offset;

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

        if (blkp->next_block == 0) break;  /* end of chain */

        if (++hops >= run) {
            kprintf("[TagRegistry] this volume's tag registry chain has taken "
                    "%u hops in a data run of %u blocks, so it is going round "
                    "in a circle — it is not mounted\n", hops, run);
            spin_unlock(&reg->lock);
            return -1;
        }
        block_num = blkp->next_block;
    }

    spin_unlock(&reg->lock);
    return 0;
}
