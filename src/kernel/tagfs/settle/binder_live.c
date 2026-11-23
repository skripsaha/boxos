#include "binder_internal.h"
#include "../block_map/block_map_derived.h"
#include "../ink/fallow.h"
#include "../mount/book_mount.h"
#include "../file_map/file_map.h"
#include "../ink/docket.h"
#include "../tag_registry/tag_registry.h"
#include "../use_memory/use_memory.h"
#include "tagfs_volume.h"

/*
 * Whether a leaf of the Tally before keeps a key of the state now. A leaf a
 * span of DogEar meets costs the next Tally what is left live in it: its
 * Sheets as it lies while a key of its range lives, none when none does — laid
 * anew it lays nothing, and only its parent, which drops it, is laid. So a
 * Tally after a large delete is cheap and owed at once, and the ribbon a mount
 * replays is bounded by the state that lives, not by what was given up (§4.2,
 * §4.7). What the next Tally is reckoned at (binder_cost.c) and a Tally's dry
 * count of itself (binder_sift.c, BinderSiftCount) ask it alike — one rule
 * with volume-read (tools/volumeread/binder.py Lives, dog_ear.py TipInBlocks).
 * The state is the one a slice takes: as the last settled stitch left it
 * (binder_slice.c).
 *
 *   FILES    a file of that state whose id is among the ids the leaf holds
 *            keys of (BinderLeafFids): a key of FILES is known by its file, so
 *            a file cut shorter keeps every leaf it began in — the map's
 *            record of a file no change since names, the Docket's `was` of one
 *            changed since and none of one born since, as the slice takes them
 *   TAGS     a tag of the registry in its range — the registry gives none up
 *   USE      the Use Context, while the volume remembers one — the slice's own
 *   DIGESTS  a block of its range a live file holds, or one held for what
 *            rotted (DIGESTS speaks for both): one the files took since the
 *            slice (DogEar's born ground), or one no file let go since (DogEar's
 *            gone ground) that an owner names in the block map and that is
 *            neither the book's own — a Tally-Quire or a Quire of the ribbon
 *            the Binder holds (BinderOwnPast) — nor a run resting in Fallow
 *            whose freer settled (FallowRestsPast); of the data run alone, so
 *            the front, the Spine's slots and the far Deed are no key either
 *
 * ‼ AT A QUIET POINT THE RULE IS EXACT; A WRITE UNDER WAY ONLY RAISES IT. A
 * block a write took for a file that no settled stitch names yet — a Plot's,
 * or one inked and not settled — is named in the map by no owner the Binder
 * or Fallow knows, so the leaf it lies in is counted live: the cost is
 * reckoned above what a Tally of the settled state lays by those leaves alone
 * — none once the write settles, when the block is born — and never below it.
 *
 * `taken` weighs a leaf by the ground the Tally under way took at its slice —
 * what moved since the Tally its leaves are of — else by what moved since the
 * last slice. By the Settle's holder or the mount, with no TagFS lock held:
 * the file map, the Docket, the registry, the block map and Fallow are asked
 * under the state lock or their own, a hold each.
 */

/* The first file at or past `from` of the state the last settled stitch left
 * — the one a slice takes (binder_slice.c, SliceBatch): the map's record of a
 * file no change since names, the Docket's `was` of one changed since, none of
 * one born since; UINT64_MAX for none. A hold of the state lock a file born
 * since: the Docket and the map are asked in one. */
static uint64_t SettledFileFrom(TagFSVolume* vol, uint64_t from)
{
    TagFSState* fs = &vol->state;
    while (from != UINT64_MAX) {
        uint64_t    fid = UINT64_MAX, jot = UINT64_MAX;
        FileRecord* was = NULL;
        TagFSLockTake(&fs->lock);
        bool changed = DocketWasFrom(vol, from, &jot, &was, NULL);
        if (FileMapFiles(fs->files, from, &fid, 1) != 1)
            fid = UINT64_MAX;
        TagFSLockDrop(&fs->lock);
        bool born = changed && jot <= fid && !was;
        FileRecordRelease(was);
        if (!born)
            return changed && jot < fid ? jot : fid;
        from = jot + 1u;
    }
    return UINT64_MAX;
}

