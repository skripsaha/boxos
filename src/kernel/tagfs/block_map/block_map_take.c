#include "block_map_internal.h"
#include "../ink/fallow.h"

/*
 * Where a take goes.
 *
 * ‼ TWO RULES, AND ONE MEASURE OF NEARNESS. A take of up to `want` blocks near
 * `hint` gets as many as one run can give — `want` wherever a run of `want`
 * lies, the longest there is otherwise — and, among the runs that give that
 * many, the one whose take can start nearest the hint: at the hint itself when
 * it is free and the run from it is long enough, so a file that grows grows in
 * place. A take for an Allotment is best-fit instead: the smallest run that
 * holds the window, so that the long runs stay long — unless the run at the
 * hint holds it, since growth in place is the best fit there is.
 *
 * The windows are asked in the order of their distance from the hint, and the
 * asking stops once no window left could hold a nearer run. A window whose
 * sector is in memory is read run by run; one whose sector is away answers
 * with its summary, whose runs are known to be free even where they are not
 * known to be the longest — so a take decided on it is never wrong, only
 * sometimes not the best. Where the sector could decide better, the window is
 * a LOOK: a door with no lock held fetches it and decides again.
 */

#define MAP_NO_FID      0u       /* a file id no file has: every Allotment is another's */

static void MapConsider(MapPick *k, uint32_t start, uint32_t len)
{
    if (len == 0)
        return;

    uint32_t got  = len < k->want ? len : k->want;
    uint32_t last = start + len - got;                 /* the latest the take may start */
    uint32_t at   = k->hint < start ? start : (k->hint > last ? last : k->hint);
    uint32_t far  = at >= k->hint ? at - k->hint : k->hint - at;
    bool     better;

    if (!k->found)
        better = true;
    else if (got != k->got)
        better = got > k->got;
    else if (k->best_fit && got == k->want && (far == 0) != (k->far == 0))
        better = far == 0;         /* growth in place is the best fit there is */
    else if (k->best_fit && got == k->want && len != k->len)
        better = len < k->len;
    else if (far != k->far)
        better = far < k->far;
    else
        better = at > k->at;       /* the same distance either side: the run after the hint */
    if (!better)
        return;

    k->found = true;
    k->start = start;
    k->len   = len;
    k->at    = at;
    k->got   = got;
    k->far   = far;
}

/* How far a run that reaches the end of a window goes on past it: whole
 * windows of free blocks, then the head of the first that is not whole. Asked
 * no further than `want` needs — and a walk that reached its natural end is
 * kept for the plan, since every window inside it would walk the same ground
 * again, and a take of more than a volume holds would cost the square of its
 * windows. */
static uint32_t MapChainFrom(MapPick *k, uint32_t w)
{
    if (k->chain_len && w >= k->chain_from && w <= k->chain_stop)
        return k->chain_len - (w - k->chain_from) * MAP_WINDOW_BLOCKS;

    uint32_t len = 0, v = w;
    bool     ended = false;         /* the walk found where the run ends */
    while (len < k->want) {
        if (v == g_map.windows) {
            ended = v > w;
            v--;
            break;
        }
        const BlockMapWindow *x = &g_map.win[v];
        if (x->free != MAP_WINDOW_BLOCKS) {
            len  += x->head;
            ended = true;
            break;
        }
        len += MAP_WINDOW_BLOCKS;
        v++;
    }
    if (ended) {
        k->chain_from = w;
        k->chain_stop = v;
        k->chain_len  = len;
    }
    return len;
}

static void MapLook(MapPick *k, uint32_t w)
{
    for (uint32_t i = 0; i < k->looks; i++)
        if (k->look[i] == w)
            return;
    if (k->looks < MAP_LOOKS)
        k->look[k->looks++] = w;
}

/* Every run of a window whose sector is in memory, as a take sees it. */
static void MapConsiderRuns(MapPick *k, uint32_t w, const uint8_t *view)
{
    uint32_t base = w * MAP_WINDOW_BLOCKS;

    for (uint32_t b = 0; b < MAP_WINDOW_BLOCKS; ) {
        if ((b % 8) == 0 && view[b / 8] == 0xFF) {
            b += 8;
            continue;
        }
        if (MapBit(view, b)) {
            b++;
            continue;
        }
        uint32_t a = b;
        while (b < MAP_WINDOW_BLOCKS) {
            if ((b % 8) == 0 && view[b / 8] == 0x00) {
                b += 8;
                continue;
            }
            if (MapBit(view, b))
                break;
            b++;
        }
        uint32_t len = b - a;
        if (b == MAP_WINDOW_BLOCKS)
            len += MapChainFrom(k, w + 1);
        MapConsider(k, base + a, len);
    }
}

