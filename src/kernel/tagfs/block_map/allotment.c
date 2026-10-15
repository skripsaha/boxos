#include "block_map_internal.h"
#include "block_map_derived.h"
#include "../ink/fallow.h"
#include "../settle/settle.h"

/*
 * Allotment — ground a growing file keeps in hand (block_map.h).
 *
 * ‼ WHY A FILE KEEPS A WINDOW AND NOT JUST A HINT. Two files written side by
 * side each ask for the block after their own last one, and a take near the
 * hint then deals them alternate blocks: every extent one block long, and a
 * record that holds 4080 bytes of them refuses the file at about 2.6 MB. A
 * window taken for the file, sized to what it already is, keeps the next
 * growths contiguous whoever else is writing — a file of N blocks is laid in
 * about log2 N extents however its writes interleave.
 *
 * The window lives in memory only. Its blocks stay free on the medium, and
 * every take but its file's own is kept off them (AllotmentOverlay), so a
 * volume cut at any moment has no block that is taken and owned by nobody.
 * Everything here is under the volume's state lock.
 */

typedef struct {
    uint64_t fid;
    uint64_t start;
    uint32_t count;
} Allotment;

/* Sorted by start, and no two overlap. */
static Allotment *s_allot;
static uint32_t   s_allot_len;
static uint32_t   s_allot_cap;
static bool       s_no_memory_said;

static uint32_t AllotmentFind(uint64_t fid)
{
    for (uint32_t i = 0; i < s_allot_len; i++)
        if (s_allot[i].fid == fid)
            return i;
    return s_allot_len;
}

/* The first window, in order, that ends past `block`. */
static uint32_t AllotmentFirstEndingAfter(uint64_t block)
{
    uint32_t lo = 0, hi = s_allot_len;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s_allot[mid].start + s_allot[mid].count <= block)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

void AllotmentOverlay(uint32_t w, uint8_t *view)
{
    uint64_t base = (uint64_t)w * MAP_WINDOW_BLOCKS;
    uint64_t end  = base + MAP_WINDOW_BLOCKS;

    for (uint32_t i = AllotmentFirstEndingAfter(base);
         i < s_allot_len && s_allot[i].start < end; i++) {
        uint64_t a = s_allot[i].start > base ? s_allot[i].start : base;
        uint64_t b = s_allot[i].start + s_allot[i].count;
        if (b > end)
            b = end;
        for (uint32_t off = (uint32_t)(a - base); off < (uint32_t)(b - base); off++)
            view[off / 8] |= (uint8_t)(1u << (off % 8));
    }
}

static bool AllotmentAdd(uint64_t fid, uint64_t start, uint32_t count)
{
    if (s_allot_len == s_allot_cap) {
        uint32_t   cap   = s_allot_cap ? s_allot_cap * 2 : 16;
        Allotment *grown = kmalloc(sizeof(Allotment) * cap);
        if (!grown)
            return false;
        if (s_allot_len)
            memcpy(grown, s_allot, sizeof(Allotment) * s_allot_len);
        kfree(s_allot);
        s_allot     = grown;
        s_allot_cap = cap;
    }
    uint32_t at = AllotmentFirstEndingAfter(start);
    memmove(&s_allot[at + 1], &s_allot[at], sizeof(Allotment) * (s_allot_len - at));
    s_allot[at].fid   = fid;
    s_allot[at].start = start;
    s_allot[at].count = count;
    s_allot_len++;
    return true;
}

static void AllotmentRemove(uint32_t i)
{
    memmove(&s_allot[i], &s_allot[i + 1], sizeof(Allotment) * (s_allot_len - i - 1));
    s_allot_len--;
}

/* A book's map is derived, and a window lies on it as free ground no take of
 * another file's may have (block_map_derived.c): nothing of format v1's map is
 * told when one comes or goes. */
static bool AllotmentOnBook(void)
{
    return tagfs_get_state()->format == DEED_FORMAT_V2;
}

