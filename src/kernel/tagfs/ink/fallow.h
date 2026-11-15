#ifndef TAGFS_FALLOW_H
#define TAGFS_FALLOW_H

#include "../tagfs.h"

/*
 * Fallow — ground the volume stopped naming, resting until nothing that could
 * still be read back off the medium names it.
 *
 * A block a file gives up is not free the moment memory says so. The record
 * that gave it up may still be only in memory, or in a stitch that a torn
 * write can take back, and the mount may stand on the stitch before it.
 * Handed to another file while any of that is so, the block would have two
 * owners the moment the power went. So it rests, and goes back to the
 * allocator only when all of these hold:
 *
 *   (1) the stitch that carries its freer settled (FallowStitchSettled), and
 *       the ribbon has left the Quire that stitch lies in — the first stitch
 *       begun past it settled (FallowQuireLeft); a stitch that runs on into
 *       its next_quire lies in the later one — never the atom it lies on,
 *       which the medium's page may tear with the next write;
 *   (2) FallowLag() settled stitches of this mount followed the one that
 *       named it;
 *   (3) no open handle of the file views it as it was before — one opened
 *       before the freer (its view_gen below the freer's gen) keeps the ground
 *       until it is closed (helm.h), so a reader's blocks outlive a delete.
 *
 * No digest is zeroed for it: a block's digest lives in the record that names
 * it, and its next owner brings its own. Its gives go to the block map derived
 * from the owners' names (block_map_derived.h), through pages held at the
 * cycle's TAKE (FallowGiveRoomHold). The ground a mount found the ribbon let
 * go rests the same way (M-1): named by the last stitch the mount read, held
 * until a stitch of this mount leaves its Quire and the lag's stitches of this
 * mount settled past it — what the mount read may have come out of the
 * medium's cache.
 *
 * Everything here is changed under the volume's state lock and nothing is
 * allocated under it: a node is made before the lock and freed after it.
 */

/* The settled stitches more a freer's stitch waits for. For a medium that
 * keeps its FLUSH, FALLOW_LAG — derived: the newest stitch can tear, and then
 * the mount stands on the stitch before it, one back and never two. Once the
 * medium lied — its ribbon shows a stitch it said it kept lost
 * (RibbonCursorLied) — FALLOW_LIED_LAG, a BUDGET and not a derivation: it
 * covers a lie one stitch deep; a deeper one is two owners of a block, which
 * the mount's census names (the design's addendum §3, "лаг Fallow 1 / 2"). */
#define FALLOW_LAG      1u
#define FALLOW_LIED_LAG 2u
uint64_t    FallowLag(void);

typedef enum {
    FALLOW_BY_RECORD = 1,      /* a record of a stitch, known by the gen of the Ink that wrote it */
    FALLOW_BY_SPINE,           /* a Tally's node or a Quire of the ribbon no slot holds */
} FallowKind;

typedef struct FallowNode FallowNode;

/* A node for the ground of `count` runs of file `fid` (0 for the book's own)
 * — DATA and SHARED, a hole passed over — taken with no TagFS lock held, with
 * the room the blocks of it more than one names may cut it into, and that room
 * asked of Commonhold too (FallowNodeCommonheld): NULL when there is no
 * memory. */
FallowNode* FallowNodeMake(const Strip* runs, uint32_t count, FallowKind kind, uint64_t fid);

/* A node for the ground the record `rec` names, of its file — every Strip of
 * ground, walked where the record holds them (file_record.h,
 * StripWalkNextGround), never laid flat — taken with no TagFS lock held, its
 * room as FallowNodeMake's: OK, with *out NULL for a record that names no
 * ground; ERR_NO_MEMORY, nothing made. */
error_t     FallowNodeOfRecord(const struct FileRecord* rec, FallowKind kind, FallowNode** out);

/*
 * ‼ A BLOCK MORE THAN ONE NAMES RESTS WHEN ITS LAST OWNER LETS IT GO
 * (ink/commonhold.h). An Ink, under the state lock and before the change is
 * put in, asks whether the node and Commonhold have the room the cut of its
 * ground takes as Commonhold stands now (FallowNodeCommonRoom) — the Ink goes
 * round when they do not; once its gen is given, the node's runs are cut to
 * the blocks the change lets go — one owner off each block in common, a block
 * another owner still names kept from it — the runs it is left with counted
 * (FallowNodeCommonheld): a node left with none is added to nothing and let go
 * by its Ink.
 */
bool        FallowNodeCommonRoom(const FallowNode* node);
uint32_t    FallowNodeCommonheld(FallowNode* node, uint64_t gen);

/* A node that was never added, let go. */
void        FallowNodeLetGo(FallowNode* node);

#ifdef CONFIG_VOLUME_TESTS
/* The blocks a node holds — for the stand. */
uint64_t    FallowNodeBlocks(const FallowNode* node);
#endif

/* Under the state lock: the node's runs come to rest, freed by `gen` — the
 * Ink's gen. The node is Fallow's from here on. */
