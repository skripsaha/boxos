#include "block_map_internal.h"

/*
 * The windows, the edits and the deltas: everything the block map decides and
 * changes, all of it in memory and under the volume's state lock. The cache
 * that brings sectors in and sends them out is block_map_cache.c; the mount's
 * pass is block_map_mount.c; where a take goes is block_map_take.c.
 */

BlockMapState g_map = { .work_lo = 1, .work_hi = 0 };

TagFSLock *MapLock(void)
{
    return &tagfs_get_state()->lock;
}

BlockMapPage *MapReady(uint32_t w)
{
    uint8_t slot = g_map.win[w].slot;
    if (!slot)
        return NULL;
    BlockMapPage *p = &g_map.page[slot - 1];
    return p->state == MAP_PAGE_READY ? p : NULL;
}

// ----------------------------------------------------------------------------
// What a window holds
// ----------------------------------------------------------------------------

void MapViewOf(uint32_t w, const uint8_t *bits, uint8_t *view)
{
    memcpy(view, bits, TAGFS_SECTOR_SIZE);

    uint64_t base = (uint64_t)w * MAP_WINDOW_BLOCKS;
    if (base + MAP_WINDOW_BLOCKS > g_map.blocks) {
        uint32_t inside = g_map.blocks > base ? (uint32_t)(g_map.blocks - base) : 0;
        for (uint32_t b = inside; b < MAP_WINDOW_BLOCKS; b++)
            view[b / 8] |= (uint8_t)(1u << (b % 8));
    }
    AllotmentOverlay(w, view);
}

/* A run of `len` clear bits from `at` has ended: where it stands among the
 * window's runs is recorded. */
static void MapRunClosed(BlockMapWindow *x, uint32_t at, uint32_t len)
{
    if (at == 0)
        x->head = (uint16_t)len;
    if (len > x->longest) {
        x->longest = (uint16_t)len;
        x->at      = (uint16_t)at;
    }
}

static void MapMeasureView(const uint8_t *view, BlockMapWindow *x)
{
    uint32_t run = 0, run_at = 0;

    x->free = x->longest = x->at = x->head = x->tail = 0;
    for (uint32_t b = 0; b < MAP_WINDOW_BLOCKS; ) {
        uint8_t v = view[b / 8];
        if ((b % 8) == 0 && (v == 0x00 || v == 0xFF)) {
            if (v == 0x00) {
                if (!run)
                    run_at = b;
                run     += 8;
                x->free += 8;
            } else if (run) {
                MapRunClosed(x, run_at, run);
                run = 0;
            }
            b += 8;
            continue;
        }
        if (!(v & (1u << (b % 8)))) {
            if (!run)
                run_at = b;
            run++;
            x->free++;
        } else if (run) {
            MapRunClosed(x, run_at, run);
            run = 0;
        }
        b++;
    }
    if (run) {
        x->tail = (uint16_t)run;
        MapRunClosed(x, run_at, run);
    }
    x->flags |= MAP_W_EXACT;
}

void MapMeasureBits(uint32_t w, const uint8_t *bits)
{
    MapViewOf(w, bits, g_map.view);
    MapMeasureView(g_map.view, &g_map.win[w]);
    if (g_map.win[w].free && w < g_map.first_room)
        g_map.first_room = w;
}

void MapMeasure(uint32_t w)
{
    MapMeasureBits(w, MapReady(w)->bits);
}

/*
 * [off, off + n) stops being blocks a take may have, in a window whose sector
 * is not in memory: the count is exact, and every bound is kept honest — a run
 * the cut went through keeps the longer of its two sides.
 */
