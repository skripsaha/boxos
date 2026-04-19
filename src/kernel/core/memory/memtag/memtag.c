#include "memtag.h"
#include "pmm.h"

static char         g_tag_names[MEMTAG_REGISTRY_CAP][MEMTAG_NAME_MAX];
static uint8_t      g_tag_count = 0;
static MemTagRegion g_regions[MEMTAG_REGION_CAP];
static spinlock_t   g_lock;
static bool         g_initialized = false;

// Inverted index: g_tag_index[tag_id] is a bitmap over region slots.
// Bit (slot & 63) of word (slot >> 6) is set iff region at slot has tag_id.
static uint64_t g_tag_index[MEMTAG_REGISTRY_CAP][MEMTAG_REG_WORDS];

// Bitmap of active region slots.
static uint64_t g_active_bits[MEMTAG_REG_WORDS];

// ─── Bit helpers ──────────────────────────────────────────────────────────────

static inline bool RegionHasTag(const MemTagRegion *r, memtag_id_t id) {
    return (r->tag_bits[id >> 6] >> (id & 63)) & 1ULL;
}

static inline void RegionSetTag(MemTagRegion *r, memtag_id_t id) {
    r->tag_bits[id >> 6] |= 1ULL << (id & 63);
}

static inline void RegionClearTag(MemTagRegion *r, memtag_id_t id) {
    r->tag_bits[id >> 6] &= ~(1ULL << (id & 63));
}

static inline size_t RegionSlot(const MemTagRegion *r) {
    return (size_t)(r - g_regions);
}

static inline void IndexAdd(memtag_id_t id, size_t slot) {
    g_tag_index[id][slot >> 6] |= 1ULL << (slot & 63);
}

static inline void IndexRemove(memtag_id_t id, size_t slot) {
    g_tag_index[id][slot >> 6] &= ~(1ULL << (slot & 63));
}

// Attach tag to region and update inverted index. Must be called under g_lock.
static void AttachTagLocked(MemTagRegion *r, memtag_id_t id) {
    if (RegionHasTag(r, id))
        return;
    RegionSetTag(r, id);
    IndexAdd(id, RegionSlot(r));
}

// Detach tag from region and update inverted index. Must be called under g_lock.
static void DetachTagLocked(MemTagRegion *r, memtag_id_t id) {
    if (!RegionHasTag(r, id))
        return;
    RegionClearTag(r, id);
    IndexRemove(id, RegionSlot(r));
}

// ─── Internal ─────────────────────────────────────────────────────────────────

static MemTagRegion *FindRegionLocked(uintptr_t base) {
    for (size_t i = 0; i < MEMTAG_REGION_CAP; i++) {
        if (g_regions[i].active && g_regions[i].base_phys == base)
            return &g_regions[i];
    }
    return NULL;
}

static MemTagRegion *AllocSlotLocked(void) {
    for (size_t i = 0; i < MEMTAG_REGION_CAP; i++) {
        if (!g_regions[i].active)
            return &g_regions[i];
    }
    return NULL;
}

