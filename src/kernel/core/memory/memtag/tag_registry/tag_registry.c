/*
 * MemTag — Tag Registry Implementation
 *
 * Mirrors src/kernel/tagfs/tag_registry/tag_registry.c. Differences:
 *   - No persistence (no flush/load, no g_registry_dirty)
 *   - Generation counter (every intern bumps it)
 *   - Ref-count on entries (region_count)
 *   - Kernel-reserved flag
 *   - InternStr / LookupStr split "key:value" on first colon
 */

#include "tag_registry.h"

/* ─── String helpers (parallel to TagFS) ──────────────────────────────── */

static uint32_t RegistryHash(const char *key, const char *value, uint32_t buckets) {
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
    return hash % buckets;
}

static uint32_t KeyHash(const char *key, uint32_t buckets) {
    uint32_t hash = 5381;
    while (*key) {
        hash = ((hash << 5) + hash) + (uint8_t)(*key++);
    }
    return hash % buckets;
}

static bool ValuesEqual(const char *a, const char *b) {
    if (a == NULL && b == NULL) return true;
    if (a == NULL || b == NULL) return false;
    return strcmp(a, b) == 0;
}

static char *CopyString(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src) + 1;
    char *dst = (char *)kmalloc(len);
    if (!dst) return NULL;
    memcpy(dst, src, len);
    return dst;
}

/* Split "key:value" on FIRST colon into two malloc'd halves. Returns true on
 * success. value_out is NULL if there was no colon. Caller frees both. */
static bool SplitColon(const char *kv, char **key_out, char **value_out) {
    *key_out = NULL;
    *value_out = NULL;
    if (!kv) return false;

    const char *colon = NULL;
    for (const char *p = kv; *p; p++) {
        if (*p == ':') { colon = p; break; }
    }

    if (!colon) {
        *key_out = CopyString(kv);
        return *key_out != NULL;
    }

    size_t klen = (size_t)(colon - kv);
    *key_out = (char *)kmalloc(klen + 1);
    if (!*key_out) return false;
    memcpy(*key_out, kv, klen);
    (*key_out)[klen] = '\0';

    *value_out = CopyString(colon + 1);
    if (!*value_out) {
        kfree(*key_out);
        *key_out = NULL;
        return false;
    }
    return true;
}

/* ─── Key-group helpers (wildcard support) ────────────────────────────── */

static void AddToKeyGroup(MemTagKeyGroup *group, uint16_t tag_id) {
    if (group->count >= group->capacity) {
        uint32_t new_cap = group->capacity * 2;
        uint16_t *new_ids = (uint16_t *)kmalloc(sizeof(uint16_t) * new_cap);
        if (!new_ids) {
            debug_printf("[MEMTAG/REG] AddToKeyGroup: grow alloc failed\n");
            return;
        }
        memcpy(new_ids, group->tag_ids, sizeof(uint16_t) * group->count);
        kfree(group->tag_ids);
        group->tag_ids = new_ids;
        group->capacity = new_cap;
    }
    group->tag_ids[group->count++] = tag_id;
}

static MemTagKeyGroup *FindOrCreateKeyGroup(MemTagRegistry *reg, const char *key) {
    uint32_t bucket = KeyHash(key, reg->key_bucket_count);
    MemTagKeyGroup *group = reg->key_buckets[bucket];
    while (group) {
        if (strcmp(group->key, key) == 0) return group;
        group = group->next;
    }

    MemTagKeyGroup *new_group = (MemTagKeyGroup *)kmalloc(sizeof(MemTagKeyGroup));
    if (!new_group) {
        debug_printf("[MEMTAG/REG] FindOrCreateKeyGroup: group alloc failed\n");
        return NULL;
    }
    new_group->key = CopyString(key);
    if (!new_group->key) {
        kfree(new_group);
        return NULL;
    }
    new_group->tag_ids = (uint16_t *)kmalloc(sizeof(uint16_t) * 8);
    if (!new_group->tag_ids) {
        kfree(new_group->key);
        kfree(new_group);
        return NULL;
    }
    new_group->count    = 0;
    new_group->capacity = 8;
    new_group->next     = reg->key_buckets[bucket];
    reg->key_buckets[bucket] = new_group;
    return new_group;
}