static void MapShrink(BlockMapWindow *x, uint32_t off, uint32_t n)
{
    uint32_t end = off + n;

    x->free = (uint16_t)(x->free - n);
    if (x->free == 0) {
        x->longest = x->at = x->head = x->tail = 0;
        x->flags  |= MAP_W_EXACT;
        return;
    }
    if (off < x->head)
        x->head = (uint16_t)off;
    if (end > MAP_WINDOW_BLOCKS - x->tail)
        x->tail = (uint16_t)(MAP_WINDOW_BLOCKS - end);

    uint32_t a = x->at, b = x->at + x->longest;
    if (off < b && end > a) {
        uint32_t left  = off > a ? off - a : 0;
        uint32_t right = end < b ? b - end : 0;
        if (right >= left) {
            x->at      = (uint16_t)end;
            x->longest = (uint16_t)right;
        } else {
            x->longest = (uint16_t)left;
        }
    }
    if (x->head > x->longest) {
        x->longest = x->head;
        x->at      = 0;
    }
    if (x->tail > x->longest) {
        x->longest = x->tail;
        x->at      = (uint16_t)(MAP_WINDOW_BLOCKS - x->tail);
    }
    x->flags &= (uint8_t)~MAP_W_EXACT;
}

/* [off, off + n) becomes blocks a take may have, in a window whose sector is
 * not in memory: what they join is joined, and nothing else is claimed. */
static void MapGrow(uint32_t w, uint32_t off, uint32_t n)
{
    BlockMapWindow *x   = &g_map.win[w];
    uint32_t        end = off + n;

    x->free = (uint16_t)(x->free + n);
    if (w < g_map.first_room)
        g_map.first_room = w;
    if (x->free == MAP_WINDOW_BLOCKS) {
        x->longest = x->head = x->tail = MAP_WINDOW_BLOCKS;
        x->at      = 0;
        x->flags  |= MAP_W_EXACT;
        return;
    }

    uint32_t a = x->at, b = x->at + x->longest;
    if (x->longest && off <= b && end >= a) {
        a = off < a ? off : a;
        b = end > b ? end : b;
    } else if (n > x->longest) {
        a = off;
        b = end;
    }
    x->at      = (uint16_t)a;
    x->longest = (uint16_t)(b - a);
    if (off <= x->head && end > x->head)
        x->head = (uint16_t)end;
    if (end >= MAP_WINDOW_BLOCKS - x->tail && off < MAP_WINDOW_BLOCKS - x->tail)
        x->tail = (uint16_t)(MAP_WINDOW_BLOCKS - off);
    if (x->head > x->longest) {
        x->longest = x->head;
        x->at      = 0;
    }
    if (x->tail > x->longest) {
        x->longest = x->tail;
        x->at      = (uint16_t)(MAP_WINDOW_BLOCKS - x->tail);
    }
    x->flags &= (uint8_t)~MAP_W_EXACT;
}

void MapAvailabilityChanged(uint32_t start, uint32_t count, bool lost)
{
    for (uint32_t b = start, end = start + count; b < end; ) {
        uint32_t w   = b / MAP_WINDOW_BLOCKS;
        uint32_t off = b % MAP_WINDOW_BLOCKS;
        uint32_t n   = end - b < MAP_WINDOW_BLOCKS - off ? end - b : MAP_WINDOW_BLOCKS - off;

        if (MapReady(w))
            MapMeasure(w);
        else if (lost)
            MapShrink(&g_map.win[w], off, n);
        else
            MapGrow(w, off, n);
        b += n;
    }
}

uint32_t MapUnreadWindows(uint32_t start, uint32_t count)
{
    uint32_t unread = 0;
    for (uint32_t w = start / MAP_WINDOW_BLOCKS; w <= (start + count - 1) / MAP_WINDOW_BLOCKS; w++)
        unread += MapReady(w) == NULL;
    return unread;
}

// ----------------------------------------------------------------------------
// Debts and deltas
// ----------------------------------------------------------------------------

void MapWorkAt(uint32_t w)
{
    if (g_map.work_lo > g_map.work_hi) {
        g_map.work_lo = g_map.work_hi = w;
        return;
    }
    if (w < g_map.work_lo) g_map.work_lo = w;
    if (w > g_map.work_hi) g_map.work_hi = w;
}