static memtag_id_t GetOrCreateLocked(const char *name) {
    for (uint8_t i = 0; i < g_tag_count; i++) {
        if (strncmp(g_tag_names[i], name, MEMTAG_NAME_MAX) == 0)
            return i;
    }
    if (g_tag_count >= MEMTAG_REGISTRY_CAP) {
        debug_printf("[MEMTAG] Registry full — ignoring tag '%s'\n", name);
        return MEMTAG_ID_NONE;
    }
    uint8_t id = g_tag_count++;
    strncpy(g_tag_names[id], name, MEMTAG_NAME_MAX - 1);
    g_tag_names[id][MEMTAG_NAME_MAX - 1] = '\0';
    return id;
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

error_t MemTagInit(void) {
    if (g_initialized)
        return ERR_ALREADY_INITIALIZED;
    memset(g_tag_names,  0, sizeof(g_tag_names));
    memset(g_regions,    0, sizeof(g_regions));
    memset(g_tag_index,  0, sizeof(g_tag_index));
    memset(g_active_bits, 0, sizeof(g_active_bits));
    g_tag_count = 0;
    spinlock_init(&g_lock);
    g_initialized = true;
    debug_printf("[MEMTAG] Initialized: %u region slots, %u tag slots, index %zu KB\n",
                 MEMTAG_REGION_CAP, MEMTAG_REGISTRY_CAP,
                 sizeof(g_tag_index) / 1024);
    return OK;
}

// ─── Registry ─────────────────────────────────────────────────────────────────

memtag_id_t MemTagLookup(const char *name) {
    if (!name || !g_initialized)
        return MEMTAG_ID_NONE;
    spin_lock(&g_lock);
    memtag_id_t id = MEMTAG_ID_NONE;
    for (uint8_t i = 0; i < g_tag_count; i++) {
        if (strncmp(g_tag_names[i], name, MEMTAG_NAME_MAX) == 0) {
            id = i;
            break;
        }
    }
    spin_unlock(&g_lock);
    return id;
}

memtag_id_t MemTagRegister(const char *name) {
    if (!name)
        return MEMTAG_ID_NONE;
    spin_lock(&g_lock);
    memtag_id_t id = GetOrCreateLocked(name);
    spin_unlock(&g_lock);
    return id;
}

const char *MemTagName(memtag_id_t id) {
    if (id == MEMTAG_ID_NONE || id >= g_tag_count)
        return "(none)";
    return g_tag_names[id];
}

// ─── Region management ────────────────────────────────────────────────────────

error_t MemTagAdd(uintptr_t base_phys, size_t pages, ...) {
    if (!g_initialized)
        return ERR_NOT_INITIALIZED;

    spin_lock(&g_lock);

    MemTagRegion *r = FindRegionLocked(base_phys);
    if (!r) {
        r = AllocSlotLocked();
        if (!r) {
            spin_unlock(&g_lock);
            debug_printf("[MEMTAG] Region pool full — cannot track 0x%lx\n", base_phys);
            return ERR_NO_MEMORY;
        }
        size_t slot = RegionSlot(r);
        memset(r, 0, sizeof(MemTagRegion));
        r->base_phys = base_phys;
        r->pages     = pages;
        r->active    = true;
        g_active_bits[slot >> 6] |= 1ULL << (slot & 63);
    }

    va_list args;
    va_start(args, pages);
    const char *tag;
    while ((tag = va_arg(args, const char *)) != NULL) {
        memtag_id_t id = GetOrCreateLocked(tag);
        if (id != MEMTAG_ID_NONE)
            AttachTagLocked(r, id);
    }
    va_end(args);

    spin_unlock(&g_lock);
    return OK;
}

error_t MemTagAddToRegion(uintptr_t base_phys, const char *tag) {
    if (!g_initialized || !tag)
        return ERR_INVALID_ARGUMENT;
    spin_lock(&g_lock);
    MemTagRegion *r = FindRegionLocked(base_phys);
    if (!r) {
        spin_unlock(&g_lock);
        return ERR_OBJECT_NOT_FOUND;
    }
    memtag_id_t id = GetOrCreateLocked(tag);
    if (id != MEMTAG_ID_NONE)
        AttachTagLocked(r, id);
    spin_unlock(&g_lock);
    return OK;
}

error_t MemTagRemoveFromRegion(uintptr_t base_phys, const char *tag) {
    if (!g_initialized || !tag)
        return ERR_INVALID_ARGUMENT;
    spin_lock(&g_lock);
    MemTagRegion *r = FindRegionLocked(base_phys);
    if (!r) {
        spin_unlock(&g_lock);
        return ERR_OBJECT_NOT_FOUND;
    }
    memtag_id_t id = MEMTAG_ID_NONE;
    for (uint8_t i = 0; i < g_tag_count; i++) {
        if (strncmp(g_tag_names[i], tag, MEMTAG_NAME_MAX) == 0) {
            id = i;
            break;
        }
    }
    if (id != MEMTAG_ID_NONE)
        DetachTagLocked(r, id);
    spin_unlock(&g_lock);
    return OK;
}

void MemTagRemoveRegion(uintptr_t base_phys) {
    if (!g_initialized)
        return;
    spin_lock(&g_lock);
    MemTagRegion *r = FindRegionLocked(base_phys);
    if (r) {
        size_t slot = RegionSlot(r);
        for (uint8_t t = 0; t < g_tag_count; t++)
            DetachTagLocked(r, t);
        g_active_bits[slot >> 6] &= ~(1ULL << (slot & 63));
        memset(r, 0, sizeof(MemTagRegion));
    }
    spin_unlock(&g_lock);
}

MemTagRegion *MemTagFindRegion(uintptr_t phys_addr) {
    if (!g_initialized)
        return NULL;
    spin_lock(&g_lock);
    MemTagRegion *result = NULL;
    for (size_t i = 0; i < MEMTAG_REGION_CAP; i++) {
        if (!g_regions[i].active)
            continue;
        uintptr_t end = g_regions[i].base_phys + g_regions[i].pages * PMM_PAGE_SIZE;
        if (phys_addr >= g_regions[i].base_phys && phys_addr < end) {
            result = &g_regions[i];
            break;
        }
    }
    spin_unlock(&g_lock);
    return result;
}

// ─── Allocation helper ────────────────────────────────────────────────────────

void *_pmm_alloc_memtag(size_t pages, const char *tag) {
    void *phys = _pmm_alloc_impl(pages, 0ULL);
    if (!phys)
        return NULL;
    if (tag && g_initialized)
        MemTagAdd((uintptr_t)phys, pages, tag, NULL);
    return phys;
}

// ─── Queries ──────────────────────────────────────────────────────────────────

// Resolve NULL-terminated name array to IDs. MemTagLookup acquires/releases
// g_lock internally; this must be called BEFORE the caller acquires g_lock.
static void ResolveTags(const char *const *names, memtag_id_t *ids, size_t *count) {
    *count = 0;
    if (!names)
        return;
    for (size_t i = 0; names[i] != NULL && *count < 16; i++)
        ids[(*count)++] = MemTagLookup(names[i]);
}

static MemTagResult RunQuery(const memtag_id_t *req,  size_t n_req,
                              const memtag_id_t *any,  size_t n_any,
                              const memtag_id_t *excl, size_t n_excl) {
    MemTagResult out = { .count = 0 };

    spin_lock(&g_lock);

    // Build result bitmap.
    uint64_t result[MEMTAG_REG_WORDS];

    if (n_req > 0) {
        // Seed with first required tag's bitmap.
        memtag_id_t first = req[0];
        if (first == MEMTAG_ID_NONE) {
            // Unknown required tag — no results possible.
            spin_unlock(&g_lock);
            return out;
        }
        for (size_t w = 0; w < MEMTAG_REG_WORDS; w++)
            result[w] = g_tag_index[first][w];
        // AND remaining required tags.
        for (size_t i = 1; i < n_req; i++) {
            if (req[i] == MEMTAG_ID_NONE) {
                spin_unlock(&g_lock);
                return out;
            }
            for (size_t w = 0; w < MEMTAG_REG_WORDS; w++)
                result[w] &= g_tag_index[req[i]][w];
        }
    } else {
        // No required tags — start with all active regions.
        for (size_t w = 0; w < MEMTAG_REG_WORDS; w++)
            result[w] = g_active_bits[w];
    }

    // OR of any-tags → intersect with result.
    if (n_any > 0) {
        uint64_t any_mask[MEMTAG_REG_WORDS];
        memset(any_mask, 0, sizeof(any_mask));
        for (size_t i = 0; i < n_any; i++) {
            if (any[i] == MEMTAG_ID_NONE)
                continue;
            for (size_t w = 0; w < MEMTAG_REG_WORDS; w++)
                any_mask[w] |= g_tag_index[any[i]][w];
        }
        for (size_t w = 0; w < MEMTAG_REG_WORDS; w++)
            result[w] &= any_mask[w];
    }

    // Remove excluded tags.
    for (size_t i = 0; i < n_excl; i++) {
        if (excl[i] == MEMTAG_ID_NONE)
            continue;
        for (size_t w = 0; w < MEMTAG_REG_WORDS; w++)
            result[w] &= ~g_tag_index[excl[i]][w];
    }

    // Enumerate set bits via ctzll + bit-clear loop.
    for (size_t w = 0; w < MEMTAG_REG_WORDS; w++) {
        uint64_t word = result[w];
        while (word && out.count < MEMTAG_QUERY_RESULT_MAX) {
            int bit = __builtin_ctzll(word);
            size_t slot = w * 64 + (size_t)bit;
            if (g_regions[slot].active)
                out.regions[out.count++] = &g_regions[slot];
            word &= word - 1;
        }
    }

    spin_unlock(&g_lock);
    return out;
}

MemTagResult MemTagQueryAnd(const char *const *tags) {
    memtag_id_t ids[16];
    size_t n;
    ResolveTags(tags, ids, &n);
    return RunQuery(ids, n, NULL, 0, NULL, 0);
}

MemTagResult MemTagQueryOr(const char *const *tags) {
    memtag_id_t ids[16];
    size_t n;
    ResolveTags(tags, ids, &n);
    return RunQuery(NULL, 0, ids, n, NULL, 0);
}

MemTagResult MemTagQueryMixed(const char *const *required,
                               const char *const *any,
                               const char *const *excluded) {
    memtag_id_t req[16], an[16], ex[16];
    size_t nr = 0, na = 0, ne = 0;
    ResolveTags(required, req, &nr);
    ResolveTags(any,      an,  &na);
    ResolveTags(excluded, ex,  &ne);
    return RunQuery(req, nr, an, na, ex, ne);
}

// ─── Dump ─────────────────────────────────────────────────────────────────────

void MemTagDump(void) {
    if (!g_initialized) {
        debug_printf("[MEMTAG] Not initialized\n");
        return;
    }
    spin_lock(&g_lock);
    debug_printf("[MEMTAG] Registry (%u/%u tags):\n", g_tag_count, MEMTAG_REGISTRY_CAP);
    for (uint8_t i = 0; i < g_tag_count; i++)
        debug_printf("[MEMTAG]   [%3u] %s\n", i, g_tag_names[i]);
    size_t active = 0;
    for (size_t i = 0; i < MEMTAG_REGION_CAP; i++) {
        if (!g_regions[i].active)
            continue;
        active++;
        debug_printf("[MEMTAG]   phys=0x%016lx  pages=%-4zu  tags:",
                     g_regions[i].base_phys, g_regions[i].pages);
        for (uint8_t t = 0; t < g_tag_count; t++) {
            if (RegionHasTag(&g_regions[i], t))
                debug_printf(" [%s]", g_tag_names[t]);
        }
        debug_printf("\n");
    }
    debug_printf("[MEMTAG] Active: %zu / %u regions\n", active, MEMTAG_REGION_CAP);
    spin_unlock(&g_lock);
}
