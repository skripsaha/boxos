#include "commonhold.h"
#include "../lock_rank/lock_rank.h"

/*
 * Commonhold (commonhold.h): one array of runs in order of their first block,
 * none meeting, under a leaf lock. Each change of an owner walks the runs it
 * meets once, cutting the one at either end of its blocks where it ends
 * inside it — two runs more at the most a change of a run, which the room a
 * change asks before its lock is counted by (CommonholdCuts).
 */

static TagFSLock  s_lock = TAGFS_LOCK_INIT("commonhold", LOCK_RANK_LEAF);
static CommonRun* s_run;         /* under s_lock */
static uint64_t   s_n;
static uint64_t   s_cap;
static uint64_t   s_wrong_at;    /* blocks let go that no count holds, said with no lock held */
static uint64_t   s_wrong_count;
static bool       s_wrong_said;

static uint64_t RunEnd(const CommonRun* r)
{
    return r->start + r->count;
}

/* The first run that ends past `at`. */
static uint64_t RunFrom(uint64_t at)
{
    uint64_t lo = 0, hi = s_n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (RunEnd(&s_run[mid]) <= at)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

error_t CommonholdRoomFor(uint64_t more)
{
    TagFSLockTake(&s_lock);
    uint64_t want = s_n + more, have = s_cap;
    TagFSLockDrop(&s_lock);
    while (want > have) {
        uint64_t   cap = want * 2u + 16u;
        CommonRun* run = kmalloc(sizeof(CommonRun) * cap);
        CommonRun* old = NULL;
        if (!run)
            return ERR_NO_MEMORY;
        TagFSLockTake(&s_lock);
        if (s_n + more <= s_cap) {
            old = run;              /* another change grew it meanwhile */
        } else if (s_n <= cap) {
            if (s_n)
                memcpy(run, s_run, sizeof(CommonRun) * s_n);
            old   = s_run;
            s_run = run;
            s_cap = cap;
        } else {
            old = run;
        }
        want = s_n + more;
        have = s_cap;
        TagFSLockDrop(&s_lock);
        kfree(old);
    }
    return OK;
}

void CommonholdCuts(const Strip* runs, uint64_t n, uint64_t* pieces, uint64_t* splits)
{
    *pieces = 0;
    *splits = 0;
    TagFSLockTake(&s_lock);
    for (uint64_t i = 0; s_n && i < n; i++) {
        uint64_t a = runs[i].start, b = a + runs[i].count, met = 0;
        if (!StripOnGround(runs[i].kind))
            continue;
        for (uint64_t k = RunFrom(a); k < s_n && s_run[k].start < b; k++)
            met++;
        *pieces += met;
        *splits += met ? 2u : 0u;
    }
    TagFSLockDrop(&s_lock);
}

bool CommonholdRoomHolds(uint64_t splits)
{
    TagFSLockTake(&s_lock);
    bool holds = s_n + splits <= s_cap;
    TagFSLockDrop(&s_lock);
    return holds;
}

/* Run `k` cut at block `at` inside it: two runs, the second from `at`. Room
 * held. */
static void RunCut(uint64_t k, uint64_t at)
{
    CommonRun* r = &s_run[k];
    memmove(&s_run[k + 2u], &s_run[k + 1u], sizeof(CommonRun) * (s_n - k - 1u));
    s_run[k + 1u]       = *r;
    s_run[k + 1u].start = at;
    s_run[k + 1u].count = (uint32_t)(RunEnd(r) - at);
    r->count            = (uint32_t)(at - r->start);
    s_n++;
}

/* The runs that meet [a, b) cut where a and b lie inside one: every run from
 * the one returned on to the first that begins at b or past it lies inside. */
static uint64_t RunsCutTo(uint64_t a, uint64_t b)
{
    uint64_t k = RunFrom(a);
    if (k < s_n && s_run[k].start < a)
        RunCut(k++, a);
    uint64_t j = RunFrom(b > 0 ? b - 1u : 0);
    if (j < s_n && s_run[j].start < b && RunEnd(&s_run[j]) > b)
        RunCut(j, b);
    return k;
}

/* Blocks a change lets go of that Commonhold counts no owner on — a run
 * SHARED on a block not here, or a block whose last owner let it go already:
 * the writer that put the run in counted no owner for it. Kept from the
 * allocator, never given under another name; the first noted, said with no
 * lock held (CommonholdSettled), and a STRICT kernel stops at it. */
static void UncountedNoted(uint64_t start, uint64_t count)
{
#if CONFIG_STRICT
    panic("[TagFS] DEFECT: a change lets go of blocks %llu+%llu, which no count of owners holds",
          (unsigned long long)start, (unsigned long long)count);
#endif
    if (s_wrong_count == 0 && !s_wrong_said) {
        s_wrong_at    = start;
        s_wrong_count = count;
    }
}

void CommonholdLetGoLocked(uint64_t start, uint64_t count, uint32_t kind, uint64_t gen, CommonholdFreedFn freed,
                           void* ctx)
{
    uint64_t a = start, b = start + count;
    TagFSLockTake(&s_lock);
    for (uint64_t k = RunsCutTo(a, b); a < b; k++) {
        uint64_t stop = k < s_n && s_run[k].start < b ? s_run[k].start : b;
        if (stop > a && kind == STRIP_SHARED)
            UncountedNoted(a, stop - a);
        else if (stop > a)
            freed(ctx, a, stop - a);    /* a block one DATA run named alone */
        if (stop >= b)
            break;
        CommonRun* r = &s_run[k];
        if (r->owners == 0)
            UncountedNoted(r->start, r->count);
        else if (--r->owners == 0) {
            r->gone = gen;
            freed(ctx, r->start, r->count);
        }
        a = RunEnd(r);
    }
    TagFSLockDrop(&s_lock);
}

void CommonholdShareLocked(uint64_t start, uint64_t count)
{
    uint64_t a = start, b = start + count;
    TagFSLockTake(&s_lock);
    for (uint64_t k = RunsCutTo(a, b); a < b; k++) {
        uint64_t stop = k < s_n && s_run[k].start < b ? s_run[k].start : b;
        if (stop > a) {             /* its one DATA owner, and the run put on it */
            memmove(&s_run[k + 1u], &s_run[k], sizeof(CommonRun) * (s_n - k));
            s_run[k] = (CommonRun){ .start = a, .gone = 0, .count = (uint32_t)(stop - a), .owners = 2 };
            s_n++;
            a = stop;
            continue;
        }
        s_run[k].owners++;
        s_run[k].gone = 0;
        a = RunEnd(&s_run[k]);
    }
    TagFSLockDrop(&s_lock);
}

uint32_t CommonholdOwners(uint64_t block)
{
    TagFSLockTake(&s_lock);
    uint64_t k      = RunFrom(block);
    uint32_t owners = k < s_n && s_run[k].start <= block ? s_run[k].owners : 0;
    TagFSLockDrop(&s_lock);
    return owners;
}

bool CommonholdOwnedAt(uint64_t* at, uint64_t end, uint64_t slice, uint64_t* lo, uint64_t* hi)
{
    bool found = false;
    TagFSLockTake(&s_lock);
    for (uint64_t k = RunFrom(*at); k < s_n && s_run[k].start < end; k++) {
        const CommonRun* r = &s_run[k];
        if (r->owners == 0 && r->gone <= slice)
            continue;
        *lo   = r->start > *at ? r->start : *at;
        *hi   = RunEnd(r) < end ? RunEnd(r) : end;
        found = true;
        break;
    }
    TagFSLockDrop(&s_lock);
    *at = found ? *hi : end;
    return found;
}

void CommonholdSettled(uint64_t slice)
{
    uint64_t kept = 0;
    TagFSLockTake(&s_lock);
    for (uint64_t i = 0; i < s_n; i++) {
        const CommonRun* r    = &s_run[i];
        CommonRun*       last = kept ? &s_run[kept - 1u] : NULL;
        if (r->owners == 0 && r->gone <= slice)
            continue;
        if (last && RunEnd(last) == r->start && last->owners == r->owners && last->gone == r->gone &&
            (uint64_t)last->count + r->count <= UINT32_MAX) {
            last->count += r->count;
            continue;
        }
        s_run[kept++] = *r;
    }
    s_n = kept;
    uint64_t at = s_wrong_at, count = s_wrong_count;
    s_wrong_said |= count != 0;
    s_wrong_count = 0;
    TagFSLockDrop(&s_lock);
    if (count)
        kprintf("[TagFS] DEFECT: a change let go of blocks %llu+%llu, which no count of owners holds — they are "
                "kept from the allocator, never given under another name\n", (unsigned long long)at,
                (unsigned long long)count);
}

void CommonholdLetGoAll(void)
{
    TagFSLockTake(&s_lock);
    CommonRun* run = s_run;
    s_run  = NULL;
    s_n    = 0;
    s_cap  = 0;
    s_wrong_count = 0;
    s_wrong_said  = false;
    TagFSLockDrop(&s_lock);
    kfree(run);
}

error_t CommonholdMountAdd(uint64_t start, uint64_t count, uint32_t owners)
{
    for (; count; ) {
        uint32_t c  = count > UINT32_MAX ? UINT32_MAX : (uint32_t)count;
        error_t  rc = CommonholdRoomFor(1);
        if (rc != OK)
            return rc;
        TagFSLockTake(&s_lock);
        s_run[s_n++] = (CommonRun){ .start = start, .gone = 0, .count = c, .owners = owners };
        TagFSLockDrop(&s_lock);
        start += c;
        count -= c;
    }
    return OK;
}

const CommonRun* CommonholdRuns(uint64_t* n)
{
    *n = s_n;
    return s_run;
}
