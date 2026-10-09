#include "block_map_internal.h"
#include "atomics.h"
#include "cpu_calibrate.h"

/*
 * The block map as the stand weighs it (CONFIG_VOLUME_TESTS): what a plan and
 * an edit cost under the state lock, and a plan made on a volume that lives in
 * memory alone. The stand's own medium holds a handful of windows; the cost
 * that must not grow with the volume is asked of tens of thousands.
 */

#ifdef CONFIG_VOLUME_TESTS

#define MAP_APART_FAR_WHOLE 20u     /* SCATTERED: whole free windows at the volume's end */
#define MAP_APART_RUN       15u     /* RUNS: whole free windows in each run */

uint32_t BlockMapHoldMost(void)
{
    TagFSLockTake(MapLock());
    uint32_t most   = g_map.hold_most;
    g_map.hold_most = 0;
    TagFSLockDrop(MapLock());
    return most;
}

uint32_t BlockMapEditMost(void)
{
    TagFSLockTake(MapLock());
    uint32_t most   = g_map.edit_most;
    g_map.edit_most = 0;
    TagFSLockDrop(MapLock());
    return most;
}

/* A hold stops before a step once MAP_PLAN_STEPS are taken, so it takes at
 * most one less, and then the dearest thing it may begin: a window, with the
 * run past it walked as far as the plan measures runs — or a best fit's list,
 * each window of it weighed from as far back as that measure, itself and the
 * window before it. */
uint32_t BlockMapStandHoldBound(uint32_t want)
{
    uint64_t reach  = (uint64_t)want + MAP_WINDOW_BLOCKS;
    uint64_t walk   = (reach + MAP_WINDOW_BLOCKS - 1u) / MAP_WINDOW_BLOCKS;
    uint64_t window = walk + 2u;
    uint64_t list   = MAP_FIT_LOOKS * (walk + 2u * window);
    return (uint32_t)(MAP_PLAN_STEPS - 1u + (list > window ? list : window));
}

static void MapApartLay(BlockMapWindow *win, uint32_t windows, BlockMapApartShape shape)
{
    memset(win, 0, sizeof(BlockMapWindow) * windows);
    for (uint32_t w = 0; w < windows; w++) {
        BlockMapWindow *x     = &win[w];
        bool            whole = shape == BLOCK_MAP_APART_SCATTERED
                                    ? w + MAP_APART_FAR_WHOLE >= windows
                                    : w % (MAP_APART_RUN + 1u) != MAP_APART_RUN;
        x->flags = MAP_W_EXACT;
        if (whole) {
            x->free = x->longest = x->head = x->tail = MAP_WINDOW_BLOCKS;
        } else if (shape == BLOCK_MAP_APART_SCATTERED) {
            x->free    = 64;        /* a run of 40 and one of 24 */
            x->longest = 40;
            x->at      = 1000;
        }
    }
}

/* The plan weighed hold by hold, with the lock kept: nothing reaches the map
 * set in place of the volume's while it stands there. */
static void MapApartWeigh(MapPick *k, BlockMapApartPlan *out)
{
    do {
        uint64_t began = rdtsc();
        MapPlan(k, false, true);
        uint64_t us = cpu_tsc_to_us(rdtsc() - began);
        out->holds++;
        out->steps += k->steps;
        if (k->steps > out->hold_most)
            out->hold_most = k->steps;
        if (us > out->hold_most_us)
            out->hold_most_us = us;
    } while (k->paused);
    out->found = k->found;
    out->at    = k->at;
    out->got   = k->got;
}

bool BlockMapStandPlanApart(uint32_t windows, BlockMapApartShape shape, bool best_fit,
                            uint32_t want, BlockMapApartPlan *out)
{
    memset(out, 0, sizeof(*out));
    if (windows == 0 || windows >= MAP_NO_WINDOW)
        return false;
    BlockMapWindow *win = kmalloc(sizeof(BlockMapWindow) * windows);
    if (!win)
        return false;
    MapApartLay(win, windows, shape);

    TagFSLockTake(MapLock());
    if (!g_map.up) {
        TagFSLockDrop(MapLock());
        kfree(win);
        return false;
    }
    BlockMapWindow *own_win     = g_map.win;
    uint32_t        own_windows = g_map.windows;
    uint64_t        own_blocks  = g_map.blocks;
    uint32_t        own_room    = g_map.first_room;
    uint32_t        own_most    = g_map.hold_most;
    uint32_t        own_fit[MAP_FIT_LISTS];
    memcpy(own_fit, g_map.fit, sizeof(own_fit));

    g_map.win        = win;
    g_map.windows    = windows;
    g_map.blocks     = (uint64_t)windows * MAP_WINDOW_BLOCKS;
    g_map.first_room = 0;
    memset(g_map.fit, 0, sizeof(g_map.fit));
    for (uint32_t w = 0; w < windows; w++)
        MapFitMoved(w, 0);

    MapPick k = { .want = want, .hint = 0, .best_fit = best_fit };
    MapApartWeigh(&k, out);

    g_map.win        = own_win;
    g_map.windows    = own_windows;
    g_map.blocks     = own_blocks;
    g_map.first_room = own_room;
    g_map.hold_most  = own_most;
    memcpy(g_map.fit, own_fit, sizeof(own_fit));
    TagFSLockDrop(MapLock());
    kfree(win);
    return true;
}

#endif /* CONFIG_VOLUME_TESTS */
