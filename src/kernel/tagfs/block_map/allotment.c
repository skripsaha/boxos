#include "block_map_internal.h"

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
    uint32_t fid;
    uint32_t start;
    uint32_t count;
} Allotment;

/* Sorted by start, and no two overlap. */
static Allotment *s_allot;
static uint32_t   s_allot_len;
static uint32_t   s_allot_cap;
static bool       s_no_memory_said;

static uint32_t AllotmentFind(uint32_t fid)
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
        if ((uint64_t)s_allot[mid].start + s_allot[mid].count <= block)
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
        uint64_t b = (uint64_t)s_allot[i].start + s_allot[i].count;
        if (b > end)
            b = end;
        for (uint32_t off = (uint32_t)(a - base); off < (uint32_t)(b - base); off++)
            view[off / 8] |= (uint8_t)(1u << (off % 8));
    }
}

static bool AllotmentAdd(uint32_t fid, uint32_t start, uint32_t count)
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

bool AllotmentTakeBackLocked(uint32_t keep_fid)
{
    uint32_t files  = 0;
    uint64_t blocks = 0;

    for (uint32_t i = s_allot_len; i-- > 0; ) {
        if (s_allot[i].fid == keep_fid)
            continue;
        uint32_t start = s_allot[i].start, count = s_allot[i].count;
        AllotmentRemove(i);
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

void AllotmentRelease(uint32_t fid)
{
    if (!g_map.up)
        return;
    uint32_t i = AllotmentFind(fid);
    if (i == s_allot_len)
        return;
    uint32_t start = s_allot[i].start, count = s_allot[i].count;
    AllotmentRemove(i);
    MapAvailabilityChanged(start, count, false);
}

/* The front of the file's own window: *n blocks from *s, the window shortened
 * by them. False when the file keeps none, or when the take cannot be written
 * down. */
static bool AllotmentTakeOwn(uint32_t fid, uint32_t want, uint32_t *s, uint32_t *n,
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
static error_t AllotmentTakeNew(uint32_t fid, uint32_t want, const MapPick *k,
                                uint32_t *s, uint32_t *n)
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
                    "for file %u — it grows without one, and its extents may be shorter\n",
                    keep, fid);
        }
    }
    MapTakeKnown(*s, *n);
    MapUnsettledAdd(fid, *s, *n);
    return OK;
}

static error_t AllotmentTakeInside(uint32_t fid, uint32_t want, uint64_t file_blocks,
                                   uint32_t hint, uint32_t *start, uint32_t *got)
{
    TagFSLock *lock      = MapLock();
    bool       may_read  = !LockRankAnyHeldHere();
    bool       caught_up = false;
    uint32_t   rounds    = 0;
    uint64_t   fits      = file_blocks < PLOT_RUN_MOST ? file_blocks : PLOT_RUN_MOST;
    uint32_t   size      = fits > want ? (uint32_t)fits : want;

    for (;;) {
        TagFSLockTake(lock);
        if (!g_map.up) {
            TagFSLockDrop(lock);
            return ERR_TAGFS_NOT_INITIALIZED;
        }

        uint32_t s = 0, n = 0;
        error_t  rc = OK;
        if (!AllotmentTakeOwn(fid, want, &s, &n, &rc)) {
            MapPick k = { .want = size, .hint = hint, .best_fit = true };
            MapPlan(&k, may_read && rounds < MAP_LOOK_ROUNDS);
            if (k.looks) {
                uint32_t look = k.look[0];
                TagFSLockDrop(lock);
                rounds++;
                (void)MapFetch(look, __func__);
                continue;
            }
            if (!k.found) {
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
            rc = AllotmentTakeNew(fid, want, &k, &s, &n);
        }
        TagFSLockDrop(lock);

        if (rc != OK) {
            kprintf("[TagFS] block map: no memory to write down a take of %u block(s) for "
                    "file %u — nothing is taken\n", want, fid);
            return rc;
        }
        *start = s;
        *got   = n;
        return OK;
    }
}

error_t AllotmentTake(uint32_t fid, uint32_t want, uint64_t file_blocks, uint32_t hint,
                      uint32_t *start, uint32_t *got)
{
    if (want == 0 || fid == 0 || !start || !got)
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = AllotmentTakeInside(fid, want, file_blocks, hint, start, got);
    tagfs_leave();
    return rc;
}

#ifdef CONFIG_VOLUME_TESTS
bool AllotmentHeld(uint32_t fid, uint32_t *start, uint32_t *count)
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
