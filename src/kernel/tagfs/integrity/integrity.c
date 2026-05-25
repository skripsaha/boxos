#include "integrity.h"
#include "../tagfs.h"
#include "../box_hash/box_hash.h"
#include "../../lib/kernel/klib.h"

// Superblock reserved[] layout: 0..15 boot hints, 16..23 CoW manifest blocks,
// 24..27 integrity-map first block, 28..31 integrity-map block count, 399/400
// CRC sentinel+value. 24..31 are otherwise unused.
#define INTEG_MAP_ROFF    24
#define INTEG_COUNT_ROFF  28
#define SB_MAP_BLOCK(sb)  (*(uint32_t *)((sb)->reserved + INTEG_MAP_ROFF))
#define SB_MAP_COUNT(sb)  (*(uint32_t *)((sb)->reserved + INTEG_COUNT_ROFF))

#define ENTRIES_PER_BLOCK (TAGFS_BLOCK_SIZE / (uint32_t)sizeof(uint64_t))  // 512

static bool            g_initialized   = false;
static spinlock_t      g_lock;
static uint64_t       *g_map           = NULL;   // per-block digest, [0..g_map_entries)
static uint32_t        g_map_entries   = 0;      // == total_blocks
static uint32_t        g_map_first_block = 0;    // first data block holding the map
static uint32_t        g_map_count     = 0;      // map data blocks
static bool           *g_dirty         = NULL;   // per map data block
static BoxHashContext  g_ctx;
static uint32_t        g_errors        = 0;

static inline bool in_map_region(uint32_t block) {
    return g_map_first_block != 0 &&
           block >= g_map_first_block && block < g_map_first_block + g_map_count;
}

error_t IntegrityInit(void) {
    if (g_initialized)
        return ERR_ALREADY_INITIALIZED;

    TagFSState *fs = tagfs_get_state();
    if (!fs || fs->superblock.total_blocks == 0)
        return ERR_NOT_INITIALIZED;

    spinlock_init(&g_lock);
    uint32_t total = fs->superblock.total_blocks;
    g_map_entries  = total;
    g_map_count    = (total + ENTRIES_PER_BLOCK - 1) / ENTRIES_PER_BLOCK;

    BoxHashInit(&g_ctx, fs->superblock.fs_uuid, 16);

    g_map   = kmalloc((uint32_t)(total * sizeof(uint64_t)));
    g_dirty = kmalloc(g_map_count * sizeof(bool));
    if (!g_map || !g_dirty) {
        if (g_map) { kfree(g_map); g_map = NULL; }
        if (g_dirty) { kfree(g_dirty); g_dirty = NULL; }
        debug_printf("[Integrity] alloc failed — integrity disabled\n");
        return ERR_NO_MEMORY;
    }
    memset(g_map, 0, (uint32_t)(total * sizeof(uint64_t)));
    memset(g_dirty, 0, g_map_count * sizeof(bool));

    uint32_t first = SB_MAP_BLOCK(&fs->superblock);
    if (first == 0) {
        // First mount with this feature: lazily allocate a contiguous map region.
        uint32_t start = 0;
        if (tagfs_alloc_blocks(g_map_count, &start) != 0 || start == 0) {
            kfree(g_map);  g_map = NULL;
            kfree(g_dirty); g_dirty = NULL;
            debug_printf("[Integrity] could not allocate %u map blocks — disabled\n", g_map_count);
            return ERR_NO_MEMORY;
        }
        g_map_first_block = start;
        SB_MAP_BLOCK(&fs->superblock)  = start;
        SB_MAP_COUNT(&fs->superblock)  = g_map_count;

        uint8_t zero[TAGFS_BLOCK_SIZE];
        memset(zero, 0, sizeof(zero));
        for (uint32_t i = 0; i < g_map_count; i++)
            tagfs_write_block(start + i, zero);

        // Persist the superblock so the map location survives; the bitmap (map
        // blocks marked used) flushes at the next sync, and mount-fsck re-marks
        // the region from the superblock either way (no reuse on crash).
        tagfs_write_superblock(&fs->superblock);
        debug_printf("[Integrity] fresh map: %u blocks at %u (covers %u blocks)\n",
                     g_map_count, start, total);
    } else {
        g_map_first_block = first;
        uint32_t stored = SB_MAP_COUNT(&fs->superblock);
        uint32_t load   = stored < g_map_count ? stored : g_map_count;
        uint8_t buf[TAGFS_BLOCK_SIZE];
        for (uint32_t i = 0; i < load; i++) {
            if (tagfs_read_block(first + i, buf) != OK)
                break;
            uint32_t base = i * ENTRIES_PER_BLOCK;
            uint32_t n    = (base + ENTRIES_PER_BLOCK <= total) ? ENTRIES_PER_BLOCK : (total - base);
            memcpy(g_map + base, buf, n * sizeof(uint64_t));
        }
        debug_printf("[Integrity] map loaded: %u blocks at %u (covers %u blocks)\n",
                     g_map_count, first, total);
    }

    g_initialized = true;   // hooks were no-ops until now (so Init's own I/O is safe)
    return OK;
}

