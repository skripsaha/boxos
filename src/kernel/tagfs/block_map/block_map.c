#include "block_map_internal.h"
#ifdef CONFIG_VOLUME_TESTS
#include "amp.h"           /* amp_get_core_index — the stand's HEAPFAIL on the holder's core */
#endif

/*
 * The windows, the edits and the deltas: everything the block map decides and
 * changes, all of it in memory and under the volume's state lock. The cache
 * that brings sectors in and sends them out is block_map_cache.c; the mount's
 * pass is block_map_mount.c; where a take goes is block_map_take.c.
 */

BlockMapState g_map;

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
// The lists of windows by the longest run they hold
// ----------------------------------------------------------------------------

/* The list a window whose longest run is `longest` sits in, + 1; 0 for none. */
static uint32_t MapFitList(uint32_t longest)
{
    return longest ? 32u - (uint32_t)__builtin_clz(longest) : 0u;
}

static void MapFitUnlink(uint32_t w, uint32_t list)
{
    BlockMapWindow *x = &g_map.win[w];
    if (x->fit_prev)
        g_map.win[x->fit_prev - 1].fit_next = x->fit_next;
    else
        g_map.fit[list - 1] = x->fit_next;
    if (x->fit_next)
        g_map.win[x->fit_next - 1].fit_prev = x->fit_prev;
    x->fit_prev = x->fit_next = 0;
}

static void MapFitLink(uint32_t w, uint32_t list)
{
    BlockMapWindow *x = &g_map.win[w];
    x->fit_prev = 0;
    x->fit_next = g_map.fit[list - 1];
    if (x->fit_next)
        g_map.win[x->fit_next - 1].fit_prev = w + 1;
    g_map.fit[list - 1] = w + 1;
}