bool AllotmentTakeBackLocked(uint64_t keep_fid)
{
    uint32_t files  = 0;
    uint64_t blocks = 0;

    for (uint32_t i = s_allot_len; i-- > 0; ) {
        if (s_allot[i].fid == keep_fid)
            continue;
        uint64_t start = s_allot[i].start;
        uint32_t count = s_allot[i].count;
        AllotmentRemove(i);
        if (!AllotmentOnBook())
            MapAvailabilityChanged(start, count, false);
        files++;
        blocks += count;
    }
    if (files)
        kprintf("[TagFS] block map: the volume is short of room, so the windows %u growing "
                "file(s) kept in hand — %llu block(s) — are taken back before anybody is "
                "refused\n", files, (unsigned long long)blocks);
    return files != 0;
}

uint64_t AllotmentKeptLocked(void)
{
    uint64_t kept = 0;
    for (uint32_t i = 0; i < s_allot_len; i++)
        kept += s_allot[i].count;
    return kept;
}

void AllotmentDropAll(void)
{
    kfree(s_allot);
    s_allot          = NULL;
    s_allot_len      = 0;
    s_allot_cap      = 0;
    s_no_memory_said = false;
}

void AllotmentRelease(uint64_t fid)
{
    if (!g_map.up && !AllotmentOnBook())
        return;
    uint32_t i = AllotmentFind(fid);
    if (i == s_allot_len)
        return;
    uint64_t start = s_allot[i].start;
    uint32_t count = s_allot[i].count;
    AllotmentRemove(i);
    if (!AllotmentOnBook())
        MapAvailabilityChanged(start, count, false);
}

bool AllotmentOthersIn(uint64_t fid, uint64_t start, uint64_t count, uint64_t *past)
{
    for (uint32_t i = AllotmentFirstEndingAfter(start); i < s_allot_len && s_allot[i].start < start + count; i++) {
        if (s_allot[i].fid != fid) {
            *past = s_allot[i].start + s_allot[i].count;
            return true;
        }
    }
    return false;
}

/* The front of the file's own window: *n blocks from *s, the window shortened
 * by them. False when the file keeps none, or when the take cannot be written
 * down. */
static bool AllotmentTakeOwn(uint64_t fid, uint32_t want, uint64_t *s, uint32_t *n,
                             error_t *refusal)
{
    uint32_t i = AllotmentFind(fid);
    if (i == s_allot_len)
        return false;

    *s = s_allot[i].start;
    *n = want < s_allot[i].count ? want : s_allot[i].count;
    if (!MapDeltaRoom(MapUnreadWindows(*s, *n), false, true) || !MapUnsettledRoom()) {
        *refusal = ERR_NO_MEMORY;
        return true;
    }
    s_allot[i].start += *n;
    s_allot[i].count -= *n;
    if (s_allot[i].count == 0)
        AllotmentRemove(i);
    MapTakeKept(*s, *n);
    MapUnsettledAdd(fid, *s, *n);
    return true;
}

/* A new window for the file, best-fit, and the growth taken from its front;
 * what is left of the window is kept. The pick is already made. */
static error_t AllotmentTakeNew(uint64_t fid, uint32_t want, const MapPick *k,
                                uint64_t *s, uint32_t *n)
{
    *s = k->at;
    *n = k->got < want ? k->got : want;
    if (!MapDeltaRoom(MapUnreadWindows(*s, *n), false, true) || !MapUnsettledRoom())
        return ERR_NO_MEMORY;

    uint32_t keep = k->got - *n;
    if (keep) {
        if (AllotmentAdd(fid, *s + *n, keep)) {
            MapAvailabilityChanged(*s + *n, keep, true);
            s_no_memory_said = false;
        } else if (!s_no_memory_said) {
            s_no_memory_said = true;
            kprintf("[TagFS] block map: no memory to keep a window of %u block(s) in hand "
                    "for file %llu — it grows without one, and its extents may be shorter\n",
                    keep, fid);
        }
    }
    MapTakeKnown(*s, *n);
    MapUnsettledAdd(fid, *s, *n);
    return OK;
}

/* No take hands out more than BLOCK_MAP_TAKE_MOST: a growth past it — a span —
 * comes as as many takes, each a run's worth of windows under the lock. */
