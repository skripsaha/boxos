#include "binder.h"

/*
 * The births of the runs SHARED this mount's writes put in (binder.h,
 * BirthsShared*): a run SHARED names ground its DATA owner was born on long
 * before, so its birth — the stitch that put the run in (§2.8) — is not the
 * ground's, and Births, a block's one entry, cannot hold it. Kept here by the
 * file and the ground, in order of both, a file's entries never overlapping:
 * a range put in again by the same file is cut out of the entries before it.
 * One file that names one block SHARED twice, the two put in by two stitches
 * of one mount, is told the later birth for both — until a mount reads each
 * from its own stitch.
 *
 * The Settle's holder alone writes and reads it — the stitch's S6, the Tally —
 * so it takes no lock.
 */

typedef struct {
    uint64_t fid;
    uint64_t start;
    uint64_t count;
    uint64_t seq;
} SharedBirth;

static SharedBirth* s_birth;
static uint64_t     s_n;
static uint64_t     s_cap;

/* Whether entry `e` lies before (fid, block) in the order of both. */
static bool BirthBefore(const SharedBirth* e, uint64_t fid, uint64_t block)
{
    return e->fid < fid || (e->fid == fid && e->start + e->count <= block);
}

/* The first entry not before (fid, block). */
static uint64_t BirthFrom(uint64_t fid, uint64_t block)
{
    uint64_t lo = 0, hi = s_n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (BirthBefore(&s_birth[mid], fid, block))
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

static bool BirthRoom(uint64_t more)
{
    if (s_n + more <= s_cap)
        return true;
    uint64_t     cap  = (s_n + more) * 2u + 16u;
    SharedBirth* grown = kmalloc(sizeof(SharedBirth) * cap);
    if (!grown)
        return false;
    if (s_n)
        memcpy(grown, s_birth, sizeof(SharedBirth) * s_n);
    kfree(s_birth);
    s_birth = grown;
    s_cap   = cap;
    return true;
}

/* [start, end) of `fid` born at `seq`: the entries of `fid` it meets cut to
 * what lies outside it, and it put in among them. Room for two more held. */
static void BirthPut(uint64_t fid, uint64_t start, uint64_t end, uint64_t seq)
{
    uint64_t i = BirthFrom(fid, start);
    if (i < s_n && s_birth[i].fid == fid && s_birth[i].start < start) {
        SharedBirth* e    = &s_birth[i];
        uint64_t     eend = e->start + e->count;
        e->count = start - e->start;
        i++;
        if (eend > end) {               /* reaches past it: its tail kept after it */
            memmove(&s_birth[i + 1u], &s_birth[i], sizeof(SharedBirth) * (s_n - i));
            s_birth[i] = (SharedBirth){ fid, end, eend - end, e->seq };
            s_n++;
        }
    }
    uint64_t j = i;
    while (j < s_n && s_birth[j].fid == fid && s_birth[j].start + s_birth[j].count <= end)
        j++;
    if (j < s_n && s_birth[j].fid == fid && s_birth[j].start < end) {
        s_birth[j].count -= end - s_birth[j].start;
        s_birth[j].start  = end;
    }
    memmove(&s_birth[i + 1u], &s_birth[j], sizeof(SharedBirth) * (s_n - j));
    s_n = s_n - (j - i) + 1u;
    s_birth[i] = (SharedBirth){ fid, start, end - start, seq };
}

error_t BirthsSharedNoted(uint64_t fid, const Strip* strips, uint64_t count, uint64_t seq)
{
    for (uint64_t i = 0; i < count; i++) {
        if (strips[i].kind != STRIP_SHARED)
            continue;
        if (!BirthRoom(2))
            return ERR_NO_MEMORY;
        BirthPut(fid, strips[i].start, strips[i].start + strips[i].count, seq);
    }
    return OK;
}

uint64_t BirthsSharedOf(uint64_t fid, uint64_t block)
{
    uint64_t i = BirthFrom(fid, block);
    return i < s_n && s_birth[i].fid == fid && s_birth[i].start <= block ? s_birth[i].seq : 0;
}

void BirthsSharedLetGo(void)
{
    kfree(s_birth);
    s_birth = NULL;
    s_n     = 0;
    s_cap   = 0;
}
