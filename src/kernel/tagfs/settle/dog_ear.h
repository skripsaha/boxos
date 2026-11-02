#ifndef TAGFS_SETTLE_DOG_EAR_H
#define TAGFS_SETTLE_DOG_EAR_H

#include "../file_map/file_record.h"
#include "../mount/tally_read.h"
#include "binder.h"

/*
 * DogEar — the keys of the book's trees changed since the Tally the next one
 * is laid against (spec_v2 §16.5 п.9, §4.3 TALLY_TAKE): spans of keys
 * [lo, hi), tree by tree — FILES, TAGS, USE, DIGESTS — put in by
 *
 *   a stitch that settles, at its S6 (sewing.c): every file it carried a
 *     record of (DogEarFileChanged), the tags it coined, the Use Context it
 *     carried;
 *   a mount, by the ribbon it replays past its slot (ribbon_transcribe.c):
 *     every file a replayed stitch changed, the same way;
 *   a copy of a leaf that would not read (DogEarMended) — the mount's walk,
 *     a read of a digest, the Binder's self-check: the next Tally writes the
 *     leaf again (§2.5);
 *   Escheat, at the TALLY_TAKE of the Tally that owes it (escheat.c): the
 *     rotten ranges, the files locked with them, and the ground it lets go
 *     (DogEarGroundChanged).
 *
 * ‼ A TALLY TAKES WHAT IS HELD AT ITS SLICE, AND ONLY THAT. TALLY_TAKE takes
 * every span held (DogEarTake) — the keys the stitches up to k changed — and
 * the spans put in after it are held for the Tally after it; refused, it gives
 * what it took back under what came since; its DONE lets it go.
 *
 * ‼ A HEAP THAT REFUSES DOGEAR ROOM LOSES IT WHOLE (§4.2: a demand, not a
 * number). Every key is then changed: the loss is said once, a Tally is
 * demanded, and the Tally that takes it — whole — ends it.
 *
 * The spans are kept joined, in order of their keys, under a leaf lock that
 * holds nothing but them; the room a put needs is asked of the heap before the
 * lock. Put from any core with no TagFS lock held — a read of a digest puts in
 * too; taken, given back and let go by the Settle's holder, the one reader of
 * what a Tally took; let go whole by the mount, with the door shut.
 */

typedef struct {
    TallyKey lo;
    TallyKey hi;      /* past the span: DOG_EAR_END for a span to its tree's end */
} DogEarSpan;

#define DOG_EAR_END ((TallyKey){ UINT64_MAX, UINT64_MAX })

/* The keys [lo, hi) of `tree` changed. */
void     DogEarPut(uint64_t tree, TallyKey lo, TallyKey hi);

/* File `fid` changed from `was` to `now` — NULL for a file born, or given up:
 * its header and every Parcel of either record not shared with the other at
 * one place of the file, the whole id for a file born or given up; and every
 * window of DIGEST_ITEM_MOST blocks whose ground one of the two names and the
 * other does not. By the Settle's holder or the mount, with no TagFS lock. */
void     DogEarFileChanged(uint64_t fid, const FileRecord* was, const FileRecord* now);

/* The leaf at `at` of `tree`, a copy of which did not hold: its range. */
void     DogEarMended(uint64_t tree, const TallyPlace* at);

/* Ground [start, start + count) whose digests changed with no record of a
 * file to say so — the held ground Escheat lets go: every window of
 * DIGEST_ITEM_MOST blocks it lies in, as DogEarFileChanged takes a file's. A
 * DIGESTS element is blocks in a row inside one window (volume_tally.h), so
 * ground let go out of the middle of one leaves the blocks past it in the
 * window an element of their own, keyed by the first of them — a key the
 * Tally before never had, and none of the ground's own. With no TagFS lock. */
void     DogEarGroundChanged(uint64_t start, uint64_t count);

/* TALLY_TAKE: every span held taken by the Tally; refused, given back under
 * what came since; its DONE, let go. Whether the Tally under way took `key` of
 * `tree` changed — every key, when what it took was lost — the spans of `tree`
 * it took, in key order (*n of them), and whether it took a loss. By the
 * Settle's holder. */
void     DogEarTake(void);
void     DogEarTakenBack(void);
void     DogEarTakenLetGo(void);
bool     DogEarTakenHolds(uint64_t tree, TallyKey key);
const DogEarSpan* DogEarTakenSpans(uint64_t tree, uint32_t* n);
bool     DogEarTakenLost(void);

/* The ground the live files took since the slice before the one the Tally
 * under way took and hold (`gone` false), or held at that slice and let go
 * since (`gone`) — a block let go and taken again in both, its data new —
 * exactly, in runs in order, *n of them, as every change of a file put it in:
 * what DIGESTS laid
 * in among the leaves of the Tally before lays anew of them (§16.5 п.16).
 * Refused, given back under what moved since as one change. By the Settle's
 * holder. */
const BinderGround* DogEarTakenGround(bool gone, uint64_t* n);

/* The ground the live files took since the last slice and hold (`gone`
 * false), or held at it and let go since (`gone`) — runs in order, *n of
 * them, as DogEarTakenGround gives the Tally's: a Tally's DONE leaves the
 * digests of the first in the Gloss, which alone holds them, though the slice
 * it laid named the block (binder_done.c); what the next is reckoned to cost
 * weighs a leaf of DIGESTS by both (binder_live.c). By the Settle's holder. */
const BinderGround* DogEarHeldGround(bool gone, uint64_t* n);

/* How many changes put in since the volume was mounted may have left a leaf
 * with no key of the state in it: a file given up, ground of one let go that
 * may leave a leaf of DIGESTS with none — not where the file, or a file that
 * took ground since the slice, still holds a block of the leaf — a Use
 * Context said. From any core. */
uint64_t DogEarGoneCount(void);

/* The spans of `tree` held since the last slice from the `from`-th on, into
 * `out` (room `room`): how many — a few at a hold of the lock, so a span put
 * meanwhile may be passed over until the next look; and whether what is held
 * was lost. By the Settle's holder (binder_cost.c). */
uint32_t DogEarHeldSpans(uint64_t tree, uint32_t from, DogEarSpan* out, uint32_t room);
bool     DogEarHeldLost(void);

/* Everything DogEar holds let go — a mount begun, the volume let go of. */
void     DogEarLetGo(void);

#ifdef CONFIG_VOLUME_TESTS
/* The spans of `tree` held since the last slice — or, `taken`, the ones the
 * Tally under way took — into `out` (room `room`): how many there are. Whether
 * that set was lost. */
uint32_t DogEarStand(uint64_t tree, bool taken, DogEarSpan* out, uint32_t room);
bool     DogEarStandLost(bool taken);
#endif

#endif /* TAGFS_SETTLE_DOG_EAR_H */