void IntegrityUpdate(uint32_t block, const void *data) {
    if (!g_initialized || !data || block >= g_map_entries || in_map_region(block))
        return;
    uint64_t h = BoxHashIntegrity(data, TAGFS_BLOCK_SIZE, &g_ctx);
    spin_lock(&g_lock);
    if (g_map[block] != h) {
        g_map[block] = h;
        g_dirty[block / ENTRIES_PER_BLOCK] = true;
    }
    spin_unlock(&g_lock);
}

bool IntegrityVerify(uint32_t block, const void *data) {
    if (!g_initialized || !data || block >= g_map_entries || in_map_region(block))
        return true;
    spin_lock(&g_lock);
    uint64_t stored = g_map[block];
    spin_unlock(&g_lock);
    if (stored == 0)
        return true;   // unknown — never false-positive pre-existing data
    uint64_t h = BoxHashIntegrity(data, TAGFS_BLOCK_SIZE, &g_ctx);
    if (h == stored)
        return true;
    __atomic_fetch_add(&g_errors, 1, __ATOMIC_RELAXED);
    debug_printf("[Integrity] BIT-ROT on block %u: stored=%016lx read=%016lx\n",
                 block, (unsigned long)stored, (unsigned long)h);
    return false;
}

error_t IntegrityFlush(void) {
    if (!g_initialized)
        return OK;
    spin_lock(&g_lock);
    for (uint32_t i = 0; i < g_map_count; i++) {
        if (!g_dirty[i])
            continue;
        uint8_t buf[TAGFS_BLOCK_SIZE];
        memset(buf, 0, sizeof(buf));
        uint32_t base = i * ENTRIES_PER_BLOCK;
        uint32_t n    = (base + ENTRIES_PER_BLOCK <= g_map_entries)
                            ? ENTRIES_PER_BLOCK : (g_map_entries - base);
        memcpy(buf, g_map + base, n * sizeof(uint64_t));
        // Writing a map block re-enters IntegrityUpdate, but in_map_region()
        // skips it before taking g_lock — no recursion, no deadlock.
        if (tagfs_write_block(g_map_first_block + i, buf) == OK)
            g_dirty[i] = false;
    }
    spin_unlock(&g_lock);
    return OK;
}

void IntegrityShutdown(void) {
    if (!g_initialized)
        return;
    IntegrityFlush();
    spin_lock(&g_lock);
    g_initialized = false;
    if (g_map)   { kfree(g_map);   g_map = NULL; }
    if (g_dirty) { kfree(g_dirty); g_dirty = NULL; }
    g_map_entries = g_map_count = g_map_first_block = 0;
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
    uint32_t first = SB_MAP_BLOCK(&fs->superblock);
    uint32_t cnt   = SB_MAP_COUNT(&fs->superblock);
    if (first == 0)
        return;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t b = first + i;
        if (b < total_blocks)
            computed_bm[b / 8] |= (uint8_t)(1u << (b % 8));
    }
}
