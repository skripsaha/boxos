#include "block_map_internal.h"
#include "../ink/fallow.h"
#include "../settle/settle.h"
#include "../glean/glean.h"

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
 *
 * ‼ NO PLAN HOLDS THE LOCK FOR LONGER THAN A FEW HUNDRED WINDOWS. The state
 * lock keeps interrupts off on its core and on every core that waits for it,
 * and a volume of 16 TiB has a million windows. So a best fit does not weigh
 * every window: it asks the lists of windows by their longest run (MapFitMoved)
 * for the few that can hold the take, smallest list first, and walks the
 * volume only when none of them does. A walk of whole free windows is kept
 * for the hold, so no window is walked twice in it. And a door that holds no
 * lock lets the lock go every MAP_PLAN_STEPS windows its plan weighs and takes
 * it again where the plan stopped; the run a plan that spanned holds chose is
 * asked again right before the take (MapRunStillFree), and a run another take
 * had meanwhile begins the plan again.
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

/* How far a run that reaches the end of window w - 1 goes on from window w:
 * whole windows of free blocks, then the head of the first that is not whole —
 * walked no further than `need` asks. The walk is kept for the hold and only
 * ever goes on from where it stopped, or takes one more whole window in front
 * of it, so the windows weighed walk each window once, whatever their number
 * and however long the run each of them ends in. */
static uint32_t MapChainFrom(MapPick *k, uint32_t w, uint64_t need)
{
    bool whole = w < g_map.windows && g_map.win[w].free == MAP_WINDOW_BLOCKS;
    if (k->chain_kept && whole && w + 1u == k->chain_from)
        k->chain_from = w;
    if (!k->chain_kept || w < k->chain_from || w > k->chain_upto) {
        k->chain_kept  = true;
        k->chain_ended = false;
        k->chain_head  = 0;
        k->chain_from  = w;
        k->chain_upto  = w;
    }

    uint64_t len = (uint64_t)(k->chain_upto - w) * MAP_WINDOW_BLOCKS;
    while (!k->chain_ended && len < need) {
        if (k->chain_upto == g_map.windows) {
            k->chain_ended = true;
            break;
        }
        const BlockMapWindow *x = &g_map.win[k->chain_upto];
        k->steps++;
        if (x->free != MAP_WINDOW_BLOCKS) {
            k->chain_ended = true;
            k->chain_head  = x->head;
            break;
        }
        k->chain_upto++;
        len += MAP_WINDOW_BLOCKS;
    }
    if (k->chain_ended)
        len += k->chain_head;
    return len > UINT32_MAX ? UINT32_MAX : (uint32_t)len;
}

/* A run of `len` blocks in its window that reaches the window's end, with the
 * run past it as far as the plan measures runs. */
static uint32_t MapRunOn(MapPick *k, uint32_t w, uint32_t len)
{
    uint64_t on = (uint64_t)len + MapChainFrom(k, w + 1u, k->reach > len ? k->reach - len : 0);
    return on > UINT32_MAX ? UINT32_MAX : (uint32_t)on;
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
            len = MapRunOn(k, w, len);
        MapConsider(k, base + a, len);
    }
}

static void MapConsiderWindow(MapPick *k, uint32_t w, bool may_read)
{
    const BlockMapWindow *x = &g_map.win[w];
    k->steps++;
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
        MapConsider(k, base + x->at, ends ? MapRunOn(k, w, x->longest) : x->longest);
    if (x->head && x->at != 0)
        MapConsider(k, base, x->head);
    if (x->tail && !ends)
        MapConsider(k, base + MAP_WINDOW_BLOCKS - x->tail, MapRunOn(k, w, x->tail));

    /* The sector would decide better: an exact window holding a run of
     * `want` may hold one nearer than its longest, and a window whose numbers
     * are bounds may hold longer runs than it says. */
    if (may_read && !k->best_fit &&
        ((x->flags & MAP_W_EXACT) ? x->longest >= k->want
                                  : x->free >= (k->found ? (k->got < k->want ? k->got + 1 : k->want) : 1)))
        MapLook(k, w);
}

/* The hold's steps are spent and the caller lets the lock go between holds:
 * the plan stops here, to go on in the next. */
static bool MapPlanSpent(MapPick *k, bool may_pause)
{
    if (!may_pause || k->steps < MAP_PLAN_STEPS)
        return false;
    k->paused = true;
    return true;
}