/* ─── Capacity growth (mirrors TagFS) ─────────────────────────────────── */

static int EnsureByIdCapacity(MemTagRegistry *reg, uint16_t needed_id) {
    while (needed_id >= reg->by_id_capacity) {
        uint32_t new_cap = reg->by_id_capacity * 2;
        if (new_cap < reg->by_id_capacity) {
            /* overflow */
            return -1;
        }
        MemTagRegistryEntry **new_by_id =
            (MemTagRegistryEntry **)kmalloc(sizeof(MemTagRegistryEntry *) * new_cap);
        if (!new_by_id) {
            debug_printf("[MEMTAG/REG] EnsureByIdCapacity: alloc failed\n");
            return -1;
        }
        memcpy(new_by_id, reg->by_id,
               sizeof(MemTagRegistryEntry *) * reg->by_id_capacity);
        memset(new_by_id + reg->by_id_capacity, 0,
               sizeof(MemTagRegistryEntry *) * (new_cap - reg->by_id_capacity));
        kfree(reg->by_id);
        reg->by_id = new_by_id;
        reg->by_id_capacity = new_cap;
    }
    return 0;
}

/* ─── Core intern / lookup (must hold reg->lock) ──────────────────────── */

static uint16_t LookupUnlocked(MemTagRegistry *reg,
                                const char *key, const char *value) {
    uint32_t bucket = RegistryHash(key, value, reg->bucket_count);
    MemTagRegistryNode *node = reg->buckets[bucket];
    while (node) {
        MemTagRegistryEntry *entry = reg->by_id[node->tag_id];
        if (entry && strcmp(entry->key, key) == 0 && ValuesEqual(entry->value, value)) {
            return node->tag_id;
        }
        node = node->next;
    }
    return MEMTAG_INVALID_TAG_ID;
}

static uint16_t InternUnlocked(MemTagRegistry *reg,
                                const char *key, const char *value) {
    uint16_t existing = LookupUnlocked(reg, key, value);
    if (existing != MEMTAG_INVALID_TAG_ID) return existing;

    if (reg->next_id > MEMTAG_MAX_TAG_ID) {
        debug_printf("[MEMTAG/REG] Intern: registry full (max %u)\n",
                     MEMTAG_MAX_TAG_ID);
        return MEMTAG_INVALID_TAG_ID;
    }

    if (EnsureByIdCapacity(reg, reg->next_id) != 0) return MEMTAG_INVALID_TAG_ID;

    MemTagRegistryEntry *entry =
        (MemTagRegistryEntry *)kmalloc(sizeof(MemTagRegistryEntry));
    if (!entry) {
        debug_printf("[MEMTAG/REG] Intern: entry alloc failed\n");
        return MEMTAG_INVALID_TAG_ID;
    }

    entry->key = CopyString(key);
    if (!entry->key) { kfree(entry); return MEMTAG_INVALID_TAG_ID; }

    entry->value = CopyString(value);
    if (value && !entry->value) {
        kfree(entry->key);
        kfree(entry);
        return MEMTAG_INVALID_TAG_ID;
    }

    entry->flags        = (value != NULL) ? MEMTAG_FLAG_HAS_VALUE : 0;
    entry->tag_id       = reg->next_id;
    entry->region_count = 0;

    MemTagRegistryNode *node =
        (MemTagRegistryNode *)kmalloc(sizeof(MemTagRegistryNode));
    if (!node) {
        kfree(entry->value);
        kfree(entry->key);
        kfree(entry);
        return MEMTAG_INVALID_TAG_ID;
    }

    uint32_t bucket = RegistryHash(key, value, reg->bucket_count);
    node->tag_id = entry->tag_id;
    node->next   = reg->buckets[bucket];
    reg->buckets[bucket] = node;

    reg->by_id[entry->tag_id] = entry;

    MemTagKeyGroup *group = FindOrCreateKeyGroup(reg, key);
    if (group) AddToKeyGroup(group, entry->tag_id);

    uint16_t assigned = reg->next_id++;
    reg->total_tags++;
    reg->generation++;
    return assigned;
}

