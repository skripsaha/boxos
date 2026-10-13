#ifndef TAGFS_FALLOW_H
#define TAGFS_FALLOW_H

#include "../tagfs.h"

/*
 * Fallow — ground the volume stopped naming, resting until nothing that could
 * still be read back off the medium names it.
 *
 * A block a file gives up is not free the moment memory says so. The record
 * that gave it up may still be only in memory, or in the pool's tail — the
 * one block of the pool that is rewritten in place, and that a torn write can
 * take back to what it held before — and the medium's older copy of the
 * Ledger may still stand for the cycle before. Handed to another file while
 * any of that is so, the block would have two owners the moment the power
 * went. So it rests, and goes back to the allocator only when all of these
 * hold (the design, M1):
 *
 *   (1) the record that freed it lies in a CLOSED block of the pool — never
 *       rewritten again;
 *   (2) the cycle that settled that block has been followed by FallowLag()
 *       more: the Ledger's other copy has moved past it too;
 *   (3) no open handle of the file views it as it was before — one opened
 *       before the freer (its view_gen below the freer's gen) keeps the ground
 *       until it is closed (helm.h), so a reader's blocks outlive a delete —
 *       in memory: a stopping machine's last cycles write such ground free on
 *       the medium all the same (FallowViewedUnnamed);
 *   (4) no snapshot keeps it — a format-v1 volume takes none, and the ones an
 *       earlier kernel left keep the blocks their redirects moved away from,
 *       which no record names;
 *   (5) its zeroed digest has settled — the digests of a node are zeroed by
 *       the first cycle after it came to rest, just before that cycle writes
 *       the integrity map, and the node goes back only once a cycle at least
 *       that late has settled.
 *
 * A freer is one of four kinds. A record of the pool (FALLOW_BY_RECORD) is
 * known by the gen of the Ink that wrote it; a field of the Ledger
 * (FALLOW_BY_LEDGER) — the integrity map's old run — by the number of the
 * first cycle whose Ledger image was taken after the field changed, for which
 * (1) is moot. A chain of the Use Context the Ledger stopped naming
 * (FALLOW_BY_USE_CHAIN) is freed as a Ledger field is, and is held besides
 * while either copy of the Ledger may still name it — asked at the moment it
 * would go back, never when it came to rest (use_memory.h): a copy whose
 * write was not proven may name the chain it held or the one it was given,
 * and a mount that rolls back to the older copy stands on its chain. A block
 * Glean took off the pool chain's front (FALLOW_BY_UNLINK) is freed as a
 * Ledger field is — by the cycle whose seal first began the chain past it —
 * and is held besides while the Deed's head may still name it on the medium
 * (glean.h, GleanHintCovers): a reader that knows no seal walks from there.
 *
 * Everything here is changed under the volume's state lock and nothing is
 * allocated under it: a node is made before the lock and freed after it.
 */

/* The cycles more a freer's cycle waits for. For a medium that keeps its
 * FLUSH, VOLUME_LEDGER_COPIES − 1 — derived: the newest copy of the Ledger a
 * cycle wrote can tear, and then the mount stands on the copy before it, one
 * cycle back and never two. Once the medium lied — a volume whose seals say it
 * once held less than a settled cycle's seal (SETTLE_MEDIUM_LIED) —
 * FALLOW_LIED_LAG, a BUDGET and not a derivation: it covers a lie one cycle
 * deep; a deeper one is two owners of a block, which the mount's census names
 * (the design's addendum §3, "лаг Fallow 1 / 2"). */
#define FALLOW_LIED_LAG 2u
uint64_t    FallowLag(void);

typedef enum {
    FALLOW_BY_RECORD    = 1,
    FALLOW_BY_LEDGER    = 2,
    FALLOW_BY_USE_CHAIN = 3,   /* runs[0] begins with the chain's first link */
    FALLOW_BY_UNLINK    = 4,
} FallowKind;

typedef struct FallowNode FallowNode;

/* A node for `count` runs of file `fid` (0 for a Ledger freer's), taken with
 * no TagFS lock held: NULL when there is no memory. */
FallowNode* FallowNodeMake(const Strip* runs, uint32_t count, FallowKind kind, uint64_t fid);

/* A node for the ground the record `rec` names, of its file — every DATA
 * Strip, walked where the record holds them (file_record.h,
 * StripWalkNextGround), never laid flat — taken with no TagFS lock held: OK,
 * with *out NULL for a record that names no ground; ERR_NO_MEMORY, nothing
 * made. */
error_t     FallowNodeOfRecord(const struct FileRecord* rec, FallowKind kind, FallowNode** out);

/* A node with room for `blocks` blocks and none in it yet, for a freer that
 * learns its ground under the state lock (Glean): NULL when there is no
 * memory. FallowNodeKeep puts one block in it, under the state lock and
 * before it is added — a block beside the last joins its run; the count of
 * blocks it holds. */
