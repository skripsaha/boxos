#ifndef TAGFS_SETTLE_BINDER_INTERNAL_H
#define TAGFS_SETTLE_BINDER_INTERNAL_H

#include "binder.h"

/*
 * The Binder, between its own files (binder.h): the Tally under way, which
 * binder.c runs a step at a time, and what one of its files lends another —
 * included by binder*.c alone, so no file outside the Binder sees a name of
 * it:
 *
 *   binder_items.c  the elements of each tree, for binder_nodes.c to pack
 *   binder_slice.c  the slice and the ground DIGESTS speaks for, for TALLY_TAKE
 *   binder_done.c   what TAKE makes for a Tally's end, its DONE, and a slot
 *                   whose write was refused
 */

/* binder_items.c: whether key `a` comes before `b` in `tree`; whether element
 * key `k` lies inside the range of rotten node `r`; the next element of the
 * tree's leaves, and of the level above the leaves being laid — the level
 * below's next node or rotten node — *got false at its end. OK, ERR_NO_MEMORY,
 * a refusal of a DIGESTS leaf the Tally stood on, or ERR_TAGFS_CORRUPTED. */
bool        BinderKeyLess(uint64_t tree, TallyKey a, TallyKey b);
bool        BinderRotCovers(const BinderRot* r, TallyKey k);
error_t     BinderLeafItemNext(BinderWork* w, BinderItem* it, bool* got);
error_t     BinderInnerItemNext(BinderWork* w, BinderItem* it, bool* got);

/* The Tally under way. */
typedef struct {
    bool        active;
    TallyStep   step;
    const char* stage;
    BinderWork  w;
    uint64_t    target;         /* every Ink the stitch settled — what a SETTLECUT asks it covers */
    uint64_t    k;              /* the stitch it is the state after, and its Clasp */
    uint32_t    clasp;
    SpineRibbon ribbon;
    SpineCounts counts;
    uint64_t    made_ns;
    uint64_t    useful_at;      /* the useful bytes up to k */
    uint64_t    used;           /* the blocks the nodes took in each run */
    uint32_t    slot;           /* the slot written over */
    uint64_t    slot_seq;
    uint64_t    chain_at;       /* the chain's index of the Quire `at` lies in */
    bool        sliced;         /* TALLY_TAKE: the slice taken, ... */
    bool        rots_read;      /* ... every DIGESTS leaf weighed, from rot_leaf on */
    uint32_t    rot_leaf;
    bool        turned;         /* TALLY_DONE: the reads of a digest turned to it — or left, said — ... */
    bool        captured;       /* ... and the Gloss may let go of what its DIGESTS names */
    bool        taken;          /* the runs at A and B are taken from the map */
    bool        slot_begun;     /* a write of the slot was issued: it may hold either Tally */
    SlotGround  fresh;          /* the ground the slot will hold, made at TAKE */
    BinderRot*  rot;            /* the nodes it names as they lie: the FILES ones the mount found, ... */
    uint32_t    nrot;           /* ... and every DIGESTS node no copy of reads, found at TAKE */
    uint32_t    caprot;
} BinderRun;

/* binder_slice.c: the slice of the state after the last settled stitch, every
 * live file's record — OK or ERR_NO_MEMORY; everything it holds let go; the
 * ground DIGESTS speaks for, in order of first block, the runs that meet
 * joined — OK or ERR_NO_MEMORY. */
error_t     BinderSliceTake(BinderWork* w);
void        BinderSliceLetGo(BinderWork* w);
error_t     BinderGroundTake(BinderWork* w);

/* binder_done.c: a DIGESTS node no copy of reads, for the Tally `ctx` names;
 * the FILES nodes the mount found so; the ground the new slot will hold, made
 * at TAKE; the Tally's DONE; a slot whose write or flush was refused held as
 * naming either Tally. */
error_t     BinderRotOfDigests(void* ctx, const TallyPlace* at);
error_t     BinderRotsTaken(BinderRun* r);
error_t     BinderFreshGround(BinderRun* r);
error_t     BinderTallyDone(BinderRun* r);
void        BinderSlotUnsure(BinderRun* r);

#endif /* TAGFS_SETTLE_BINDER_INTERNAL_H */
