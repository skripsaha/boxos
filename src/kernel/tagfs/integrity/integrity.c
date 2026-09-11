#include "integrity.h"
#include "../tagfs.h"
#include "../box_hash/box_hash.h"
#include "../../lib/kernel/klib.h"
#include "touch.h"

#define SB_MAP_BLOCK(fs)  ((fs)->ledger.integrity_map_block)
#define SB_MAP_COUNT(fs)  ((fs)->ledger.integrity_map_blocks)

#define ENTRIES_PER_BLOCK (TAGFS_BLOCK_SIZE / (uint32_t)sizeof(uint64_t))
#define MAP_CACHE_SLOTS   16
#define ROT_RING_MAX      64
#define SLOT_EMPTY        0xFFFFFFFFu

typedef struct {
    uint32_t map_idx;
    uint32_t lru;
    bool     dirty;
    uint64_t entries[ENTRIES_PER_BLOCK];
} MapSlot;

static bool           g_initialized   = false;
static spinlock_t     g_lock;
static uint32_t       g_map_entries   = 0;
static uint32_t       g_map_first_block = 0;
static uint32_t       g_map_count     = 0;
static MapSlot        g_cache[MAP_CACHE_SLOTS];
static uint32_t       g_lru_tick      = 0;
static BoxHashContext g_ctx;
static uint32_t       g_errors        = 0;
static uint32_t       g_rot_ring[ROT_RING_MAX];
static uint32_t       g_rot_count     = 0;
static uint16_t       g_integrity_tag = 0xFFFF;

static inline bool in_map_region(uint32_t block) {
    return g_map_first_block != 0 &&
           block >= g_map_first_block && block < g_map_first_block + g_map_count;
}

static MapSlot *cache_get(uint32_t idx) {
    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; s++) {
        if (g_cache[s].map_idx == idx) {
            g_cache[s].lru = ++g_lru_tick;
            return &g_cache[s];
        }
    }
    uint32_t victim = 0;
    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; s++) {
        if (g_cache[s].map_idx == SLOT_EMPTY) { victim = s; break; }
        if (g_cache[s].lru < g_cache[victim].lru) victim = s;
    }
    MapSlot *v = &g_cache[victim];

    if (v->map_idx != SLOT_EMPTY && v->dirty) {
        uint8_t buf[TAGFS_BLOCK_SIZE];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, v->entries, sizeof(v->entries));
        if (tagfs_write_block(g_map_first_block + v->map_idx, buf) != OK) {
            return NULL;
        }
    }

    uint8_t buf[TAGFS_BLOCK_SIZE];
    if (tagfs_read_block(g_map_first_block + idx, buf) != OK)
        return NULL;
    memcpy(v->entries, buf, sizeof(v->entries));
    v->map_idx = idx;
    v->dirty   = false;
    v->lru     = ++g_lru_tick;
    return v;
}

error_t IntegrityInit(void) {
    if (g_initialized)
        return ERR_ALREADY_INITIALIZED;

    TagFSState *fs = tagfs_get_state();
    if (!fs || fs->layout.data_blocks == 0)
        return ERR_NOT_INITIALIZED;

    spinlock_init(&g_lock);
    g_map_entries = fs->layout.data_blocks;
    g_map_count   = (g_map_entries + ENTRIES_PER_BLOCK - 1) / ENTRIES_PER_BLOCK;
    BoxHashInit(&g_ctx, fs->uuid, 16);

    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; s++) {
        g_cache[s].map_idx = SLOT_EMPTY;
        g_cache[s].dirty   = false;
    }
    g_lru_tick = 0;
    g_rot_count = 0;

    uint32_t first = SB_MAP_BLOCK(fs);
    if (first == 0) {
        uint32_t start = 0;
        if (tagfs_alloc_blocks(g_map_count, &start) != 0 || start == 0) {
            debug_printf("[Integrity] could not allocate %u map blocks — disabled\n", g_map_count);
            return ERR_NO_MEMORY;
        }
        g_map_first_block = start;
        SB_MAP_BLOCK(fs) = start;
        SB_MAP_COUNT(fs) = g_map_count;

        uint8_t zero[TAGFS_BLOCK_SIZE];
        memset(zero, 0, sizeof(zero));
        for (uint32_t i = 0; i < g_map_count; i++)
            tagfs_write_block(start + i, zero);

        tagfs_write_ledger();
        debug_printf("[Integrity] fresh map: %u blocks at %u (covers %u, %u-slot cache)\n",
                     g_map_count, start, g_map_entries, MAP_CACHE_SLOTS);
    } else {
        g_map_first_block = first;

        uint32_t stored = SB_MAP_COUNT(fs);
        if (stored != 0 && stored < g_map_count) {
            uint32_t start = 0;
            uint8_t *page  = (uint8_t *)kmalloc(TAGFS_BLOCK_SIZE);
            bool moved = false;

            if (page && tagfs_alloc_blocks(g_map_count, &start) == 0 && start != 0) {
                moved = true;
                for (uint32_t i = 0; i < g_map_count && moved; i++) {
                    if (i < stored) {
                        if (tagfs_read_block(first + i, page) != OK) { moved = false; break; }
                    } else {
                        memset(page, 0, TAGFS_BLOCK_SIZE);
                    }
                    if (tagfs_write_block(start + i, page) != OK) moved = false;
                }
            }
            kfree(page);

            if (moved) {
                uint32_t was_block = SB_MAP_BLOCK(fs);
                uint32_t was_count = SB_MAP_COUNT(fs);
                SB_MAP_BLOCK(fs) = start;
                SB_MAP_COUNT(fs) = g_map_count;
                if (tagfs_write_ledger() != OK) {
                    SB_MAP_BLOCK(fs) = was_block;
                    SB_MAP_COUNT(fs) = was_count;
                    tagfs_free_blocks(start, g_map_count);
                    start = 0;
                    moved = false;
                    kprintf("[Integrity] a bigger map was laid down and the "
                            "ledger would not take it — the map stays where it "
                            "was and the room is given back\n");
                }
            }

            if (moved) {
                tagfs_free_blocks(first, stored);
                g_map_first_block = start;
                kprintf("[Integrity] the volume grew, so its map did too: %u "
                        "block(s) at %u, carrying the %u it already had — every "
                        "block of %u is covered\n",
                        g_map_count, start, stored, g_map_entries);
            } else {
                if (start) tagfs_free_blocks(start, g_map_count);
                uint32_t covered = stored * ENTRIES_PER_BLOCK;
                g_map_count   = stored;
                g_map_entries = covered;
                kprintf("[Integrity] the map covers %u block(s) and the volume "
                        "now has %u — there was no room to lay down a bigger "
                        "one, so verify-on-read covers the first %u and says "
                        "nothing about the rest\n",
                        covered, fs->layout.data_blocks, covered);
            }
        }

        debug_printf("[Integrity] map at %u (%u blocks, %u-slot paged cache)\n",
                     g_map_first_block, g_map_count, MAP_CACHE_SLOTS);
    }

    if (fs->registry)
        g_integrity_tag = tag_registry_intern(fs->registry, "integrity", NULL);

    g_initialized = true;
    return OK;
}