void        FallowAdd(FallowNode* node, uint64_t gen);

/* Under the state lock, at the end of the `cycle`-th settled stitch, by the
 * rule above: every node whose lag has come, whose stitch's Quire the ribbon
 * left and whose file no open handle views as it was before goes back to the
 * allocator — as far as the pages held for the derived map reach: a run past
 * them rests on for the next cycle, said once until a cycle gives back all
 * that ripened. Answers the blocks given back; the nodes spent come back in
 * *spent, for FallowNodesFree after the lock. */
uint64_t    FallowRipen(uint64_t cycle, FallowNode** spent);

/* Under the state lock, at the DONE of stitch `seq`, the `cycle`-th
 * of this mount to settle, every Ink up to `target` in it: every record freer
 * up to `target` no stitch named yet is named by this one, and may go back
 * once FallowLag() stitches more settled — and the ribbon left its Quire. The
 * mount's nodes are named so by the last stitch it read, as cycle 0. */
void        FallowStitchSettled(uint64_t target, uint64_t seq, uint64_t cycle);

/*
 * Under the state lock, at a Tally's DONE (settle/binder_done.c): ground no
 * slot of the Spine holds any more — the nodes of the Tally its slot was
 * written over that no slot's Tally names, the Quires of the ribbon every slot
 * names its `at` later than (FALLOW_BY_SPINE, §3.4, M-7, L-12). The slot that
 * let go of it lies on the medium once its flush was kept, but a medium may
 * lie about a flush: so it rests as a stitch's freer does, for FallowLag()
 * stitches more to settle after the `cycle`-th — with no Quire to leave and no
 * handle to wait for, and goes back with them.
 */
void        FallowSpineAdd(FallowNode* node, uint64_t cycle);

/* Under the state lock: the least cycle a node of that ground still resting
 * goes back at, UINT64_MAX for none — what a slot counts its own past its
 * DONE is free once no node of it, ripe by its cycle, rests (binder_slots.c,
 * BinderRestRipens). */
uint64_t    FallowSpineRipeFrom(void);

/* Under the state lock: the ribbon left the Quire the stitches before `seq`
 * lie in — `seq` is the first stitch begun past it, and it settled — so every
 * node a stitch before it named waits for its lag alone from here. */
void        FallowQuireLeft(uint64_t seq);

/* Under the state lock: whether ground rests that a stitch named and only the
 * ribbon's leaving its Quire holds — what closing that Quire early gives back
 * (В1); and the blocks resting in a Quire the ribbon has left, which come back
 * with their lag and nothing more — not those an open handle still views as
 * they were, which no cycle gives back before the handle closes (RibbonClaim
 * waits for room only where cycles bring it). */
bool        FallowAwaitsQuire(void);
uint64_t    FallowLeftBlocks(void);

/* Under the state lock: the end of the run resting here that `block` lies in,
 * of a node whose freer settled — ground no settled state names any more, of
 * a file or of the book's own (FALLOW_BY_SPINE) — or `block` itself where none
 * does: one whose freer is still unsettled names ground the state the last
 * settled stitch left still holds. What a Tally is reckoned to cost passes
 * over it in a range of DIGESTS (settle/binder_live.c). */
uint64_t    FallowRestsPast(uint64_t block);

/* By the Settle's holder, with no TagFS lock held, at the TAKE of cycle
 * `cycle`: the pages the derived block map may want for the gives of every
 * node that may go back at the cycle's end, held (BlockMapDerivedHold), so
 * FallowRipen asks the heap for nothing. */
void        FallowGiveRoomHold(uint64_t cycle);

/* Under the state lock (fallow.c, NodeBarred): FallowBarsRun — whether
 * [start, start + count) meets ground a record's freer gave back that a Tally
 * a mount may stand on still names as a file's, which a Quire of the ribbon
 * may not take; *past where the last of it that meets the run ends.
 * FallowBarLift — `slice`, the stitch the oldest Tally a mount may stand on
 * took in, now stands (UINT64_MAX: none is known, nothing is kept from the
 * ribbon): what it no longer keeps from the ribbon, spent, to be freed with
 * no TagFS lock held (FallowNodesFree). */
bool        FallowBarsRun(uint64_t start, uint64_t count, uint64_t* past);
FallowNode* FallowBarLift(uint64_t slice);
#ifdef CONFIG_VOLUME_TESTS
/* For the stand: every free run kept from the ribbon as ground a freer named
 * by stitch `stitch` gave back — until a slice of `stitch` or past it stands
 * (FallowBarLift), as a delete's node is (NodeBarred); 0 lets it go. */
void        FallowStandBar(uint64_t stitch);
#endif

/* Nodes FallowRipen spent, freed with no TagFS lock held. */
void        FallowNodesFree(FallowNode* list);

/* Under the state lock: how many blocks rest. */
uint64_t    FallowBlocks(void);

/* The volume let go of: every node freed and nothing given back — what memory
 * held of the map is dropped with it. With the door shut. */
void        FallowDropAll(void);

#endif /* TAGFS_FALLOW_H */