void MapOwedMark(uint32_t w)
{
    if (g_map.win[w].flags & MAP_W_OWED)
        return;
    g_map.win[w].flags |= MAP_W_OWED;
    g_map.owed++;
    MapWorkAt(w);
}

bool MapDeltaRoom(uint32_t more)
{
    if (g_map.deltas + more <= g_map.delta_cap)
        return true;

    uint32_t cap = g_map.delta_cap ? g_map.delta_cap * 2 : 16;
    while (cap < g_map.deltas + more)
        cap *= 2;
    BlockMapDelta *grown = kmalloc(sizeof(BlockMapDelta) * cap);
    if (!grown)
        return false;
    if (g_map.deltas)
        memcpy(grown, g_map.delta, sizeof(BlockMapDelta) * g_map.deltas);
    kfree(g_map.delta);
    g_map.delta     = grown;
    g_map.delta_cap = cap;
    return true;
}

void MapDeltaAdd(uint32_t start, uint32_t count, bool take)
{
    uint32_t w = start / MAP_WINDOW_BLOCKS;

    g_map.delta[g_map.deltas].start = start;
    g_map.delta[g_map.deltas].count = count;
    g_map.delta[g_map.deltas].take  = take;
    g_map.deltas++;
    if (!take)
        g_map.gives++;
    g_map.win[w].flags |= MAP_W_DELTAS;
    MapWorkAt(w);
}

static void MapDeltaDrop(uint32_t i)
{
    if (!g_map.delta[i].take)
        g_map.gives--;
    memmove(&g_map.delta[i], &g_map.delta[i + 1], sizeof(BlockMapDelta) * (g_map.deltas - i - 1));
    g_map.deltas--;
}

static bool MapDeltaIn(uint32_t w)
{
    for (uint32_t i = 0; i < g_map.deltas; i++)
        if (g_map.delta[i].start / MAP_WINDOW_BLOCKS == w)
            return true;
    return false;
}

/* A give of exactly the run a take written down without its sector took is
 * that take undone, not a second edit: the run was never taken on any sector. */
static bool MapDeltaUndoTake(uint32_t start, uint32_t count)
{
    for (uint32_t i = g_map.deltas; i-- > 0; ) {
        BlockMapDelta *d = &g_map.delta[i];
        if (d->start / MAP_WINDOW_BLOCKS != start / MAP_WINDOW_BLOCKS)
            continue;
        if (d->take && d->start == start && d->count == count) {
            MapDeltaDrop(i);
            if (!MapDeltaIn(start / MAP_WINDOW_BLOCKS))
                g_map.win[start / MAP_WINDOW_BLOCKS].flags &= (uint8_t)~MAP_W_DELTAS;
            return true;
        }
        return false;       /* a later edit of the window stands between */
    }
    return false;
}

/* Set [off, off + n) on the sector. A bit already set means the block was
 * somebody's while memory counted it free: it is said, with the block, and the
 * free count, which counted it twice, is put right. */
static void MapLayTake(BlockMapPage *p, uint32_t off, uint32_t n)
{
    uint32_t base  = p->window * MAP_WINDOW_BLOCKS;
    uint32_t taken = 0, first = 0;

    for (uint32_t i = off; i < off + n; i++) {
        uint8_t bit = (uint8_t)(1u << (i % 8));
        if (p->bits[i / 8] & bit) {
            if (!taken++)
                first = base + i;
        }
        p->bits[i / 8] |= bit;
    }
    p->dirty = 1;
    if (taken) {
        tagfs_get_state()->ledger.free_blocks += taken;
        kprintf("[TagFS] block map: a take of blocks %u..%u met %u of them already taken "
                "on the medium, the first block %u — memory had counted them free, so "
                "they may have two owners now\n",
                base + off, base + off + n - 1, taken, first);
    }
}