FallowNode* FallowNodeRoom(uint32_t blocks, FallowKind kind);
uint32_t    FallowNodeKeep(FallowNode* node, uint64_t block);

/* A node that was never added, let go. */
void        FallowNodeLetGo(FallowNode* node);

/* Under the state lock: the node's runs come to rest, freed by `gen` — the
 * Ink's gen; 0 for a Ledger freer, whose cycle the next S0 names
 * (FallowLedgerTaken). The node is Fallow's from here on. */
void        FallowAdd(FallowNode* node, uint64_t gen);

/* Under the state lock, at S0 of cycle number `cycle`, as the Ledger's image
 * is taken: every Ledger freer not yet named — a field, or a Use Context
 * chain — is freed by this cycle. */
void        FallowLedgerTaken(uint64_t cycle);

/* By the Settle's holder, with no TagFS lock held, before the cycle numbered
 * `cycle` writes the integrity map: the digests of every node not yet zeroed
 * are zeroed (IntegrityForget), and the node says which cycle did it. And the
 * room the block map needs to write down the gives of every node that may go
 * back at this cycle's end is made and held now (BlockMapGiveRoomHold), so
 * FallowRipen asks the heap for nothing. */
void        FallowZeroDigests(uint64_t cycle);

/* Under the state lock: what the unsettled record written by Ink `from` had
 * freed is freed by Ink `to` now — the record was replaced in the tail by a
 * newer one of the same file, and the older one never reaches the medium. */
void        FallowRetag(uint64_t from, uint64_t to);

/*
 * Under the state lock, at S0 of a cycle that writes the Deed's head to name
 * the chain's first block as the last settled seal has it: every block taken
 * off the chain by a cycle up to `upto` — and the node `also`, the ones the
 * mount found the head naming (GleanClaimGround) — lies before that block, and
 * once the write lands nothing on the medium names it. Each is left free in
 * every image of the map from this cycle's S1 on (FallowUnnamedLeftOut), its
 * sector owed now, while memory keeps it at rest: a cycle settled with the
 * head written leaves no ground that nothing owns, and a mount that rolls back
 * to an older seal finds such a block named and free, and takes it again.
 */
void        FallowUnlinkedUnnamed(uint64_t upto, FallowNode* also);

/*
 * Under the state lock, at S0 of one of a stopping machine's last cycles
 * (TagFSSettleForHalt): every record freer's node that rested its cycles by
 * the settled cycle `settled`, whose zeroed digests a settled cycle wrote, and
 * which only an open handle's view from before its freer still holds (3) is
 * left free in every image of the map from this cycle's S1 on, its sectors
 * owed now, while memory keeps it at rest. The machine stops before that
 * handle is closed: a node held for it would be a block taken on the medium
 * that nothing names, for good, and a reader still running until the stop
 * reads what it read, since no other file is handed the block.
 */
void        FallowViewedUnnamed(uint64_t settled);

/* Under the state lock, as the block map sends a sector out: every resting
 * block of [first, first + count) nothing on the medium names, cleared in
 * `bits` (one bit per block from `first`); how many. */
uint32_t    FallowUnnamedLeftOut(uint64_t first, uint32_t count, uint8_t* bits);

/* Under the state lock, at the end of cycle number `cycle`: every node whose
 * freer now lies in a settled closed block (gen <= closed_gen) is given the
 * cycle it may go back at, and every node whose cycle has come, whose digests
 * a settled cycle zeroed, whose file no open handle views as it was before,
 * and — a Use Context chain — which no copy of the Ledger may name, or — an
 * unlinked pool block — which the Deed's head no longer names, goes back
 * to the allocator — as far as the room held for the map's deltas reaches: a
 * run past it rests on for the next cycle, said once until a cycle gives back
 * all that ripened. Answers the blocks given back; the nodes spent come back
 * in *spent, for FallowNodesFree after the lock. */
uint64_t    FallowRipen(uint64_t closed_gen, uint64_t cycle, FallowNode** spent);

/* Under the state lock: whether ground rests on a record newer than
 * `closed_gen` — one in the pool's open tail, whose ground no cycle gives back
 * until a cycle closes the tail (1). */
bool        FallowRestsOnTail(uint64_t closed_gen);

/* Nodes FallowRipen spent, freed with no TagFS lock held. */
void        FallowNodesFree(FallowNode* list);

/* Under the state lock: how many blocks rest — and of them, how many every
 * image of the map still writes taken (the rest are left out of it). */
uint64_t    FallowBlocks(void);
uint64_t    FallowBlocksMapTaken(void);

/* The volume let go of: every node freed and nothing given back — what memory
 * held of the map is dropped with it. With the door shut. */
void        FallowDropAll(void);

#endif /* TAGFS_FALLOW_H */