static error_t AllotmentTakeInside(uint64_t fid, uint32_t want, uint64_t file_blocks,
                                   uint64_t hint, uint64_t *start, uint32_t *got)
{
    TagFSLock *lock      = MapLock();
    bool       may_read  = !LockRankAnyHeldHere();
    bool       caught_up = false;
    uint32_t   rounds    = 0;
    uint64_t   fits      = file_blocks < BLOCK_MAP_TAKE_MOST ? file_blocks
                                                             : BLOCK_MAP_TAKE_MOST;
    uint32_t   size;

    if (want > BLOCK_MAP_TAKE_MOST)
        want = BLOCK_MAP_TAKE_MOST;
    size = fits > want ? (uint32_t)fits : want;

    MapPick k = { .want = size, .hint = hint, .best_fit = true };
    for (;;) {
        TagFSLockTake(lock);
        if (!g_map.up) {
            TagFSLockDrop(lock);
            return ERR_TAGFS_NOT_INITIALIZED;
        }

        /* Its own window as much as a new one: what the pool's reserve still
         * lacks is left free for it (block_map.h). */
        uint64_t s = 0;
        uint32_t n = 0;
        error_t  rc  = OK;
        uint64_t may = MapMayHaveLocked();
        uint32_t ask = may < want ? (uint32_t)may : want;
        if (!ask || !AllotmentTakeOwn(fid, ask, &s, &n, &rc)) {
            if (ask) {
                MapPlan(&k, may_read && rounds < MAP_LOOK_ROUNDS, may_read);
                if (k.paused) {
                    TagFSLockDrop(lock);
                    continue;
                }
            }
            k.stage = MAP_PLAN_FRESH;
            if (ask && k.looks) {
                uint32_t look = k.look[0];
                TagFSLockDrop(lock);
                rounds++;
                (void)MapFetch(look, __func__);
                continue;
            }
            if (!ask || !k.found) {
                if (AllotmentTakeBackLocked(fid)) {
                    TagFSLockDrop(lock);
                    continue;
                }
                if (!may_read || !g_map.gives || caught_up) {
                    error_t short_by = MapShort(want);
                    TagFSLockDrop(lock);
                    return short_by;
                }
                TagFSLockDrop(lock);
                caught_up = true;
                BlockMapSettle();
                continue;
            }
            if (k.spanned && !MapRunStillFree(k.at, k.got)) {
                TagFSLockDrop(lock);
                continue;
            }
            rc = AllotmentTakeNew(fid, ask, &k, &s, &n);
        }
        TagFSLockDrop(lock);

        if (rc != OK) {
            kprintf("[TagFS] block map: no memory to write down a take of %u block(s) for "
                    "file %llu — nothing is taken\n", want, fid);
            return rc;
        }
        *start = s;
        *got   = n;
        return OK;
    }
}

error_t AllotmentTake(uint64_t fid, uint32_t want, uint64_t file_blocks, uint64_t hint,
                      uint64_t *start, uint32_t *got)
{
    if (want == 0 || fid == 0 || !start || !got)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = AllotmentTakeInside(fid, want, file_blocks, hint, start, got);
    tagfs_leave();
    return rc;
}

// ----------------------------------------------------------------------------
// A book's data (spec_v2 §3.1, Allotment 1 and 2; §3.6)
// ----------------------------------------------------------------------------

/* Every block a book's data was handed since the volume came up: the reserve
 * the book keeps grows with them between two of its measures (endpaper.c). */
static volatile uint64_t s_book_taken;
static volatile uint8_t  s_book_short_said;  /* the refusal said, until a take is had: any core, no lock */

uint64_t AllotmentBookTaken(void)
{
    return __atomic_load_n(&s_book_taken, __ATOMIC_ACQUIRE);
}

/* Room for one more window in the list, made with no lock held: a window the
 * take keeps finds it under the lock, or — another take having used it — is
 * not kept, the window being a hint and nothing else (model A). */
static void AllotmentBookRoom(void)
{
    TagFSLock *lock = MapLock();
    TagFSLockTake(lock);
    uint32_t cap = s_allot_len < s_allot_cap ? 0 : (s_allot_cap ? s_allot_cap * 2 : 16);
    TagFSLockDrop(lock);
    if (!cap)
        return;
    Allotment *grown = kmalloc(sizeof(Allotment) * cap);
    if (!grown)
        return;
    TagFSLockTake(lock);
    Allotment *gone = grown;
    if (s_allot_len == s_allot_cap && cap > s_allot_cap) {
        if (s_allot_len)
            memcpy(grown, s_allot, sizeof(Allotment) * s_allot_len);
        gone        = s_allot;
        s_allot     = grown;
        s_allot_cap = cap;
    }
    TagFSLockDrop(lock);
    kfree(gone);
}