/* The first block of [off, off + n) that is clear on the sector, or `n` past
 * the end when every one is taken. */
static uint32_t MapFirstClear(const BlockMapPage *p, uint32_t off, uint32_t n)
{
    for (uint32_t i = off; i < off + n; i++)
        if (!MapBit(p->bits, i))
            return i;
    return off + n;
}

static void MapSayFreeTwice(uint32_t start, uint32_t count, uint32_t block)
{
    kprintf("[TagFS] block map: blocks %u..%u are given back and block %u among them "
            "is already free — the give is refused whole and the map is left as it "
            "is, because a block freed twice is a block handed to two owners\n",
            start, start + count - 1, block);
}

/* Clear [off, off + n) on the sector, or refuse it whole when a block of it
 * is already clear. */
static bool MapLayGive(BlockMapPage *p, uint32_t off, uint32_t n)
{
    uint32_t base  = p->window * MAP_WINDOW_BLOCKS;
    uint32_t clear = MapFirstClear(p, off, n);

    if (clear < off + n) {
        MapSayFreeTwice(base + off, n, base + clear);
        return false;
    }
    for (uint32_t i = off; i < off + n; i++)
        p->bits[i / 8] &= (uint8_t)~(1u << (i % 8));
    p->dirty = 1;
    tagfs_get_state()->ledger.free_blocks += n;
    return true;
}

/* Every delta of the window laid onto its sector, in the order made. */
void MapDeltaApply(uint32_t w, BlockMapPage *p)
{
    if (!(g_map.win[w].flags & MAP_W_DELTAS))
        return;

    uint32_t base = w * MAP_WINDOW_BLOCKS;
    for (uint32_t i = 0; i < g_map.deltas; ) {
        BlockMapDelta d = g_map.delta[i];
        if (d.start / MAP_WINDOW_BLOCKS != w) {
            i++;
            continue;
        }
        MapDeltaDrop(i);
        if (d.take)
            MapLayTake(p, d.start - base, d.count);
        else
            MapLayGive(p, d.start - base, d.count);
    }
    g_map.win[w].flags &= (uint8_t)~MAP_W_DELTAS;
}

// ----------------------------------------------------------------------------
// Taking and giving back, in memory
// ----------------------------------------------------------------------------

static void MapTakeEach(uint32_t start, uint32_t count, bool kept)
{
    for (uint32_t b = start, end = start + count; b < end; ) {
        uint32_t      w   = b / MAP_WINDOW_BLOCKS;
        uint32_t      off = b % MAP_WINDOW_BLOCKS;
        uint32_t      n   = end - b < MAP_WINDOW_BLOCKS - off ? end - b : MAP_WINDOW_BLOCKS - off;
        BlockMapPage *p   = MapReady(w);

        if (p) {
            MapLayTake(p, off, n);
            MapMeasure(w);
        } else {
            MapDeltaAdd(b, n, true);
            if (!kept)
                MapShrink(&g_map.win[w], off, n);
        }
        MapOwedMark(w);
        b += n;
    }

    TagFSState *fs = tagfs_get_state();
    if (fs->ledger.free_blocks >= count) {
        fs->ledger.free_blocks -= count;
    } else {
        kprintf("[TagFS] block map: %u block(s) are taken and the free count says %llu — "
                "it is held at 0, and the next mount derives it from the map again\n",
                count, (unsigned long long)fs->ledger.free_blocks);
        fs->ledger.free_blocks = 0;
    }
    g_map.no_room_said = false;
}

void MapTakeKnown(uint32_t start, uint32_t count)
{
    MapTakeEach(start, count, false);
}

/* An Allotment's own blocks were never ones a take may have, so taking them
 * changes no summary of a window whose sector is away. */
void MapTakeKept(uint32_t start, uint32_t count)
{
    MapTakeEach(start, count, true);
}

