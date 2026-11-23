#include "commonhold.h"
#include "../lock_rank/lock_rank.h"
#include "tagfs_volume.h"

/*
 * Commonhold (commonhold.h): one array of runs in order of their first block,
 * none meeting, under a leaf lock. Each change of an owner walks the runs it
 * meets once, cutting the one at either end of its blocks where it ends
 * inside it — two runs more at the most a change of a run, which the room a
 * change asks before its lock is counted by (CommonholdCuts).
 */


static uint64_t RunEnd(const CommonRun* r)
{
    return r->start + r->count;
}

/* The first run that ends past `at`. */
static uint64_t RunFrom(TagFSVolume* vol, uint64_t at)
{
    uint64_t lo = 0, hi = vol->commonhold.n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (RunEnd(&vol->commonhold.run[mid]) <= at)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

error_t CommonholdRoomFor(TagFSVolume* vol, uint64_t more)
{
    TagFSLockTake(&vol->commonhold.lock);
    uint64_t want = vol->commonhold.n + more, have = vol->commonhold.cap;
    TagFSLockDrop(&vol->commonhold.lock);
    while (want > have) {
        uint64_t   cap = want * 2u + 16u;
        CommonRun* run = kmalloc(sizeof(CommonRun) * cap);
        CommonRun* old = NULL;
        if (!run)
            return ERR_NO_MEMORY;
        TagFSLockTake(&vol->commonhold.lock);
        if (vol->commonhold.n + more <= vol->commonhold.cap) {
            old = run;              /* another change grew it meanwhile */
        } else if (vol->commonhold.n <= cap) {
            if (vol->commonhold.n)
                memcpy(run, vol->commonhold.run, sizeof(CommonRun) * vol->commonhold.n);
            old   = vol->commonhold.run;
            vol->commonhold.run = run;
            vol->commonhold.cap = cap;
        } else {
            old = run;
        }
        want = vol->commonhold.n + more;
        have = vol->commonhold.cap;
        TagFSLockDrop(&vol->commonhold.lock);
        kfree(old);
    }
    return OK;
}

void CommonholdCuts(TagFSVolume* vol, const Strip* runs, uint64_t n, uint64_t* pieces, uint64_t* splits)
{
    *pieces = 0;
    *splits = 0;
    TagFSLockTake(&vol->commonhold.lock);
    for (uint64_t i = 0; vol->commonhold.n && i < n; i++) {
        uint64_t a = runs[i].start, b = a + runs[i].count, met = 0;
        if (!StripOnGround(runs[i].kind))
            continue;
        for (uint64_t k = RunFrom(vol, a); k < vol->commonhold.n && vol->commonhold.run[k].start < b; k++)
            met++;
        *pieces += met;
        *splits += met ? 2u : 0u;
    }
    TagFSLockDrop(&vol->commonhold.lock);
}

bool CommonholdRoomHolds(TagFSVolume* vol, uint64_t splits)
{
    TagFSLockTake(&vol->commonhold.lock);
    bool holds = vol->commonhold.n + splits <= vol->commonhold.cap;
    TagFSLockDrop(&vol->commonhold.lock);
    return holds;
}

/* Run `k` cut at block `at` inside it: two runs, the second from `at`. Room
 * held. */
static void RunCut(TagFSVolume* vol, uint64_t k, uint64_t at)
{
    CommonRun* r = &vol->commonhold.run[k];
    memmove(&vol->commonhold.run[k + 2u], &vol->commonhold.run[k + 1u], sizeof(CommonRun) * (vol->commonhold.n - k - 1u));
    vol->commonhold.run[k + 1u]       = *r;
    vol->commonhold.run[k + 1u].start = at;
    vol->commonhold.run[k + 1u].count = (uint32_t)(RunEnd(r) - at);
    r->count            = (uint32_t)(at - r->start);
    vol->commonhold.n++;
}

/* The runs that meet [a, b) cut where a and b lie inside one: every run from
 * the one returned on to the first that begins at b or past it lies inside. */
static uint64_t RunsCutTo(TagFSVolume* vol, uint64_t a, uint64_t b)
{
    uint64_t k = RunFrom(vol, a);
    if (k < vol->commonhold.n && vol->commonhold.run[k].start < a)
        RunCut(vol, k++, a);
    uint64_t j = RunFrom(vol, b > 0 ? b - 1u : 0);
    if (j < vol->commonhold.n && vol->commonhold.run[j].start < b && RunEnd(&vol->commonhold.run[j]) > b)
        RunCut(vol, j, b);
    return k;
}

/* Blocks a change lets go of that Commonhold counts no owner on — a run
 * SHARED on a block not here, or a block whose last owner let it go already:
 * the writer that put the run in counted no owner for it. Kept from the
 * allocator, never given under another name; the first noted, said with no
 * lock held (CommonholdSettled), and a STRICT kernel stops at it. */
static void UncountedNoted(TagFSVolume* vol, uint64_t start, uint64_t count)
{
#if CONFIG_STRICT
    panic("[TagFS] DEFECT: a change lets go of blocks %llu+%llu, which no count of owners holds",
          (unsigned long long)start, (unsigned long long)count);
#endif
    if (vol->commonhold.wrong_count == 0 && !vol->commonhold.wrong_said) {
        vol->commonhold.wrong_at    = start;
        vol->commonhold.wrong_count = count;
    }
}

void CommonholdLetGoLocked(TagFSVolume* vol, uint64_t start, uint64_t count, uint32_t kind, uint64_t gen, CommonholdFreedFn freed,
                           void* ctx)
{
    uint64_t a = start, b = start + count;
    TagFSLockTake(&vol->commonhold.lock);
    for (uint64_t k = RunsCutTo(vol, a, b); a < b; k++) {
        uint64_t stop = k < vol->commonhold.n && vol->commonhold.run[k].start < b ? vol->commonhold.run[k].start : b;
        if (stop > a && kind == STRIP_SHARED)
            UncountedNoted(vol, a, stop - a);
        else if (stop > a)
            freed(ctx, a, stop - a);    /* a block one DATA run named alone */
        if (stop >= b)
            break;
        CommonRun* r = &vol->commonhold.run[k];
        if (r->owners == 0)
            UncountedNoted(vol, r->start, r->count);
        else if (--r->owners == 0) {
            r->gone = gen;
            freed(ctx, r->start, r->count);
        }
        a = RunEnd(r);
    }
    TagFSLockDrop(&vol->commonhold.lock);
}

void CommonholdShareLocked(TagFSVolume* vol, uint64_t start, uint64_t count)
{
    uint64_t a = start, b = start + count;
    TagFSLockTake(&vol->commonhold.lock);
    for (uint64_t k = RunsCutTo(vol, a, b); a < b; k++) {
        uint64_t stop = k < vol->commonhold.n && vol->commonhold.run[k].start < b ? vol->commonhold.run[k].start : b;
        if (stop > a) {             /* its one DATA owner, and the run put on it */
            memmove(&vol->commonhold.run[k + 1u], &vol->commonhold.run[k], sizeof(CommonRun) * (vol->commonhold.n - k));
            vol->commonhold.run[k] = (CommonRun){ .start = a, .gone = 0, .count = (uint32_t)(stop - a), .owners = 2 };
            vol->commonhold.n++;
            a = stop;
            continue;
        }
        vol->commonhold.run[k].owners++;
        vol->commonhold.run[k].gone = 0;
        a = RunEnd(&vol->commonhold.run[k]);
    }
    TagFSLockDrop(&vol->commonhold.lock);
}

uint32_t CommonholdOwners(TagFSVolume* vol, uint64_t block)
{
    TagFSLockTake(&vol->commonhold.lock);
    uint64_t k      = RunFrom(vol, block);
    uint32_t owners = k < vol->commonhold.n && vol->commonhold.run[k].start <= block ? vol->commonhold.run[k].owners : 0;
    TagFSLockDrop(&vol->commonhold.lock);
    return owners;
}

bool CommonholdOwnedAt(TagFSVolume* vol, uint64_t* at, uint64_t end, uint64_t slice, uint64_t* lo, uint64_t* hi)
{
    bool found = false;
    TagFSLockTake(&vol->commonhold.lock);
    for (uint64_t k = RunFrom(vol, *at); k < vol->commonhold.n && vol->commonhold.run[k].start < end; k++) {
        const CommonRun* r = &vol->commonhold.run[k];
        if (r->owners == 0 && r->gone <= slice)
            continue;
        *lo   = r->start > *at ? r->start : *at;
        *hi   = RunEnd(r) < end ? RunEnd(r) : end;
        found = true;
        break;
    }
    TagFSLockDrop(&vol->commonhold.lock);
    *at = found ? *hi : end;
    return found;
}

void CommonholdSettled(TagFSVolume* vol, uint64_t slice)
{
    uint64_t kept = 0;
    TagFSLockTake(&vol->commonhold.lock);
    for (uint64_t i = 0; i < vol->commonhold.n; i++) {
        const CommonRun* r    = &vol->commonhold.run[i];
        CommonRun*       last = kept ? &vol->commonhold.run[kept - 1u] : NULL;
        if (r->owners == 0 && r->gone <= slice)
            continue;
        if (last && RunEnd(last) == r->start && last->owners == r->owners && last->gone == r->gone &&
            (uint64_t)last->count + r->count <= UINT32_MAX) {
            last->count += r->count;
            continue;
        }
        vol->commonhold.run[kept++] = *r;
    }
    vol->commonhold.n = kept;
    uint64_t at = vol->commonhold.wrong_at, count = vol->commonhold.wrong_count;
    vol->commonhold.wrong_said |= count != 0;
    vol->commonhold.wrong_count = 0;
    TagFSLockDrop(&vol->commonhold.lock);
    if (count)
        kprintf("[TagFS] DEFECT: a change let go of blocks %llu+%llu, which no count of owners holds — they are "
                "kept from the allocator, never given under another name\n", (unsigned long long)at,
                (unsigned long long)count);
}

void CommonholdLetGoAll(TagFSVolume* vol)
{
    TagFSLockTake(&vol->commonhold.lock);
    CommonRun* run = vol->commonhold.run;
    vol->commonhold.run  = NULL;
    vol->commonhold.n    = 0;
    vol->commonhold.cap  = 0;
    vol->commonhold.wrong_count = 0;
    vol->commonhold.wrong_said  = false;
    TagFSLockDrop(&vol->commonhold.lock);
    kfree(run);
}

error_t CommonholdMountAdd(TagFSVolume* vol, uint64_t start, uint64_t count, uint32_t owners)
{
    for (; count; ) {
        uint32_t c  = count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
        error_t  rc = CommonholdRoomFor(vol, 1);
        if (rc != OK)
            return rc;
        TagFSLockTake(&vol->commonhold.lock);
        vol->commonhold.run[vol->commonhold.n++] = (CommonRun){ .start = start, .gone = 0, .count = c, .owners = owners };
        TagFSLockDrop(&vol->commonhold.lock);
        start += c;
        count -= c;
    }
    return OK;
}

const CommonRun* CommonholdRuns(TagFSVolume* vol, uint64_t* n)
{
    *n = vol->commonhold.n;
    return vol->commonhold.run;
}

/* The volume's commonhold part as it begins: what was not nothing. */
void CommonholdVolumeBegin(TagFSVolume* vol)
{
    vol->commonhold.lock = (TagFSLock)TAGFS_LOCK_INIT("commonhold", LOCK_RANK_LEAF);
}