/* The first window with room, moved up to it — a hold's steps at a time. */
static bool MapPlanRoom(MapPick *k, bool may_pause)
{
    while (g_map.first_room < g_map.windows && g_map.win[g_map.first_room].free == 0) {
        if (MapPlanSpent(k, may_pause))
            return false;
        g_map.first_room++;
        k->steps++;
    }
    k->lo = g_map.first_room;
    return true;
}

/* A whole free window, weighed from as far back as the run it lies in is
 * measured: one near the end of a long run of whole windows would otherwise
 * look as short as what lies after it. */
static void MapConsiderWhole(MapPick *k, uint32_t w)
{
    uint32_t s = w;
    while (s > k->lo && g_map.win[s - 1u].free == MAP_WINDOW_BLOCKS &&
           (uint64_t)(w - s + 1u) * MAP_WINDOW_BLOCKS < k->reach) {
        s--;
        k->steps++;
    }
    if (s > k->lo)
        MapConsiderWindow(k, s - 1u, false);
    MapConsiderWindow(k, s, false);
}

/* The best fit asked of the lists, from the one whose windows may just hold
 * the take upwards: a few windows of each, until a list gives a run of all of
 * `want`. A list is weighed within one hold, so the place to go on from is
 * the list, never a window that may have left it. */
static bool MapPlanFit(MapPick *k, bool may_pause)
{
    for (; k->list <= MAP_FIT_LISTS; k->list++) {
        if (MapPlanSpent(k, may_pause))
            return false;
        uint32_t looked = 0;
        for (uint32_t at = g_map.fit[k->list - 1u]; at && looked < MAP_FIT_LOOKS; looked++) {
            uint32_t next = g_map.win[at - 1u].fit_next;
            if (k->list == MAP_FIT_LISTS)
                MapConsiderWhole(k, at - 1u);
            else
                MapConsiderWindow(k, at - 1u, false);
            at = next;
        }
        if (k->found && k->got == k->want)
            break;
    }
    return true;
}

/* Every window from `ahead` on: the best fit the lists could not give. */
static bool MapPlanEvery(MapPick *k, bool may_pause)
{
    for (; k->ahead < g_map.windows; k->ahead++) {
        if (MapPlanSpent(k, may_pause))
            return false;
        MapConsiderWindow(k, (uint32_t)k->ahead, false);
    }
    return true;
}

/* Outward from the hint's window: `ahead` is the next window after it,
 * `behind` one past the next window before it (0 when there is none). */
static bool MapPlanOutward(MapPick *k, bool may_read, bool may_pause)
{
    uint64_t windows = g_map.windows;
    for (;;) {
        uint64_t gap_ahead  = k->ahead < windows ? k->ahead * MAP_WINDOW_BLOCKS - k->hint
                                                 : (uint64_t)-1;
        uint64_t gap_behind = k->behind > k->lo ? k->hint - (k->behind * MAP_WINDOW_BLOCKS - 1)
                                                : (uint64_t)-1;
        if (gap_ahead == (uint64_t)-1 && gap_behind == (uint64_t)-1)
            return true;
        if (k->found && k->got == k->want && gap_ahead > k->far && gap_behind > k->far)
            return true;
        if (MapPlanSpent(k, may_pause))
            return false;
        if (gap_ahead <= gap_behind)
            MapConsiderWindow(k, (uint32_t)k->ahead++, may_read);
        else
            MapConsiderWindow(k, (uint32_t)--k->behind, may_read);
    }
}

/* The hint's own window first: where a file grows in place. */
static void MapPlanHint(MapPick *k, bool may_read)
{
    if (k->hint >= g_map.blocks)
        k->hint = g_map.blocks - 1;
    uint32_t h = k->hint / MAP_WINDOW_BLOCKS;
    if (h >= k->lo)
        MapConsiderWindow(k, h, may_read && !k->best_fit);
    if (k->best_fit) {
        uint32_t fits = k->want < MAP_WINDOW_BLOCKS ? k->want : MAP_WINDOW_BLOCKS;
        k->list  = 32u - (uint32_t)__builtin_clz(fits);
        k->stage = k->found && k->far == 0 && k->got == k->want ? MAP_PLAN_DONE : MAP_PLAN_FIT;
        return;
    }
    k->ahead  = h < k->lo ? k->lo : (uint64_t)h + 1u;
    k->behind = h < k->lo ? 0 : h;
    k->stage  = MAP_PLAN_WALK;
}

