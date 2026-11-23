#ifndef TAGFS_COMMONHOLD_H
#define TAGFS_COMMONHOLD_H

#include "../tagfs.h"

/*
 * Commonhold — how many name each block a run SHARED names: a
 * block owned in common. A run SHARED (STRIP_SHARED, plot/strip.h) names
 * ground another run may name too — the one DATA run the block was written
 * by, other SHARED runs, or none once that owner has gone. The count is
 * memory's alone: no tree of it is written (§16.7 п.10); every mount derives
 * it from the names its census meets (mount/ground_shares.c), and every
 * change of a file on the volume keeps it.
 *
 * Held as runs of blocks of one count, in order, none meeting: every block a
 * live run SHARED names, its owners the DATA runs, the SHARED runs and — one
 * owner for all of them — what the mount held for a rotten range or a locked
 * file there, which no name that reads owns (§16.1 п.5). A block no run
 * SHARED names is not here: its one owner is its DATA run, and a block a
 * record stops naming that is not here is let go as it ever was.
 *
 * ‼ A BLOCK IS LET GO ONCE, BY THE LAST OF ITS OWNERS (§8.2). A change that
 * stops naming a block here takes one owner off it; the block rests in Fallow
 * only when that leaves none — the change of the last owner, whoever wrote
 * the block first. Taken under the TagFS lock that puts the change in the
 * mirror, so two files let go of one block in one order and one of them lets
 * it go.
 *
 * ‼ A STITCH ASKS WHO OWNED A BLOCK AS IT STOOD AT ITS SLICE. The run a change
 * left with no owner keeps the Ink that did it (`gone`) until the stitch that
 * carries that Ink settles: DogEar, at that stitch's S6, asks whether a block
 * a file let go was still owned by another at the stitch's slice — so the
 * next Tally keeps the block's one digest while an owner lives (§8.1), and
 * lets it go with the last (CommonholdOwnedAt).
 *
 * Under a leaf lock of its own, taken inside the TagFS lock by an Ink and
 * alone by the rest; the room a change takes is asked of the heap before any
 * lock (CommonholdRoomFor), and a change that finds it short under the lock
 * goes round. The mount's (with the door shut) builds it and lets it go.
 */

/* The run of a block in common: `count` blocks from `start`, `owners` names
 * each; `gone` the Ink whose change left it none — 0 while it has one. */
typedef struct {
    uint64_t start;
    uint64_t gone;
    uint32_t count;
    uint32_t owners;
} CommonRun;

/* Before any lock: room in Commonhold for `more` runs past those it holds.
 * ERR_NO_MEMORY. */
error_t  CommonholdRoomFor(TagFSVolume* vol, uint64_t more);

/* The runs `runs` (`n` Strips, ground and holes) may be cut into past their
 * number by blocks of them more than one names (*pieces), and the runs
 * Commonhold may be cut into past its own (*splits) by one change letting go
 * of all of them — as Commonhold stands now. */
void     CommonholdCuts(TagFSVolume* vol, const Strip* runs, uint64_t n, uint64_t* pieces, uint64_t* splits);

/* Whether Commonhold has room now for a change that cuts it into `splits`
 * runs more — under the TagFS lock, before the change is put in. */
bool     CommonholdRoomHolds(TagFSVolume* vol, uint64_t splits);

/* Under the TagFS lock, room held (CommonholdRoomHolds): one owner less on
 * every block of [start, start + count) a run of `kind` stops naming, by Ink
 * `gen`. The blocks it lets go — no owner left, or never here — handed to
 * `freed` in runs, in order; a block another owner still names is kept from
 * it. A run SHARED on a block not here is a DEFECT of the writer that put it
 * in: said, and its block kept rather than let go under another name. */
typedef void (*CommonholdFreedFn)(void* ctx, uint64_t start, uint64_t count);
void     CommonholdLetGoLocked(TagFSVolume* vol, uint64_t start, uint64_t count, uint32_t kind, uint64_t gen, CommonholdFreedFn freed,
                               void* ctx);

/* Under the TagFS lock, room held: one owner more on every block of [start,
 * start + count) — a run SHARED put on it, beside the one DATA run that names
 * a block not here yet. */
void     CommonholdShareLocked(TagFSVolume* vol, uint64_t start, uint64_t count);

/* How many name `block` — 0 when it is not here (one owner, or none). */
uint32_t CommonholdOwners(TagFSVolume* vol, uint64_t block);

/* The first run of blocks from *at, before `end`, that an owner held at the
 * slice of Ink `slice` — one here now, or let go by an Ink past it: [*lo, *hi),
 * and *at past it. False when none is. */
bool     CommonholdOwnedAt(TagFSVolume* vol, uint64_t* at, uint64_t end, uint64_t slice, uint64_t* lo, uint64_t* hi);

/* The stitch that carries every Ink up to `slice` settled, its DogEar told:
 * the runs those Inks left with no owner let go of, and a change's DEFECT
 * noted under the lock said, with no lock held. */
void     CommonholdSettled(TagFSVolume* vol, uint64_t slice);

/* The mount's, the door shut: every run let go; a run of a block in common
 * added past the last (in order, none meeting) — ERR_NO_MEMORY; the runs held,
 * read only, *n of them. */
void            CommonholdLetGoAll(TagFSVolume* vol);
error_t         CommonholdMountAdd(TagFSVolume* vol, uint64_t start, uint64_t count, uint32_t owners);
const CommonRun* CommonholdRuns(TagFSVolume* vol, uint64_t* n);

/* What one volume holds of commonhold.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    TagFSLock lock;
    CommonRun* run;  /* under commonhold.lock */
    uint64_t n;
    uint64_t cap;
    uint64_t wrong_at;  /* blocks let go that no count holds, said with no lock held */
    uint64_t wrong_count;
    bool wrong_said;
} CommonholdState;

void CommonholdVolumeBegin(TagFSVolume* vol);

#endif /* TAGFS_COMMONHOLD_H */