/* ─── Public API ──────────────────────────────────────────────────────── */

error_t MemTagRegistryInit(MemTagRegistry *reg) {
    if (!reg) return ERR_INVALID_ARGUMENT;

    reg->buckets = (MemTagRegistryNode **)
        kmalloc(sizeof(MemTagRegistryNode *) * MEMTAG_REG_BUCKETS);
    if (!reg->buckets) return ERR_NO_MEMORY;
    memset(reg->buckets, 0, sizeof(MemTagRegistryNode *) * MEMTAG_REG_BUCKETS);

    reg->by_id = (MemTagRegistryEntry **)
        kmalloc(sizeof(MemTagRegistryEntry *) * MEMTAG_REG_INITIAL_BY_ID);
    if (!reg->by_id) {
        kfree(reg->buckets);
        return ERR_NO_MEMORY;
    }
    memset(reg->by_id, 0, sizeof(MemTagRegistryEntry *) * MEMTAG_REG_INITIAL_BY_ID);

    reg->key_buckets = (MemTagKeyGroup **)
        kmalloc(sizeof(MemTagKeyGroup *) * MEMTAG_KEY_BUCKETS);
    if (!reg->key_buckets) {
        kfree(reg->by_id);
        kfree(reg->buckets);
        return ERR_NO_MEMORY;
    }
    memset(reg->key_buckets, 0, sizeof(MemTagKeyGroup *) * MEMTAG_KEY_BUCKETS);

    reg->bucket_count     = MEMTAG_REG_BUCKETS;
    reg->by_id_capacity   = MEMTAG_REG_INITIAL_BY_ID;
    reg->key_bucket_count = MEMTAG_KEY_BUCKETS;
    reg->total_tags       = 0;
    reg->next_id          = 0;
    reg->generation       = 0;

    spinlock_init(&reg->lock);

    debug_printf("[MEMTAG/REG] Init: %u reg_buckets, %u key_buckets, by_id=%u\n",
                 MEMTAG_REG_BUCKETS, MEMTAG_KEY_BUCKETS, MEMTAG_REG_INITIAL_BY_ID);
    return OK;
}

void MemTagRegistryShutdown(MemTagRegistry *reg) {
    if (!reg) return;

    for (uint32_t i = 0; i < reg->bucket_count; i++) {
        MemTagRegistryNode *node = reg->buckets[i];
        while (node) {
            MemTagRegistryNode *next = node->next;
            kfree(node);
            node = next;
        }
    }
    kfree(reg->buckets);
    reg->buckets = NULL;

    for (uint32_t i = 0; i < reg->next_id; i++) {
        MemTagRegistryEntry *e = reg->by_id[i];
        if (e) {
            kfree(e->key);
            if (e->value) kfree(e->value);
            kfree(e);
        }
    }
    kfree(reg->by_id);
    reg->by_id = NULL;

    for (uint32_t i = 0; i < reg->key_bucket_count; i++) {
        MemTagKeyGroup *g = reg->key_buckets[i];
        while (g) {
            MemTagKeyGroup *next = g->next;
            kfree(g->key);
            kfree(g->tag_ids);
            kfree(g);
            g = next;
        }
    }
    kfree(reg->key_buckets);
    reg->key_buckets = NULL;
}