static void MapPlanGo(MapPick *k, bool may_read, bool may_pause)
{
    if (k->stage == MAP_PLAN_ROOM) {
        if (!MapPlanRoom(k, may_pause))
            return;
        k->stage = k->lo >= g_map.windows || k->want == 0 ? MAP_PLAN_DONE : MAP_PLAN_HINT;
    }
    if (k->stage == MAP_PLAN_HINT)
        MapPlanHint(k, may_read);
    if (k->stage == MAP_PLAN_FIT) {
        if (!MapPlanFit(k, may_pause))
            return;
        k->ahead = k->lo;
        k->stage = k->found && k->got == k->want ? MAP_PLAN_DONE : MAP_PLAN_WALK;
    }
    if (k->stage == MAP_PLAN_WALK) {
        if (!(k->best_fit ? MapPlanEvery(k, may_pause) : MapPlanOutward(k, may_read, may_pause)))
            return;
        k->stage = MAP_PLAN_DONE;
    }

    /* A best fit fetches only the hint's sector: growth in place is the one
     * place nearness decides. */
    uint32_t h = k->hint / MAP_WINDOW_BLOCKS;
    if (k->best_fit && may_read && k->want && h >= k->lo && h < g_map.windows && !MapReady(h) &&
        g_map.win[h].free && !(k->found && k->far == 0))
        MapLook(k, h);
}

void MapPlan(MapPick *k, bool may_read, bool may_pause)
{
    if (k->stage == MAP_PLAN_FRESH) {
        /* A run is measured a window past `want`: far enough to tell a fit
         * from a longer run, and to start the take at a hint inside it. */
        k->found   = false;
        k->looks   = 0;
        k->spanned = false;
        k->reach   = (uint64_t)k->want + MAP_WINDOW_BLOCKS;
        k->stage   = MAP_PLAN_ROOM;
    } else {
        k->spanned = true;
    }
    k->paused     = false;
    k->steps      = 0;
    k->chain_kept = false;
    MapPlanGo(k, may_read, may_pause);
#ifdef CONFIG_VOLUME_TESTS
    if (k->steps > g_map.hold_most)
        g_map.hold_most = k->steps;
#endif
}

/* Inside [at, at + longest), the head or the tail — or the whole window. */
static bool MapSpanKnownFree(const BlockMapWindow *x, uint32_t off, uint32_t n)
{
    uint32_t end = off + n;
    return end <= x->head || off >= MAP_WINDOW_BLOCKS - x->tail ||
           (off >= x->at && end <= (uint32_t)x->at + x->longest);
}

bool MapRunStillFree(uint32_t start, uint32_t count)
{
    for (uint32_t b = start, end = start + count; b < end; ) {
        uint32_t      w   = b / MAP_WINDOW_BLOCKS;
        uint32_t      off = b % MAP_WINDOW_BLOCKS;
        uint32_t      n   = end - b < MAP_WINDOW_BLOCKS - off ? end - b : MAP_WINDOW_BLOCKS - off;
        BlockMapPage *p   = MapReady(w);

        if (p) {
            MapViewOf(w, p->bits, g_map.view);
            for (uint32_t i = off; i < off + n; i++)
                if (MapBit(g_map.view, i))
                    return false;
        } else if (!MapSpanKnownFree(&g_map.win[w], off, n)) {
            return false;
        }
        b += n;
    }
    return true;
}

void MapSayNoRoom(uint32_t want)
{
    if (g_map.no_room_said)
        return;
    g_map.no_room_said = true;

    /* The longest run told by the lists it sits in, not by a walk of every
     * window under the lock. */
    uint32_t list = MAP_FIT_LISTS;
    while (list && !g_map.fit[list - 1u])
        list--;
    uint64_t free = tagfs_get_state()->ledger.free_blocks;
    if (list == 0)
        kprintf("[TagFS] block map: no run of %u block(s) can be had — %llu block(s) of the "
                "volume are free and not one of them is a block a take may have\n",
                want, (unsigned long long)free);
    else if (list == MAP_FIT_LISTS)
        kprintf("[TagFS] block map: no run of %u block(s) can be had — %llu block(s) of the "
                "volume are free and the longest run a take may have takes a whole window "
                "of %u and what the windows beside it give\n",
                want, (unsigned long long)free, MAP_WINDOW_BLOCKS);
    else
        kprintf("[TagFS] block map: no run of %u block(s) can be had — %llu block(s) of the "
                "volume are free and the longest run a take may have inside one window is "
                "%u to %u block(s)\n",
                want, (unsigned long long)free, 1u << (list - 1u), (1u << list) - 1u);
}

