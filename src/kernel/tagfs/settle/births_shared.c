#include "binder.h"
#include "tagfs_volume.h"

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


/* Whether entry `e` lies before (fid, block) in the order of both. */
static bool BirthBefore(const SharedBirth* e, uint64_t fid, uint64_t block)
{
    return e->fid < fid || (e->fid == fid && e->start + e->count <= block);
}

/* The first entry not before (fid, block). */
static uint64_t BirthFrom(TagFSVolume* vol, uint64_t fid, uint64_t block)
{
    uint64_t lo = 0, hi = vol->births_shared.n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (BirthBefore(&vol->births_shared.birth[mid], fid, block))
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

static bool BirthRoom(TagFSVolume* vol, uint64_t more)
{
    if (vol->births_shared.n + more <= vol->births_shared.cap)
        return true;
    uint64_t     cap  = (vol->births_shared.n + more) * 2u + 16u;
    SharedBirth* grown = kmalloc(sizeof(SharedBirth) * cap);
    if (!grown)
        return false;
    if (vol->births_shared.n)
        memcpy(grown, vol->births_shared.birth, sizeof(SharedBirth) * vol->births_shared.n);
    kfree(vol->births_shared.birth);
    vol->births_shared.birth = grown;
    vol->births_shared.cap   = cap;
    return true;
}

/* [start, end) of `fid` born at `seq`: the entries of `fid` it meets cut to
 * what lies outside it, and it put in among them. Room for two more held. */
static void BirthPut(TagFSVolume* vol, uint64_t fid, uint64_t start, uint64_t end, uint64_t seq)
{
    uint64_t i = BirthFrom(vol, fid, start);
    if (i < vol->births_shared.n && vol->births_shared.birth[i].fid == fid && vol->births_shared.birth[i].start < start) {
        SharedBirth* e    = &vol->births_shared.birth[i];
        uint64_t     eend = e->start + e->count;
        e->count = start - e->start;
        i++;
        if (eend > end) {               /* reaches past it: its tail kept after it */
            memmove(&vol->births_shared.birth[i + 1u], &vol->births_shared.birth[i], sizeof(SharedBirth) * (vol->births_shared.n - i));
            vol->births_shared.birth[i] = (SharedBirth){ fid, end, eend - end, e->seq };
            vol->births_shared.n++;
        }
    }
    uint64_t j = i;
    while (j < vol->births_shared.n && vol->births_shared.birth[j].fid == fid && vol->births_shared.birth[j].start + vol->births_shared.birth[j].count <= end)
        j++;
    if (j < vol->births_shared.n && vol->births_shared.birth[j].fid == fid && vol->births_shared.birth[j].start < end) {
        vol->births_shared.birth[j].count -= end - vol->births_shared.birth[j].start;
        vol->births_shared.birth[j].start  = end;
    }
    memmove(&vol->births_shared.birth[i + 1u], &vol->births_shared.birth[j], sizeof(SharedBirth) * (vol->births_shared.n - j));
    vol->births_shared.n = vol->births_shared.n - (j - i) + 1u;
    vol->births_shared.birth[i] = (SharedBirth){ fid, start, end - start, seq };
}

error_t BirthsSharedNoted(TagFSVolume* vol, uint64_t fid, const Strip* strips, uint64_t count, uint64_t seq)
{
    for (uint64_t i = 0; i < count; i++) {
        if (strips[i].kind != STRIP_SHARED)
            continue;
        if (!BirthRoom(vol, 2))
            return ERR_NO_MEMORY;
        BirthPut(vol, fid, strips[i].start, strips[i].start + strips[i].count, seq);
    }
    return OK;
}

uint64_t BirthsSharedOf(TagFSVolume* vol, uint64_t fid, uint64_t block)
{
    uint64_t i = BirthFrom(vol, fid, block);
    return i < vol->births_shared.n && vol->births_shared.birth[i].fid == fid && vol->births_shared.birth[i].start <= block ? vol->births_shared.birth[i].seq : 0;
}

void BirthsSharedLetGo(TagFSVolume* vol)
{
    kfree(vol->births_shared.birth);
    vol->births_shared.birth = NULL;
    vol->births_shared.n     = 0;
    vol->births_shared.cap   = 0;
}