static void MapConsiderWindow(MapPick *k, uint32_t w, bool may_read)
{
    const BlockMapWindow *x = &g_map.win[w];
    if (x->free == 0)
        return;

    BlockMapPage *p = MapReady(w);
    if (p) {
        MapViewOf(w, p->bits, g_map.view);
        MapConsiderRuns(k, w, g_map.view);
        return;
    }

    uint32_t base = w * MAP_WINDOW_BLOCKS;
    bool     ends = x->longest && x->at + x->longest == MAP_WINDOW_BLOCKS;
    if (x->longest)
        MapConsider(k, base + x->at, x->longest + (ends ? MapChainFrom(k, w + 1) : 0));
    if (x->head && x->at != 0)
        MapConsider(k, base, x->head);
    if (x->tail && !ends)
        MapConsider(k, base + MAP_WINDOW_BLOCKS - x->tail,
                    x->tail + MapChainFrom(k, w + 1));

    /* The sector would decide better: an exact window holding a run of
     * `want` may hold one nearer than its longest, and a window whose numbers
     * are bounds may hold longer runs than it says. */
    if (may_read && !k->best_fit &&
        ((x->flags & MAP_W_EXACT) ? x->longest >= k->want
                                  : x->free >= (k->found ? (k->got < k->want ? k->got + 1 : k->want) : 1)))
        MapLook(k, w);
}

void MapPlan(MapPick *k, bool may_read)
{
    uint32_t windows = g_map.windows;

    k->found     = false;
    k->looks     = 0;
    k->chain_len = 0;
    while (g_map.first_room < windows && g_map.win[g_map.first_room].free == 0)
        g_map.first_room++;
    uint32_t lo = g_map.first_room;
    if (lo >= windows || k->want == 0)
        return;
    if (k->hint >= g_map.blocks)
        k->hint = g_map.blocks - 1;

    uint32_t h = k->hint / MAP_WINDOW_BLOCKS;
    if (k->best_fit) {
        for (uint32_t w = lo; w < windows; w++)
            MapConsiderWindow(k, w, false);
        if (may_read && h >= lo && !MapReady(h) && g_map.win[h].free && !(k->found && k->far == 0))
            MapLook(k, h);
        return;
    }

    /* Outward from the hint's window: `ahead` is the next window after it,
     * `behind` one past the next window before it (0 when there is none). */
    uint64_t ahead, behind;
    if (h < lo) {
        ahead  = lo;
        behind = 0;
    } else {
        MapConsiderWindow(k, h, may_read);
        ahead  = (uint64_t)h + 1;
        behind = h;
    }
    for (;;) {
        uint64_t gap_ahead  = ahead < windows ? ahead * MAP_WINDOW_BLOCKS - k->hint : (uint64_t)-1;
        uint64_t gap_behind = behind > lo ? k->hint - (behind * MAP_WINDOW_BLOCKS - 1) : (uint64_t)-1;
        if (gap_ahead == (uint64_t)-1 && gap_behind == (uint64_t)-1)
            break;
        if (k->found && k->got == k->want && gap_ahead > k->far && gap_behind > k->far)
            break;
        if (gap_ahead <= gap_behind)
            MapConsiderWindow(k, (uint32_t)ahead++, may_read);
        else
            MapConsiderWindow(k, (uint32_t)--behind, may_read);
    }
}

void MapSayNoRoom(uint32_t want)
{
    if (g_map.no_room_said)
        return;
    g_map.no_room_said = true;

    uint32_t longest = 0;
    for (uint32_t w = 0; w < g_map.windows; w++)
        if (g_map.win[w].longest > longest)
            longest = g_map.win[w].longest;
    kprintf("[TagFS] block map: no run of %u block(s) can be had — %llu block(s) of the "
            "volume are free and the longest run a take may have is %u\n",
            want, (unsigned long long)tagfs_get_state()->ledger.free_blocks, longest);
}

error_t MapShort(uint32_t want)
{
    if (FallowBlocks() > 0)
        return ERR_WOULD_BLOCK;
    MapSayNoRoom(want);
    return ERR_TAGFS_NO_SPACE;
}

/*
 * The door every take outside an Allotment goes through. `exact` asks for a
 * run of exactly `want` or nothing; otherwise the longest up to `want`. With
 * no TagFS lock held it fetches what decides better; held under one it reads
 * nothing. Neither writes the sector the take lands in (M4b): the take is owed
 * to the medium, and the Settle cycle pays it at S1 — before any record, row
 * or Ledger naming the block reaches the medium.
 */