/* Short while Fallow rests blocks: a cycle gives them back, so one is demanded
 * now rather than when Lull allows it. Short while gives wait as deltas for
 * sectors not in memory — Fallow's own, given back at a cycle's end under the
 * lock: the ground is free already and only unseen, and the next cycle's S1
 * lays them on. Short with neither while the pool's chain holds more than its
 * live records need: the next cycles glean it down to them, and what comes off
 * rests and comes back the same way. */
error_t MapShort(uint32_t want)
{
    if (FallowBlocks() > 0 || g_map.gives != 0) {
        SettleDemand(SETTLE_DEMAND_ROOM);
        return ERR_WOULD_BLOCK;
    }
    if (GleanMayFree()) {
        GleanShort();
        SettleDemand(SETTLE_DEMAND_ROOM);
        return ERR_WOULD_BLOCK;
    }
    MapSayNoRoom(want);
    return ERR_TAGFS_NO_SPACE;
}

/*
 * The door every take outside an Allotment goes through. `exact` asks for a
 * run of exactly `want` or nothing; otherwise the longest up to `want`. With
 * no TagFS lock held it fetches what decides better; held under one it reads
 * nothing. Neither writes the sector the take lands in (M4b): the take is owed
 * to the medium, and the Settle cycle pays it at S1 — before any record, row
 * or Ledger naming the block reaches the medium. A take for a file's write
 * (`fid`) is one no settled record names until its Ink settles.
 */
static error_t MapTakeDoor(uint32_t want, uint32_t hint, bool exact, uint32_t fid,
                           uint32_t *start, uint32_t *got, const char *site)
{
    if (want == 0 || !start || !got)
        return ERR_INVALID_ARGUMENT;

    TagFSLock *lock      = MapLock();
    bool       may_read  = !LockRankAnyHeldHere();
    bool       caught_up = false;
    uint32_t   rounds    = 0;
    MapPick    k         = { .want = want, .hint = hint };

    for (;;) {
        TagFSLockTakeAt(lock, site);
        if (!g_map.up) {
            TagFSLockDropAt(lock, site);
            return ERR_TAGFS_NOT_INITIALIZED;
        }

        MapPlan(&k, may_read && rounds < MAP_LOOK_ROUNDS, may_read);
        if (k.paused) {
            TagFSLockDropAt(lock, site);
            continue;
        }
        k.stage = MAP_PLAN_FRESH;
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
        if (k.spanned && !MapRunStillFree(k.at, n)) {
            TagFSLockDropAt(lock, site);
            continue;
        }
        if (!MapDeltaRoom(MapUnreadWindows(k.at, n), false, true) ||
            (fid != MAP_NO_FID && !MapUnsettledRoom())) {
            TagFSLockDropAt(lock, site);
            kprintf("[TagFS] block map: no memory to write down a take of %u block(s) at "
                    "%u — nothing is taken\n", n, k.at);
            return ERR_NO_MEMORY;
        }
        MapTakeKnown(k.at, n);
        if (fid != MAP_NO_FID)
            MapUnsettledAdd(fid, k.at, n);
        TagFSLockDropAt(lock, site);

        *start = k.at;
        *got   = n;
        return OK;
    }
}

error_t BlockMapTake(uint32_t count, uint32_t hint, uint32_t *start, const char *site)
{
    uint32_t got = 0;
    return MapTakeDoor(count, hint, true, MAP_NO_FID, start, &got, site);
}

error_t BlockMapTakeFor(uint32_t fid, uint32_t count, uint32_t hint, uint32_t *start,
                        const char *site)
{
    uint32_t got = 0;
    return MapTakeDoor(count, hint, true, fid, start, &got, site);
}

error_t BlockMapTakeLocked(uint32_t count, uint32_t hint, uint32_t *start)
{
    if (!g_map.up)
        return ERR_TAGFS_NOT_INITIALIZED;
    if (count == 0 || !start)
        return ERR_INVALID_ARGUMENT;

    for (;;) {
        MapPick k = { .want = count, .hint = hint };
        MapPlan(&k, false, false);
        if (k.found && k.got == count) {
            if (!MapDeltaRoom(MapUnreadWindows(k.at, count), false, true)) {
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

/* No longer than one extent: a run past it is taken by as many takes, each a
 * run's worth of windows under the lock. */
error_t TagFSTakeRun(uint32_t want, uint32_t hint, uint32_t *start, uint32_t *got)
{
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = MapTakeDoor(want < PLOT_RUN_MOST ? want : PLOT_RUN_MOST, hint, false,
                             MAP_NO_FID, start, got, __func__);
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
        MapPlan(&k, g_map.up && rounds < MAP_LOOK_ROUNDS, false);
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