static bool FilesLive(TagFSVolume* vol, const TallyPlace* p, bool first)
{
    uint64_t from = 0, to = 0;
    BinderLeafFids(p, first, &from, &to);
    return from < to && SettledFileFrom(vol, from) < to;
}

static bool TagsLive(TagFSVolume* vol, const TallyPlace* p, bool first)
{
    uint64_t    from = p->has_lo && !first ? p->lo.k1 : 0;
    uint64_t    to   = p->has_hi && p->hi.k1 < UINT32_MAX ? p->hi.k1 : UINT32_MAX;
    TagRolled   rolled;
    return from < to && TagRoll(vol->state.registry, (uint32_t)from, (uint32_t)to, &rolled);
}

static bool UseLives(TagFSVolume* vol)
{
    UseText* u    = tagfs_recall_use_context(vol, NULL);
    bool     live = u && u->bytes;
    UseTextDrop(u);
    return live;
}

/* The first run of `g` (`n`, in order, none meeting) that ends past `at`. */
static uint64_t RunPast(const BinderGround* g, uint64_t n, uint64_t at)
{
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2u;
        if (g[mid].start + g[mid].count <= at)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo;
}

/* A block of [lo, stop) the map names that neither the book's own ground nor
 * a run resting in Fallow is — a live file's: what is left of what the map
 * names, past each of those in turn. */
static bool FileTakenIn(TagFSVolume* vol, uint64_t lo, uint64_t stop)
{
    while (lo < stop) {
        uint64_t b = BlockMapDerivedTakenFrom(vol, lo, stop);
        if (b >= stop)
            return false;
        uint64_t past = BinderOwnPast(vol, b);
        if (past == b) {
            TagFSLockTake(&vol->state.lock);
            past = FallowRestsPast(vol, b);
            TagFSLockDrop(&vol->state.lock);
        }
        if (past == b)
            return true;
        lo = past;
    }
    return false;
}

/* A block of [lo, hi) — of the data run, where a file's ground lies — born
 * since the slice, or a live file's in the map and not in a run of the ground
 * let go since: the map asked between those runs. */
static bool GroundLive(TagFSVolume* vol, const TallyPlace* p, bool first, bool taken)
{
    uint64_t            nb = 0, ng = 0;
    uint64_t            lo = p->has_lo && !first ? p->lo.k1 : 0, hi = p->has_hi ? p->hi.k1 : UINT64_MAX;
    const BinderGround* born = taken ? DogEarTakenGround(vol, false, &nb) : DogEarHeldGround(vol, false, &nb);
    const BinderGround* gone = taken ? DogEarTakenGround(vol, true, &ng) : DogEarHeldGround(vol, true, &ng);
    lo = lo > vol->book.frame.data_block ? lo : vol->book.frame.data_block;
    hi = hi < vol->book.data_end ? hi : vol->book.data_end;
    if (lo >= hi)
        return false;
    uint64_t b = RunPast(born, nb, lo);
    if (b < nb && born[b].start < hi)
        return true;
    for (uint64_t g = RunPast(gone, ng, lo); lo < hi; g++) {
        uint64_t stop = g < ng && gone[g].start < hi ? gone[g].start : hi;
        if (lo < stop && FileTakenIn(vol, lo, stop))
            return true;
        if (stop == hi)
            break;
        lo = gone[g].start + gone[g].count;
    }
    return false;
}

bool BinderLeafLives(TagFSVolume* vol, uint64_t tree, const TallyPlace* p, bool first, bool taken)
{
    switch (tree) {
    case TALLY_FILES:
        return FilesLive(vol, p, first);
    case TALLY_TAGS:
        return TagsLive(vol, p, first);
    case TALLY_USE:
        return UseLives(vol);
    case TALLY_DIGESTS:
        return GroundLive(vol, p, first, taken);
    default:
        return true;
    }
}