void MapFitMoved(uint32_t w, uint32_t was)
{
    uint32_t from = MapFitList(was), to = MapFitList(g_map.win[w].longest);
    if (from == to)
        return;
    if (from)
        MapFitUnlink(w, from);
    if (to)
        MapFitLink(w, to);
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
    uint32_t was = g_map.win[w].longest;
    MapViewOf(w, bits, g_map.view);
    MapMeasureView(g_map.view, &g_map.win[w]);
    MapFitMoved(w, was);
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
static void MapShrinkSummary(BlockMapWindow *x, uint32_t off, uint32_t n)
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

static void MapShrink(uint32_t w, uint32_t off, uint32_t n)
{
    uint32_t was = g_map.win[w].longest;
    MapShrinkSummary(&g_map.win[w], off, n);
    MapFitMoved(w, was);
}

/* [off, off + n) becomes blocks a take may have, in a window whose sector is
 * not in memory: what they join is joined, and nothing else is claimed. */
static void MapGrowSummary(uint32_t w, uint32_t off, uint32_t n)
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

static void MapGrow(uint32_t w, uint32_t off, uint32_t n)
{
    uint32_t was = g_map.win[w].longest;
    MapGrowSummary(w, off, n);
    MapFitMoved(w, was);
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
            MapShrink(w, off, n);
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
    uint32_t word = w / 64u;
    g_map.work[word]             |= 1ull << (w % 64u);
    g_map.work_words[word / 64u] |= 1ull << (word % 64u);
}

void MapWorkDone(uint32_t w)
{
    uint32_t word = w / 64u;
    g_map.work[word] &= ~(1ull << (w % 64u));
    if (!g_map.work[word])
        g_map.work_words[word / 64u] &= ~(1ull << (word % 64u));
}

uint32_t MapWorkNext(uint32_t w)
{
    uint32_t words = (g_map.windows + 63u) / 64u;
    if (w >= g_map.windows)
        return g_map.windows;

    uint32_t word = w / 64u;
    uint64_t bits = g_map.work[word] & (~0ull << (w % 64u));
    if (bits)
        return word * 64u + (uint32_t)__builtin_ctzll(bits);
    for (uint32_t next = word + 1u; next < words; next = (next / 64u + 1u) * 64u) {
        uint64_t marks = g_map.work_words[next / 64u] & (~0ull << (next % 64u));
        if (marks) {
            uint32_t found = (next / 64u) * 64u + (uint32_t)__builtin_ctzll(marks);
            return found * 64u + (uint32_t)__builtin_ctzll(g_map.work[found]);
        }
    }
    return g_map.windows;
}

void MapOwedMark(uint32_t w)
{
    if (g_map.win[w].flags & MAP_W_OWED)
        return;
    g_map.win[w].flags |= MAP_W_OWED;
    g_map.owed++;
    MapWorkAt(w);
}

#ifdef CONFIG_VOLUME_TESTS
static volatile uint8_t s_stand_room_refused;

void BlockMapGiveRoomRefused(bool refused)
{
    __atomic_store_n(&s_stand_room_refused, (uint8_t)refused, __ATOMIC_RELEASE);
}
#endif

/* The capacity the delta array grows to for `need` deltas; 0 for a need no
 * array of deltas can be sized to. */
static uint32_t MapDeltaCapFor(uint64_t need)
{
    uint64_t cap = g_map.delta_cap ? (uint64_t)g_map.delta_cap * 2u : 16u;
    while (cap < need)
        cap *= 2u;
    return cap <= UINT32_MAX / sizeof(BlockMapDelta) ? (uint32_t)cap : 0u;
}

/* The pool moved into `grown`, of `cap`, every slot where it was — the chains
 * name slots: the array it replaces back. */
static BlockMapDelta *MapDeltaSwap(BlockMapDelta *grown, uint32_t cap)
{
    BlockMapDelta *old = g_map.delta;
    if (g_map.delta_high)
        memcpy(grown, g_map.delta, sizeof(BlockMapDelta) * g_map.delta_high);
    g_map.delta     = grown;
    g_map.delta_cap = cap;
    return old;
}

bool MapDeltaRoom(uint32_t more, bool give, bool heap)
{
    uint64_t bare = (uint64_t)g_map.deltas + more;
    if (bare + g_map.delta_held <= g_map.delta_cap)
        return true;

    if (heap) {
        uint32_t       cap   = MapDeltaCapFor(bare + g_map.delta_held);
        BlockMapDelta *grown = cap ? kmalloc(sizeof(BlockMapDelta) * cap) : NULL;
        if (grown) {
            kfree(MapDeltaSwap(grown, cap));
            return true;
        }
    }
    /* A give the heap would not serve draws on the room held for Fallow's:
     * what that leaves short rests on in Fallow a cycle more, where a give
     * refused here would stay taken for good. A take never does. */
    return give && bare <= g_map.delta_cap;
}

/* The room held is what the deltas leave of the array, at most. */
static void MapDeltaHeldFits(void)
{
    if ((uint64_t)g_map.deltas + g_map.delta_held > g_map.delta_cap)
        g_map.delta_held = g_map.delta_cap - g_map.deltas;
}

error_t BlockMapGiveRoomHold(uint32_t gives)
{
    TagFSLockTake(MapLock());
    if (!g_map.up) {
        TagFSLockDrop(MapLock());
        return OK;
    }
    /* No more deltas than blocks: every one holds a block of its own. */
    g_map.delta_held = gives < g_map.blocks ? gives : g_map.blocks;
    uint64_t need = (uint64_t)g_map.deltas + g_map.delta_held;
    if (need <= g_map.delta_cap) {
        TagFSLockDrop(MapLock());
        return OK;
    }
    uint32_t cap = MapDeltaCapFor(need);
    TagFSLockDrop(MapLock());

    BlockMapDelta *grown = NULL;
    if (cap) {
#ifdef CONFIG_VOLUME_TESTS
        if (__atomic_exchange_n(&s_stand_room_refused, 0u, __ATOMIC_ACQ_REL))
            HeapFailArm(amp_get_core_index(), 1);
#endif
        grown = kmalloc(sizeof(BlockMapDelta) * cap);
    }

    /* Deltas written down meanwhile may have outgrown it, or the heap gave
     * nothing: the room held is then what there is, so a take is held to no
     * room it was not held to before, and the gives draw on the rest. */
    TagFSLockTake(MapLock());
    if (grown && g_map.up && cap > g_map.delta_cap && g_map.delta_high <= cap)
        grown = MapDeltaSwap(grown, cap);
    bool held = (uint64_t)g_map.deltas + g_map.delta_held <= g_map.delta_cap;
    MapDeltaHeldFits();
    TagFSLockDrop(MapLock());
    kfree(grown);
    return held ? OK : ERR_NO_MEMORY;
}

bool BlockMapGivesUnseenLocked(void)
{
    return g_map.gives != 0;
}

/* An edit looked at `looked` deltas: counted for the stand, which holds it
 * to what the edit's own window has. */
static inline void MapEditLooked(uint32_t looked)
{
#ifdef CONFIG_VOLUME_TESTS
    if (looked > g_map.edit_most)
        g_map.edit_most = looked;
#else
    (void)looked;
#endif
}

/* A slot of the pool for one delta more; room was made first, so the free
 * ones or the ground past delta_high hold one. */
static uint32_t MapDeltaSlot(void)
{
    if (g_map.delta_free) {
        uint32_t i = g_map.delta_free - 1u;
        g_map.delta_free = g_map.delta[i].older;
        return i;
    }
    return g_map.delta_high++;
}

void MapDeltaAdd(uint32_t start, uint32_t count, bool take)
{
    uint32_t        w = start / MAP_WINDOW_BLOCKS;
    BlockMapWindow *x = &g_map.win[w];
    uint32_t        i = MapDeltaSlot();

    g_map.delta[i].start = start;
    g_map.delta[i].count = count;
    g_map.delta[i].take  = take;
    g_map.delta[i].older = x->delta_last;
    x->delta_last        = i + 1u;
    g_map.deltas++;
    if (!take)
        g_map.gives++;
    x->flags |= MAP_W_DELTAS;
    MapWorkAt(w);
}

/* Slot i, out of its window's chain already, back with the free ones. */
static void MapDeltaLetGo(uint32_t i)
{
    if (!g_map.delta[i].take)
        g_map.gives--;
    g_map.delta[i].older = g_map.delta_free;
    g_map.delta_free     = i + 1u;
    g_map.deltas--;
}

/* A give of exactly the run a take written down without its sector took is
 * that take undone, not a second edit: the run was never taken on any sector.
 * Only the window's newest delta can be that take — a later edit of the window
 * would stand between. */
static bool MapDeltaUndoTake(uint32_t start, uint32_t count)
{
    BlockMapWindow *x = &g_map.win[start / MAP_WINDOW_BLOCKS];
    if (!x->delta_last)
        return false;

    uint32_t       i = x->delta_last - 1u;
    BlockMapDelta *d = &g_map.delta[i];
    MapEditLooked(1u);
    if (!d->take || d->start != start || d->count != count)
        return false;
    x->delta_last = d->older;
    MapDeltaLetGo(i);
    if (!x->delta_last)
        x->flags &= (uint8_t)~MAP_W_DELTAS;
    return true;
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

/* Every delta of the window laid onto its sector, in the order made: its
 * chain, newest first, turned round, and each slot let go as it is laid. */
void MapDeltaApply(uint32_t w, BlockMapPage *p)
{
    BlockMapWindow *x = &g_map.win[w];
    if (!(x->flags & MAP_W_DELTAS))
        return;

    uint32_t oldest = 0, looked = 0;
    for (uint32_t at = x->delta_last; at; looked++) {
        uint32_t older = g_map.delta[at - 1u].older;
        g_map.delta[at - 1u].older = oldest;
        oldest = at;
        at     = older;
    }
    MapEditLooked(looked);
    x->delta_last = 0;
    x->flags     &= (uint8_t)~MAP_W_DELTAS;

    uint32_t base = w * MAP_WINDOW_BLOCKS;
    for (uint32_t at = oldest; at; ) {
        BlockMapDelta d = g_map.delta[at - 1u];
        MapDeltaLetGo(at - 1u);
        if (d.take)
            MapLayTake(p, d.start - base, d.count);
        else
            MapLayGive(p, d.start - base, d.count);
        at = d.older;
    }
}

// ----------------------------------------------------------------------------
// Runs no settled record names yet
//
// ‼ A WRITE'S GROUND REACHES THE MEDIUM WITH ITS RECORD, NOT BEFORE. A cycle
// writes the map at S1 as memory has it, and memory has a write's run taken
// from the moment of the take — while the record that names it may be inked
// only after the cycle's snapshot, or never. A map that went out with the run
// taken would leave the cycle's seal with ground that nothing owns. So the
// run is left free in every image sent until the S1 of the cycle whose
// snapshot holds the Ink that named it, which owes its sector and writes it
// taken; a run given back meanwhile was never on the medium at all.
// ----------------------------------------------------------------------------

static bool MapUnsettledGrow(uint32_t need)
{
    if (need <= g_map.unsettled_cap)
        return true;
    uint32_t      cap   = g_map.unsettled_cap ? g_map.unsettled_cap * 2 : 16;
    while (cap < need)
        cap *= 2;
    MapUnsettled *grown = kmalloc(sizeof(MapUnsettled) * cap);
    if (!grown)
        return false;
    if (g_map.unsettled_count)
        memcpy(grown, g_map.unsettled, sizeof(MapUnsettled) * g_map.unsettled_count);
    kfree(g_map.unsettled);
    g_map.unsettled     = grown;
    g_map.unsettled_cap = cap;
    return true;
}

bool MapUnsettledRoom(void)
{
    return MapUnsettledGrow(g_map.unsettled_count + 2u);
}

void MapUnsettledAdd(uint32_t fid, uint32_t start, uint32_t count)
{
    MapUnsettled *u = &g_map.unsettled[g_map.unsettled_count++];
    u->start = start;
    u->count = count;
    u->fid   = fid;
    u->gen   = 0;
}

static void MapUnsettledDrop(uint32_t i)
{
    g_map.unsettled[i] = g_map.unsettled[--g_map.unsettled_count];
}

/* [start, start + count) given back: no run holds it any more. A run the give
 * cuts in two keeps both sides — held whole instead when there is no room for
 * the second, or the give may not ask the heap (`heap`), which only leaves out
 * of the image a block that is free. */
static void MapUnsettledGiven(uint32_t start, uint32_t count, bool heap)
{
    uint64_t end = (uint64_t)start + count;

    for (uint32_t i = 0; i < g_map.unsettled_count; ) {
        MapUnsettled *u  = &g_map.unsettled[i];
        uint64_t      ue = (uint64_t)u->start + u->count;
        if (ue <= start || u->start >= end) {
            i++;
            continue;
        }
        if (u->start >= start && ue <= end) {
            MapUnsettledDrop(i);
            continue;
        }
        if (u->start < start && ue > end) {
            if (g_map.unsettled_count == g_map.unsettled_cap &&
                (!heap || !MapUnsettledGrow(g_map.unsettled_count + 1u))) {
                i++;
                continue;
            }
            u = &g_map.unsettled[i];
            MapUnsettled *tail = &g_map.unsettled[g_map.unsettled_count++];
            *tail        = *u;
            tail->start  = (uint32_t)end;
            tail->count  = (uint32_t)(ue - end);
            u->count     = start - u->start;
        } else if (u->start < start) {
            u->count = start - u->start;
        } else {
            u->count = (uint32_t)(ue - end);
            u->start = (uint32_t)end;
        }
        i++;
    }
}

uint32_t MapUnsettledLeftOut(uint32_t first, uint32_t count, uint8_t *bits)
{
    uint64_t end  = (uint64_t)first + count;
    uint32_t left = 0;

    for (uint32_t i = 0; i < g_map.unsettled_count; i++) {
        const MapUnsettled *u  = &g_map.unsettled[i];
        uint64_t            a  = u->start > first ? u->start : first;
        uint64_t            ue = (uint64_t)u->start + u->count;
        uint64_t            b  = ue < end ? ue : end;
        for (uint64_t k = a; k < b; k++) {
            uint32_t off = (uint32_t)(k - first);
            bits[off / 8] &= (uint8_t)~(1u << (off % 8));
            left++;
        }
    }
    return left;
}

void MapUnsettledSettled(uint64_t target)
{
    for (uint32_t i = 0; i < g_map.unsettled_count; ) {
        const MapUnsettled *u = &g_map.unsettled[i];
        if (u->gen == 0 || u->gen > target) {
            i++;
            continue;
        }
        uint32_t last = (u->start + u->count - 1u) / MAP_WINDOW_BLOCKS;
        for (uint32_t w = u->start / MAP_WINDOW_BLOCKS; w <= last; w++)
            MapOwedMark(w);
        MapUnsettledDrop(i);
    }
}

void BlockMapInkedLocked(uint32_t fid, uint64_t gen)
{
    for (uint32_t i = 0; i < g_map.unsettled_count; i++)
        if (g_map.unsettled[i].fid == fid && g_map.unsettled[i].gen == 0)
            g_map.unsettled[i].gen = gen;
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
                MapShrink(w, off, n);
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

static error_t MapGive(uint32_t start, uint32_t count, bool heap)
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
    if (!MapDeltaRoom(away, true, heap)) {
        if (heap)
            kprintf("[TagFS] block map: no memory to write down the give back of blocks "
                    "%u..%u — they stay taken, and the next mount's roll call finds them "
                    "owned by nothing\n", start, start + count - 1);
        return ERR_NO_MEMORY;
    }
    MapUnsettledGiven(start, count, heap);

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
    MapDeltaHeldFits();
    return OK;
}

error_t BlockMapGiveLocked(uint32_t start, uint32_t count)
{
    return MapGive(start, count, true);
}

error_t BlockMapGiveHeldLocked(uint32_t start, uint32_t count)
{
    return MapGive(start, count, false);
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