uint16_t MemTagRegistryIntern(MemTagRegistry *reg,
                               const char *key, const char *value) {
    if (!reg || !key) return MEMTAG_INVALID_TAG_ID;
    spin_lock(&reg->lock);
    uint16_t id = InternUnlocked(reg, key, value);
    spin_unlock(&reg->lock);
    return id;
}

uint16_t MemTagRegistryInternStr(MemTagRegistry *reg, const char *kv) {
    if (!reg || !kv) return MEMTAG_INVALID_TAG_ID;
    char *k = NULL, *v = NULL;
    if (!SplitColon(kv, &k, &v)) return MEMTAG_INVALID_TAG_ID;
    uint16_t id = MemTagRegistryIntern(reg, k, v);
    kfree(k);
    if (v) kfree(v);
    return id;
}

uint16_t MemTagRegistryLookup(MemTagRegistry *reg,
                               const char *key, const char *value) {
    if (!reg || !key) return MEMTAG_INVALID_TAG_ID;
    spin_lock(&reg->lock);
    uint16_t id = LookupUnlocked(reg, key, value);
    spin_unlock(&reg->lock);
    return id;
}

uint16_t MemTagRegistryLookupStr(MemTagRegistry *reg, const char *kv) {
    if (!reg || !kv) return MEMTAG_INVALID_TAG_ID;
    char *k = NULL, *v = NULL;
    if (!SplitColon(kv, &k, &v)) return MEMTAG_INVALID_TAG_ID;
    uint16_t id = MemTagRegistryLookup(reg, k, v);
    kfree(k);
    if (v) kfree(v);
    return id;
}

const char *MemTagRegistryKey(MemTagRegistry *reg, uint16_t tag_id) {
    if (!reg || tag_id == MEMTAG_INVALID_TAG_ID) return NULL;
    spin_lock(&reg->lock);
    const char *result = NULL;
    if (tag_id < reg->next_id && reg->by_id[tag_id])
        result = reg->by_id[tag_id]->key;
    spin_unlock(&reg->lock);
    return result;
}

const char *MemTagRegistryValue(MemTagRegistry *reg, uint16_t tag_id) {
    if (!reg || tag_id == MEMTAG_INVALID_TAG_ID) return NULL;
    spin_lock(&reg->lock);
    const char *result = NULL;
    if (tag_id < reg->next_id && reg->by_id[tag_id])
        result = reg->by_id[tag_id]->value;
    spin_unlock(&reg->lock);
    return result;
}

MemTagKeyGroup *MemTagRegistryKeyGroup(MemTagRegistry *reg, const char *key) {
    if (!reg || !key) return NULL;
    spin_lock(&reg->lock);
    uint32_t bucket = KeyHash(key, reg->key_bucket_count);
    MemTagKeyGroup *group = reg->key_buckets[bucket];
    while (group) {
        if (strcmp(group->key, key) == 0) break;
        group = group->next;
    }
    spin_unlock(&reg->lock);
    return group;
}

uint64_t MemTagRegistryGeneration(MemTagRegistry *reg) {
    if (!reg) return 0;
    /* Atomic 64-bit read on x86-64 — aligned uint64_t is atomic by ISA */
    return reg->generation;
}

uint32_t MemTagRegistryTotalTags(MemTagRegistry *reg) {
    if (!reg) return 0;
    return reg->total_tags;
}

void MemTagRegistryRefInc(MemTagRegistry *reg, uint16_t tag_id) {
    if (!reg || tag_id == MEMTAG_INVALID_TAG_ID) return;
    spin_lock(&reg->lock);
    if (tag_id < reg->next_id && reg->by_id[tag_id])
        reg->by_id[tag_id]->region_count++;
    spin_unlock(&reg->lock);
}

void MemTagRegistryRefDec(MemTagRegistry *reg, uint16_t tag_id) {
    if (!reg || tag_id == MEMTAG_INVALID_TAG_ID) return;
    spin_lock(&reg->lock);
    if (tag_id < reg->next_id && reg->by_id[tag_id]) {
        MemTagRegistryEntry *e = reg->by_id[tag_id];
        if (e->region_count > 0) e->region_count--;
    }
    spin_unlock(&reg->lock);
}