void IntegrityUpdate(uint32_t block, const void *data) {
    if (!g_initialized || !data || block >= g_map_entries || in_map_region(block))
        return;
    uint64_t h   = BoxHashIntegrity(data, TAGFS_BLOCK_SIZE, &g_ctx);
    uint32_t idx = block / ENTRIES_PER_BLOCK;
    uint32_t off = block % ENTRIES_PER_BLOCK;
    spin_lock(&g_lock);
    MapSlot *s = cache_get(idx);
    if (s && s->entries[off] != h) {
        s->entries[off] = h;
        s->dirty = true;
    }
    spin_unlock(&g_lock);
}

bool IntegrityVerify(uint32_t block, const void *data) {
    if (!g_initialized || !data || block >= g_map_entries || in_map_region(block))
        return true;
    uint32_t idx = block / ENTRIES_PER_BLOCK;
    uint32_t off = block % ENTRIES_PER_BLOCK;
    spin_lock(&g_lock);
    MapSlot *s    = cache_get(idx);
    uint64_t stored = s ? s->entries[off] : 0;
    spin_unlock(&g_lock);

    if (!s || stored == 0)
        return true;
    uint64_t h = BoxHashIntegrity(data, TAGFS_BLOCK_SIZE, &g_ctx);
    if (h == stored)
        return true;

    __atomic_fetch_add(&g_errors, 1, __ATOMIC_RELAXED);
    spin_lock(&g_lock);
    if (g_rot_count < ROT_RING_MAX)
        g_rot_ring[g_rot_count++] = block;
    spin_unlock(&g_lock);
    debug_printf("[Integrity] BIT-ROT on block %u: stored=%016lx read=%016lx\n",
                 block, (unsigned long)stored, (unsigned long)h);
    return false;
}

error_t IntegrityFlush(void) {
    if (!g_initialized)
        return OK;
    spin_lock(&g_lock);
    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; s++) {
        if (g_cache[s].map_idx == SLOT_EMPTY || !g_cache[s].dirty)
            continue;
        uint8_t buf[TAGFS_BLOCK_SIZE];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, g_cache[s].entries, sizeof(g_cache[s].entries));
        if (tagfs_write_block(g_map_first_block + g_cache[s].map_idx, buf) == OK)
            g_cache[s].dirty = false;
    }
    spin_unlock(&g_lock);
    return OK;
}

void IntegrityDrainReports(void) {
    if (!g_initialized || g_integrity_tag == 0xFFFF)
        return;
    uint32_t local[ROT_RING_MAX];
    uint32_t n;
    spin_lock(&g_lock);
    n = g_rot_count;
    if (n > ROT_RING_MAX) n = ROT_RING_MAX;
    memcpy(local, g_rot_ring, n * sizeof(uint32_t));
    g_rot_count = 0;
    spin_unlock(&g_lock);

    for (uint32_t i = 0; i < n; i++) {
        struct __attribute__((packed)) {
            uint32_t block;
            uint8_t  op;
            uint8_t  _pad[3];
        } ev = { local[i], 3, {0, 0, 0} };
        TouchPublishId(g_integrity_tag, &ev, sizeof(ev), 0, TOUCH_FLAG_TAGFS);
    }
}

void IntegrityShutdown(void) {
    if (!g_initialized)
        return;
    IntegrityFlush();
    spin_lock(&g_lock);
    g_initialized = false;
    for (uint32_t s = 0; s < MAP_CACHE_SLOTS; s++)
        g_cache[s].map_idx = SLOT_EMPTY;
    g_map_first_block = 0;
    g_map_count = 0;
    g_map_entries = 0;
    spin_unlock(&g_lock);
}

bool IntegrityIsInitialized(void) { return g_initialized; }
uint32_t IntegrityErrorCount(void) { return g_errors; }

void IntegrityMarkMapBlocks(uint8_t *computed_bm, uint32_t total_blocks) {
    if (!computed_bm)
        return;
    TagFSState *fs = tagfs_get_state();
    if (!fs)
        return;
    uint32_t first = SB_MAP_BLOCK(fs);
    uint32_t cnt   = SB_MAP_COUNT(fs);
    if (first == 0)
        return;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t b = first + i;
        if (b < total_blocks)
            computed_bm[b / 8] |= (uint8_t)(1u << (b % 8));
    }
}