#ifndef TAGFS_GLEAN_H
#define TAGFS_GLEAN_H

#include "../tagfs.h"

/*
 * Glean — the pool's live records gathered into its tail, and the blocks they
 * leave taken off the chain's front.
 *
 * The pool is only appended to (meta_pool.h): a file changed ten thousand
 * times across ten thousand cycles leaves ten thousand records, and all but
 * the newest are dead — yet every one of them lies on the chain, and the
 * mount walks the whole chain. Glean bounds it. Before each cycle's S0 (a
 * machine stopping excepted), a step at a time:
 *
 *   a block at the chain's front the map of live files names nothing in is
 *     taken off the chain, in every cycle, for nothing — no read, no write;
 *   while the chain is at least GLEAN_SLACK times the blocks its live records
 *     would fill, plus one, the block at its front is read, each record in it
 *     that is still its file's newest is laid again at the tail's end by an Ink
 *     of its own (InkGleanLocked), and the block is taken off.
 *
 * ‼ ONLY THE FRONT, AND THE SEAL SAYS WHERE THE CHAIN BEGINS. Taking a block
 * off is moving chain_first in the seal the cycle's S4 writes; nothing on the
 * medium is written over for it — a closed block never is. The seal names the
 * new front and the tail holding the moved records in one sector, so a cut
 * cycle leaves the chain as the cycle before it had it: the blocks taken off
 * rest in Fallow, whole, until both copies of the Ledger have moved past them.
 *
 * ‼ A QUITCLAIM IS LEFT BEHIND, ONCE NO OLDER COPY OF ITS FILE REMAINS — and at
 * the front none ever does. A file's records lie in the order they were
 * inked, a moved one included (it is its file's newest when it moves, and
 * lands past every record there is), so every older record of a file whose
 * Quitclaim reaches the front lies before it — in the blocks already taken
 * off, or before it in its own. Nothing the map does not name is moved, and so
 * a Quitclaim is dropped with the block that holds it.
 *
 * ‼ THE DEED'S HEAD IS NEVER TAKEN (RT-08). A kernel before the seal, and a
 * reader that knows none, walk the pool from the block the Deed names. When
 * the front Glean takes is that head, its live records move and the chain
 * begins after it, but the block stays the pool's; and once the chain begins
 * past it, its next_block — a hint for those readers, which no seal counts —
 * is written again to name the chain's first block, in the S1 of the cycle
 * after the one whose seal settled that first block, its records left as they
 * were. A block taken off rests in Fallow until the head no longer names it
 * on the medium (GleanHintCovers), so no reader's walk ever runs into ground
 * that went back to the allocator.
 *
 * Everything below is the Settle's holder's — one cycle at a time — and the
 * mount's with the door shut; what it changes of the pool and the map it
 * changes under the volume's state lock, with nothing there that reads the
 * medium or asks the heap.
 */

/*
 * ‼ GLEAN_SLACK IS A POLICY, NOT A DERIVATION. How many times the blocks its
 * live records fill the chain may grow before Glean moves records: a point on
 * the curve between the mount's time — which walks the whole chain — and the
 * pool's write amplification — which every moved record adds to. The premise
 * that a chain of S times its live blocks is 1/S live at its front is false:
 * under uniform overwrites the front's live share u solves u = e^-(1-u)S (at
 * S = 2, u ≈ 0.2), and under a hot and cold load u goes to 1. So the number is
 * measured for, not argued: the chain's length and the bytes moved per byte
 * inked, on both loads (the stand, GleanCountsRead). A volume whose allocator
 * is short while Fallow has nothing to give back lowers it to one and a
 * sliver: any block more than the live records and the open tail need.
 */
#define GLEAN_SLACK  2u

/* A cycle that gleans says so first — its pass begins (settle.c), and the pass
 * before it, when it was one at the lowered slack, is judged by the chain it
 * left. */
void     GleanPassBegin(void);

/* One step of the pass, with no TagFS lock held — at most one command to the
 * medium, and the committer back in its loop between two: *done once nothing
 * is left for this cycle — the chain within its slack, a front Glean may not
 * take yet, or no Spare to move onto. A pass reads each block of the chain at
 * most once: what it moves lands in blocks no cycle has written, which it
 * never takes. What a long pass costs the medium is the cycle's own time, and
 * the Lull that follows it grows with it. Glean never refuses a cycle: what it
 * could not do it says once, and leaves to the next. */
void     GleanStep(bool* done);

/* The head's rewrite, taken at S0 under the state lock and written at S1 —
 * one step to read the block when memory does not hold it, one to write it.
 * A refused write never refuses the cycle: the head is a hint, the blocks it
 * may still name stay taken, and the next cycle writes it again. */
typedef struct {
    bool     write;       /* the head's next_block is owed */
    bool     written;
    uint32_t to;          /* the chain's first block as the last settled seal named it */
} GleanHead;

/* A write owed leaves free in the map's images, from this cycle's S1 on, every
 * resting block it names past (FallowUnlinkedUnnamed). */
void     GleanHeadTake(GleanHead* h);
void     GleanHeadStep(GleanHead* h, bool* done);

/* S6, under the state lock, cycle `cycle` settled with its seal's chain_first
 * `first`: what the head names on the medium from now on. */
void     GleanHeadSettled(const GleanHead* h, uint64_t cycle, uint32_t first);

/* Under the state lock: whether the Deed's head is off the chain — the pool's
 * still, and none of the chain's links. */
bool     GleanHeadOffChain(void);

/* The newest cycle every block taken off by it or before it the head no
 * longer names on the medium: an unlinked block of a later one stays in
 * Fallow. */
uint64_t GleanHintCovers(void);

/* Under the state lock: whether the chain holds more blocks than its live
 * records and its open tail need — ground a pass at the lowered slack gives
 * back through Fallow — and the lowered slack is not lifted: a cycle at it
 * that left the chain no shorter lifts it until a block's worth of records
 * has died (GleanPassBegin), so a take short of room hears there is none
 * rather than wait on cycles that give nothing back. */
bool     GleanMayFree(void);

/* The allocator is short with nothing resting: the next pass gleans at the
 * lowered slack, unless it is lifted. From any context. */
void     GleanShort(void);

/*
 * The mount, with the door shut, once the pool is walked: `head` the Deed's
 * block of the pool, `first` the block the chain the mount stands on begins
 * at. When the chain begins past the head, the head is read and what its
 * next_block names is followed up to that first block — blocks an earlier
 * cycle took off whose head the medium had not yet seen written again, or
 * whose rest the power cut. They are the pool's until the head names past
 * them. Nothing is refused here: a head that does not read, or a walk that
 * does not reach the chain, is said, and the head is written again.
 */
void     GleanMount(uint32_t head, uint32_t first);

/* The mount's census: the blocks GleanMount found claimed as the pool's, and
 * rested in Fallow until the head no longer names them. */
void     GleanClaimGround(void);

/* The volume let go of: nothing Glean holds of it stays. */
void     GleanUnmount(void);

#ifdef CONFIG_VOLUME_TESTS
/* What Glean did since the mount — for the stand's measures. */
typedef struct {
    uint64_t passes;          /* passes that moved or took anything */
    uint64_t reads;           /* front blocks read */
    uint64_t moved_records;
    uint64_t moved_bytes;
    uint64_t unlinked;        /* blocks taken off the chain, the head among them */
    uint64_t head_writes;     /* the head's next_block written and settled */
} GleanCounts;

void     GleanCountsRead(GleanCounts* out);
#endif

#endif /* TAGFS_GLEAN_H */