/* The run [s, s + n) taken out of window i: what is left past it kept — or,
 * nothing past it, what is left before — and the window gone when nothing is. */
static void AllotmentCut(uint32_t i, uint64_t s, uint64_t n)
{
    if (i == s_allot_len)
        return;
    uint64_t ws = s_allot[i].start, we = ws + s_allot[i].count;
    if (s + n <= ws || s >= we)
        return;
    if (s + n < we) {
        s_allot[i].start = s + n;
        s_allot[i].count = (uint32_t)(we - s - n);
    } else if (s > ws) {
        s_allot[i].count = (uint32_t)(s - ws);
    } else {
        AllotmentRemove(i);
    }
}

/* A file that went on its own run keeps the ground ahead of it in hand, as a
 * window sized to it — the free blocks from `at` on, as many as are free in a
 * row — so no other take, the ribbon's Quire among them, lands on the block
 * its next growth continues on. Nothing when it keeps a window already. */
static void AllotmentBookAhead(uint64_t fid, uint64_t at, uint64_t size)
{
    if (size == 0 || AllotmentFind(fid) < s_allot_len || s_allot_len == s_allot_cap)
        return;
    uint64_t free = BlockMapDerivedFreeFrom(at, size, fid);
    if (free)
        (void)AllotmentAdd(fid, at, (uint32_t)free);
}

/* A new window of `size` blocks for the file, the take `want` from its front
 * and the rest kept: false when no run that long is free. */
static bool AllotmentBookWindow(uint64_t fid, uint64_t size, uint32_t want, uint64_t hint, uint64_t *s)
{
    uint64_t at = 0;
    if (!BlockMapDerivedRunFree(hint, size, fid, &at))
        return false;
    uint32_t i = AllotmentFind(fid);
    if (i < s_allot_len)
        AllotmentRemove(i);
    *s = at;
    if (size > want && s_allot_len < s_allot_cap)
        (void)AllotmentAdd(fid, at + want, (uint32_t)(size - want));
    return true;
}

/*
 * ‼ A BLOCK THAT WILL NOT BE FULL IS LAID AWAY FROM WHERE THE FILE GOES ON
 * (Allotment 1). Its version is written over by the next write that reaches
 * it, and the version it leaves rests in Fallow — for cycles, wherever it
 * lies: laid on a block the file goes on into, it would keep that block from
 * the version that fills it, and every block a log fills would begin a run of
 * its own. So it goes on the free ground farthest from where the file goes
 * on: the last free block past the blocks the file may grow into — as many
 * more as it has — or else the last before where it goes on; on them, past
 * the very block that continues the run, only when no other block is free.
 */
static bool AllotmentBookAside(uint64_t fid, uint32_t want, uint64_t file_blocks, uint64_t hint, uint64_t *s,
                               uint32_t *n)
{
    uint64_t past = hint ? hint + (file_blocks > want ? file_blocks : want) : 0;
    for (uint32_t w = want; w > 0; w /= 2u) {
        if (BlockMapDerivedRunFreeBack(past, ~0ull, w, fid, s) ||
            (hint && BlockMapDerivedRunFreeBack(0, hint, w, fid, s)) || BlockMapDerivedRunFree(hint + 1u, w, fid, s)) {
            *n = w;
            return true;
        }
    }
    return false;
}

/*
 * Where the take lies, the lock held, `want` cut to what the reserve leaves —
 * a take `aside` laid away from where the file goes on (above); else (1) the
 * block that continues the file's run, while it is free — the hint, exact
 * (Allotment 1); (2) the front of the file's own window; (3) a new window
 * sized to the file — a run of `want` in one (Allotment 2) whose rest is
 * kept; (4) a run of `want` anywhere, then of half as many, down to one.
 * False when nothing is free.
 */