error_t BlockMapGiveLocked(uint32_t start, uint32_t count)
{
    if (count == 0)
        return OK;
    if (!g_map.up)
        return ERR_TAGFS_NOT_INITIALIZED;
    if (start >= g_map.blocks || count > g_map.blocks - start) {
        kprintf("[TagFS] block map: blocks %u..+%u are given back and the data run ends "
                "at %u — nothing is changed\n", start, count, g_map.blocks);
        return ERR_INVALID_ARGUMENT;
    }

    /* Judged whole first, wherever the sector is in memory: a give that meets
     * a block already free changes nothing anywhere. */
    uint32_t away = 0;
    for (uint32_t b = start, end = start + count; b < end; ) {
        uint32_t      w   = b / MAP_WINDOW_BLOCKS;
        uint32_t      off = b % MAP_WINDOW_BLOCKS;
        uint32_t      n   = end - b < MAP_WINDOW_BLOCKS - off ? end - b : MAP_WINDOW_BLOCKS - off;
        BlockMapPage *p   = MapReady(w);

        if (p) {
            uint32_t clear = MapFirstClear(p, off, n);
            if (clear < off + n) {
                MapSayFreeTwice(start, count, w * MAP_WINDOW_BLOCKS + clear);
                return ERR_TAGFS_EXTENT_ERROR;
            }
        } else {
            away++;
        }
        b += n;
    }
    if (!MapDeltaRoom(away)) {
        kprintf("[TagFS] block map: no memory to write down the give back of blocks "
                "%u..%u — they stay taken, and the next mount's roll call finds them "
                "owned by nothing\n", start, start + count - 1);
        return ERR_NO_MEMORY;
    }

    /* Where the sector is away the give is judged when it comes in. */
    for (uint32_t b = start, end = start + count; b < end; ) {
        uint32_t      w   = b / MAP_WINDOW_BLOCKS;
        uint32_t      off = b % MAP_WINDOW_BLOCKS;
        uint32_t      n   = end - b < MAP_WINDOW_BLOCKS - off ? end - b : MAP_WINDOW_BLOCKS - off;
        BlockMapPage *p   = MapReady(w);

        if (p) {
            MapLayGive(p, off, n);
            MapMeasure(w);
        } else if (MapDeltaUndoTake(b, n)) {
            tagfs_get_state()->ledger.free_blocks += n;
            MapGrow(w, off, n);
        } else {
            MapDeltaAdd(b, n, false);
        }
        b += n;
    }
    return OK;
}

// ----------------------------------------------------------------------------
// For the startup tests
// ----------------------------------------------------------------------------

#ifdef CONFIG_VOLUME_TESTS
bool BlockMapIsTaken(uint32_t block)
{
    if (!g_map.up || block >= g_map.blocks)
        return false;

    uint32_t w = block / MAP_WINDOW_BLOCKS;
    (void)MapFetch(w, __func__);

    TagFSLockTake(MapLock());
    BlockMapPage *p     = MapReady(w);
    bool          taken = !p || MapBit(p->bits, block % MAP_WINDOW_BLOCKS);
    TagFSLockDrop(MapLock());
    return taken;
}

/*
 * The free count and the windows, derived against each other — the count a
 * mount would take from the map, the blocks growing files keep counted in, as
 * they are free on the medium. For a startup test that has watched a block be
 * given back twice: a refused give leaves nothing to drift.
 */
int64_t TagFSAllocatorRederive(void)
{
    TagFSState *fs = tagfs_get_state();

    TagFSLockTake(&fs->lock);
    uint64_t derived = AllotmentKeptLocked();
    for (uint32_t w = 0; w < g_map.windows; w++)
        derived += g_map.win[w].free;
    int64_t drift = (int64_t)fs->ledger.free_blocks - (int64_t)derived;
    fs->ledger.free_blocks = derived;
    TagFSLockDrop(&fs->lock);
    return drift;
}
#endif