static error_t MapTakeDoor(uint32_t want, uint32_t hint, bool exact,
                           uint32_t *start, uint32_t *got, const char *site)
{
    if (want == 0 || !start || !got)
        return ERR_INVALID_ARGUMENT;

    TagFSLock *lock      = MapLock();
    bool       may_read  = !LockRankAnyHeldHere();
    bool       caught_up = false;
    uint32_t   rounds    = 0;

    for (;;) {
        TagFSLockTakeAt(lock, site);
        if (!g_map.up) {
            TagFSLockDropAt(lock, site);
            return ERR_TAGFS_NOT_INITIALIZED;
        }

        MapPick k = { .want = want, .hint = hint };
        MapPlan(&k, may_read && rounds < MAP_LOOK_ROUNDS);
        if (k.looks) {
            uint32_t look[MAP_LOOKS], looks = k.looks;
            memcpy(look, k.look, sizeof(look));
            TagFSLockDropAt(lock, site);
            rounds++;
            for (uint32_t i = 0; i < looks; i++)
                (void)MapFetch(look[i], site);
            continue;
        }

        if (!k.found || (exact && k.got < want)) {
            /* Before anybody is told there is no room, every growing file's
             * window is taken back, and every give written down without its
             * sector is laid on. */
            if (AllotmentTakeBackLocked(MAP_NO_FID)) {
                TagFSLockDropAt(lock, site);
                continue;
            }
            bool gives = g_map.gives != 0;
            if (!may_read || !gives || caught_up) {
                error_t short_by = MapShort(want);
                TagFSLockDropAt(lock, site);
                return short_by;
            }
            TagFSLockDropAt(lock, site);
            caught_up = true;
            BlockMapSettle();
            continue;
        }

        uint32_t n = exact ? want : k.got;
        if (!MapDeltaRoom(MapUnreadWindows(k.at, n))) {
            TagFSLockDropAt(lock, site);
            kprintf("[TagFS] block map: no memory to write down a take of %u block(s) at "
                    "%u — nothing is taken\n", n, k.at);
            return ERR_NO_MEMORY;
        }
        MapTakeKnown(k.at, n);
        TagFSLockDropAt(lock, site);

        *start = k.at;
        *got   = n;
        return OK;
    }
}

error_t BlockMapTake(uint32_t count, uint32_t hint, uint32_t *start, const char *site)
{
    uint32_t got = 0;
    return MapTakeDoor(count, hint, true, start, &got, site);
}

error_t BlockMapTakeLocked(uint32_t count, uint32_t hint, uint32_t *start)
{
    if (!g_map.up)
        return ERR_TAGFS_NOT_INITIALIZED;
    if (count == 0 || !start)
        return ERR_INVALID_ARGUMENT;

    for (;;) {
        MapPick k = { .want = count, .hint = hint };
        MapPlan(&k, false);
        if (k.found && k.got == count) {
            if (!MapDeltaRoom(MapUnreadWindows(k.at, count))) {
                kprintf("[TagFS] block map: no memory to write down a take of %u block(s) "
                        "at %u — nothing is taken\n", count, k.at);
                return ERR_NO_MEMORY;
            }
            MapTakeKnown(k.at, count);
            *start = k.at;
            return OK;
        }
        if (!AllotmentTakeBackLocked(MAP_NO_FID))
            break;
    }
    return MapShort(count);
}

error_t TagFSTakeRun(uint32_t want, uint32_t hint, uint32_t *start, uint32_t *got)
{
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = MapTakeDoor(want, hint, false, start, got, __func__);
    tagfs_leave();
    return rc;
}

#ifdef CONFIG_VOLUME_TESTS
/* The block BlockMapTake(1, 0) would be handed now: the same plan, with the
 * same sectors fetched to decide it. */
bool BlockMapNextFree(uint32_t *block)
{
    for (uint32_t rounds = 0; ; rounds++) {
        TagFSLockTake(MapLock());
        MapPick k = { .want = 1, .hint = 0 };
        MapPlan(&k, g_map.up && rounds < MAP_LOOK_ROUNDS);
        if (k.looks) {
            uint32_t look[MAP_LOOKS], looks = k.looks;
            memcpy(look, k.look, sizeof(look));
            TagFSLockDrop(MapLock());
            for (uint32_t i = 0; i < looks; i++)
                (void)MapFetch(look[i], __func__);
            continue;
        }
        *block = k.at;
        TagFSLockDrop(MapLock());
        return k.found;
    }
}
#endif