static bool AllotmentBookPick(uint64_t fid, uint32_t want, uint64_t file_blocks, uint64_t hint, bool aside,
                              uint64_t *s, uint32_t *n)
{
    uint32_t i    = AllotmentFind(fid);
    uint64_t size = file_blocks < BLOCK_MAP_TAKE_MOST ? file_blocks : BLOCK_MAP_TAKE_MOST;
    if (aside)
        return AllotmentBookAside(fid, want, file_blocks, hint, s, n);
    if (hint && (*n = (uint32_t)BlockMapDerivedFreeFrom(hint, want, fid)) > 0) {
        *s = hint;
        AllotmentCut(i, *s, *n);
        AllotmentBookAhead(fid, *s + *n, size);
        return true;
    }
    if (i < s_allot_len) {
        *n = want < s_allot[i].count ? want : s_allot[i].count;
        *s = s_allot[i].start;
        AllotmentCut(i, *s, *n);
        return true;
    }
    *n = want;
    if (size > want && AllotmentBookWindow(fid, size, want, hint, s))
        return true;
    for (uint32_t w = want; w > 0; w /= 2u) {
        if (BlockMapDerivedRunFree(hint, w, fid, s)) {
            *n = w;
            return true;
        }
    }
    return false;
}

static error_t AllotmentBookLocked(uint64_t fid, uint32_t want, uint64_t file_blocks, uint64_t hint, bool aside,
                                   uint64_t keep, uint64_t *start, uint32_t *got)
{
    uint64_t free = tagfs_get_state()->ledger.free_blocks;
    uint64_t may  = free > keep ? free - keep : 0;
    if (want > may)
        want = (uint32_t)may;
    if (want > BLOCK_MAP_TAKE_MOST)
        want = BLOCK_MAP_TAKE_MOST;
    for (uint32_t round = 0; want && round < 2u; round++) {
        uint64_t s = 0;
        uint32_t n = 0;
        if (AllotmentBookPick(fid, want, file_blocks, hint, aside, &s, &n)) {
            error_t rc = BlockMapDerivedTakeLocked(fid, s, n);
            if (rc != OK)
                return rc;
            __atomic_add_fetch(&s_book_taken, n, __ATOMIC_RELEASE);
            *start = s;
            *got   = n;
            return OK;
        }
        if (!AllotmentTakeBackLocked(fid))
            break;
    }
    return ERR_TAGFS_NO_SPACE;
}

/* Nothing can be had now: room on its way while Fallow rests ground a cycle
 * gives back — the cycle asked for — else none, said once until a take is had. */
static error_t AllotmentBookShort(uint32_t want, uint64_t keep)
{
    if (FallowBlocks() > 0) {
        SettleDemand(SETTLE_DEMAND_ROOM);
        return ERR_WOULD_BLOCK;
    }
    if (!__atomic_exchange_n(&s_book_short_said, 1u, __ATOMIC_ACQ_REL))
        kprintf("[TagFS] no block can be had for %u block(s) of a file's data: %llu free, and the book keeps "
                "%llu for its ribbon and its Tally (ERR_TAGFS_NO_SPACE)\n", want,
                (unsigned long long)tagfs_get_state()->ledger.free_blocks, (unsigned long long)keep);
    return ERR_TAGFS_NO_SPACE;
}

error_t AllotmentTakeBook(uint64_t fid, uint32_t want, uint64_t file_blocks, uint64_t hint, bool aside,
                          uint64_t keep, uint64_t *start, uint32_t *got)
{
    if (want == 0 || fid == 0 || !start || !got)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    AllotmentBookRoom();
    error_t rc = BlockMapDerivedStock();
    if (rc == OK) {
        TagFSLockTake(MapLock());
        rc = AllotmentBookLocked(fid, want, file_blocks, hint, aside, keep, start, got);
        TagFSLockDrop(MapLock());
    }
    if (rc == ERR_TAGFS_NO_SPACE)
        rc = AllotmentBookShort(want, keep);
    else if (rc == OK)
        __atomic_store_n(&s_book_short_said, 0u, __ATOMIC_RELEASE);
    tagfs_leave();
    return rc;
}

#ifdef CONFIG_VOLUME_TESTS
bool AllotmentHeld(uint64_t fid, uint64_t *start, uint32_t *count)
{
    TagFSLockTake(MapLock());
    uint32_t i    = AllotmentFind(fid);
    bool     held = i < s_allot_len;
    if (held) {
        *start = s_allot[i].start;
        *count = s_allot[i].count;
    }
    TagFSLockDrop(MapLock());
    return held;
}
#endif