uint32_t MemTagRegistryRefCount(MemTagRegistry *reg, uint16_t tag_id) {
    if (!reg || tag_id == MEMTAG_INVALID_TAG_ID) return 0;
    spin_lock(&reg->lock);
    uint32_t rc = 0;
    if (tag_id < reg->next_id && reg->by_id[tag_id])
        rc = reg->by_id[tag_id]->region_count;
    spin_unlock(&reg->lock);
    return rc;
}

void MemTagRegistryMarkReserved(MemTagRegistry *reg, uint16_t tag_id) {
    if (!reg || tag_id == MEMTAG_INVALID_TAG_ID) return;
    spin_lock(&reg->lock);
    if (tag_id < reg->next_id && reg->by_id[tag_id])
        reg->by_id[tag_id]->flags |= MEMTAG_FLAG_KERNEL_RESERVED;
    spin_unlock(&reg->lock);
}

bool MemTagRegistryIsReserved(MemTagRegistry *reg, uint16_t tag_id) {
    if (!reg || tag_id == MEMTAG_INVALID_TAG_ID) return false;
    spin_lock(&reg->lock);
    bool reserved = false;
    if (tag_id < reg->next_id && reg->by_id[tag_id])
        reserved = (reg->by_id[tag_id]->flags & MEMTAG_FLAG_KERNEL_RESERVED) != 0;
    spin_unlock(&reg->lock);
    return reserved;
}

void MemTagRegistryMarkGuard(MemTagRegistry *reg, uint16_t tag_id) {
    if (!reg || tag_id == MEMTAG_INVALID_TAG_ID) return;
    spin_lock(&reg->lock);
    if (tag_id < reg->next_id && reg->by_id[tag_id]) {
        reg->by_id[tag_id]->flags |= MEMTAG_FLAG_GUARD;
        reg->generation++;
    }
    spin_unlock(&reg->lock);
}

void MemTagRegistryClearGuard(MemTagRegistry *reg, uint16_t tag_id) {
    if (!reg || tag_id == MEMTAG_INVALID_TAG_ID) return;
    spin_lock(&reg->lock);
    if (tag_id < reg->next_id && reg->by_id[tag_id]) {
        reg->by_id[tag_id]->flags &= (uint8_t)~MEMTAG_FLAG_GUARD;
        reg->generation++;
    }
    spin_unlock(&reg->lock);
}

bool MemTagRegistryIsGuard(MemTagRegistry *reg, uint16_t tag_id) {
    if (!reg || tag_id == MEMTAG_INVALID_TAG_ID) return false;
    spin_lock(&reg->lock);
    bool guard = false;
    if (tag_id < reg->next_id && reg->by_id[tag_id])
        guard = (reg->by_id[tag_id]->flags & MEMTAG_FLAG_GUARD) != 0;
    spin_unlock(&reg->lock);
    return guard;
}

void MemTagRegistryDump(MemTagRegistry *reg) {
    if (!reg) return;
    spin_lock(&reg->lock);
    debug_printf("[MEMTAG/REG] %u tags  gen=%lu  cap=%u\n",
                 reg->total_tags, (unsigned long)reg->generation,
                 reg->by_id_capacity);
    for (uint32_t i = 0; i < reg->next_id; i++) {
        MemTagRegistryEntry *e = reg->by_id[i];
        if (!e) continue;
        debug_printf("[MEMTAG/REG]   [%5u] %s%s%s  flags=0x%02x  refs=%u\n",
                     e->tag_id, e->key,
                     e->value ? ":" : "",
                     e->value ? e->value : "",
                     e->flags, e->region_count);
    }
    spin_unlock(&reg->lock);
}
