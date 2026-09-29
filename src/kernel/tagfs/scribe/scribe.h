#ifndef TAGFS_SCRIBE_H
#define TAGFS_SCRIBE_H

#include "../../../lib/kernel/ktypes.h"
#include "../../core/error/error.h"

/*
 * Scribe — who writes a structure of the volume, and what is already durable.
 *
 * Two questions every writer of the volume's metadata has to be able to ask,
 * and until now could only answer by holding an interrupts-off lock across the
 * medium:
 *
 *   which cache flush proves that a write I have already seen complete is on
 *   the medium? — the flush tickets, VolumeFlush*;
 *
 *   is somebody writing this structure right now, and when he is done, did he
 *   write what I changed? — the Scribe.
 *
 * Nothing here takes a lock, allocates, or is ever reset: the counters are
 * monotonic for the life of the boot, across every mount and every medium,
 * and that monotony is part of what makes them right.
 */

// ============================================================================
// Flush tickets
//
// A write that COMPLETED is a write the device received, into a cache it can
// lose; a cache flush issued AFTER that completion is what puts it on the
// medium. So a flush is worth something to a write only when it was issued
// after the write's completion was seen — and "a flush succeeded at some
// point" says nothing about that. A count of successful flushes raised once
// the flush answers is wrong the same way: core A's flush is issued and done,
// A is interrupted before it counts; core B completes a write and reads the
// count; A counts, and B's write is declared covered by a flush that finished
// before B's write was issued.
//
// So every flush takes a ticket BEFORE it is issued, and a write asks, after
// its completion was seen, for the smallest ticket that could cover it. The
// locked add that hands out a ticket is a full barrier, and the command that
// issues the flush is a store to the device that cannot pass it, so a flush
// carrying a ticket >= need was issued after need was read. Of the flushes
// the medium kept, the highest ticket is remembered; a refusal moves nothing.
//
// The only place a ticket is taken and answered is tagfs_flush_cache. The
// flushes a growing volume makes of its Deed before its door opens go past it:
// an unticketed flush can only make a need look unanswered for longer, never
// answered early.
// ============================================================================

/* Taken by tagfs_flush_cache immediately before it asks the medium. */
uint64_t VolumeFlushBegin(void);

/* The medium's answer to the flush carrying `ticket`. Only a flush the medium
 * kept moves the mark. */
void     VolumeFlushEnd(uint64_t ticket, bool kept);

/* Read AFTER a write's completion was observed: the smallest ticket whose
 * kept flush proves that write is on the medium. */
uint64_t VolumeFlushNeed(void);

/* Has a flush carrying a ticket >= need been kept? */
bool     VolumeFlushKept(uint64_t need);

/*
 * Make `need` durable: OK at once, with no word to the medium, when a kept
 * flush already covers it; otherwise a flush, and the medium's answer as it
 * is.
 *
 * There is no asking again. Two flushes of the same volume meeting is
 * ordinary traffic, and it used to reach here as ERR_BUSY — another
 * whole-port command holding the port at that instant — which this asked
 * about again in a loop. The meeting is now settled where it happens: the
 * second flush waits in the port's roadstead (core/boardroom/roadstead.h) for
 * the first to give the port back, so the answer that comes out of the medium
 * is always about the medium.
 */
error_t  VolumeFlushFor(uint64_t need);

// ============================================================================
// Scribe — one writer of a structure at a time, and a caller who waited sees
// whether his change was written
//
// A caller changes the structure in memory, under the structure's own lock,
// lets the lock go and asks for a ticket. Whoever then holds the Scribe takes
// one snapshot of everything asked so far — that is its `cover` — and writes
// it with no TagFS lock held. A caller whose ticket a finished pass covered is
// answered by that pass (WRITTEN or REFUSED) and does no I/O at all; one whose
// ticket no pass covered yet takes the next pass itself.
//
// THE CONTRACT, which every consumer keeps:
//
//   1. Ask after the change is in memory and the structure's lock is let go.
//   2. Snapshot after MINE, under the structure's lock; I/O outside every
//      TagFS lock.
//   3. Take or wait for a Scribe with no TagFS lock held — a core spinning for
//      a Scribe with interrupts off would never hear the completion the holder
//      is waiting for — and nest Scribes only downward: sync, then registry,
//      then ledger. The witness names a Scribe asked for under a TagFS lock
//      ([LockRank] ... held across a wait for a Scribe), and a "[Scribe]" line
//      names one asked for out of order; both let the work go on.
//   4. A pass that fails keeps what it did not write owed in memory, so the
//      next pass writes it again: a later pass covers every earlier ticket.
//   5. A module's I/O buffer belongs to whoever holds its Scribe.
//   6. No baton continuation takes a Scribe. The drain core runs them while it
//      waits for one, and a continuation asking for a Scribe that core holds
//      beneath it would be answered SELF.
//   7. Give on the core that took it, holding no spinlock of any kind: an
//      orderly halt that found this core in a pass stops the core right there
//      (below).
//
// THE HOLDER IS NEVER SWITCHED AWAY FROM. MINE holds the scheduler off this
// core's kernel context (SchedulerHoldHere) until the Give: a pass run by the
// idle context of an application core, or of a single-core machine, would
// otherwise stop mid-write whenever a process became ready, and every other
// caller would wait on it for as long as that process ran. Interrupts stay on.
//
// AND IT IS NEVER STOPPED IN THE MIDDLE OF A PASS. A core an orderly halt asks
// to stop while it holds a Scribe does not stop there: it owes the halt its
// stop, runs its pass to the end, and stops in the ScribeGive that ends the
// last span it holds (HaltFreezeHere). A pass is never taken over from a stopped
// core, because none is ever stopped holding one — and a command a stopped
// core had in flight can never land over the next pass's. A panic stops every
// core at once, as it always did.
// ============================================================================

typedef enum {
    SCRIBE_RANK_SYNC     = 1,   /* tagfs_sync, the whole pass */
    SCRIBE_RANK_REGISTRY = 2,
    SCRIBE_RANK_LEDGER   = 3    /* innermost */
} ScribeRank;

typedef enum {
    SCRIBE_MINE,        /* the caller holds it: run the pass, then ScribeGive */
    SCRIBE_WRITTEN,     /* a kept pass that began after the ask wrote it */
    SCRIBE_REFUSED,     /* the pass that covered the ask failed; *refusal says why */
    SCRIBE_HELD,        /* (ScribeTry only) another core holds it */
    SCRIBE_SELF         /* this very core holds it, beneath the caller */
} ScribeTurn;

typedef struct Scribe {
    volatile uint32_t holder;     /* 0, or the holding core's index + 1 */
    uint8_t           rank;       /* ScribeRank */
    const char       *what;       /* the structure, as a line names it */
    volatile uint64_t asked;      /* tickets handed out */
    volatile uint64_t written;    /* the highest ticket a kept pass covered */
    volatile uint64_t refused;    /* the highest ticket a failed pass covered */
    volatile uint32_t refusal;    /* error_t of the latest failed pass */
} Scribe;

#define SCRIBE_INIT(rank_, what_) { .holder = 0, .rank = (rank_), .what = (what_) }

/* The volume's sync: held by tagfs_sync for its whole pass. */
extern Scribe g_sync_scribe;

uint64_t   ScribeAsk (Scribe *s);

/* Answered, taken, or not now — never a wait. */
ScribeTurn ScribeTry (Scribe *s, uint64_t ticket, uint64_t *cover, error_t *refusal);

/* Answered or taken; waits while another core holds it, with interrupts as
 * the caller left them, running the drain core's continuations meanwhile.
 * SELF at once rather than a wait that would never end. */
ScribeTurn ScribeWait(Scribe *s, uint64_t ticket, uint64_t *cover, error_t *refusal);

/* The end of the pass that MINE began: `cover` as MINE gave it, and the
 * pass's own answer. */
void       ScribeGive(Scribe *s, uint64_t cover, error_t result);

bool       ScribeHeldHere(const Scribe *s);
bool       ScribeAnyHeldHere(void);

/* The same wait for a turn somebody else keeps: SCRIBE_WRITTEN once
 * settled(ctx) is true, SCRIBE_SELF at once when *holder names this core.
 * Every turn of it — and of ScribeWait's — runs the drain core's
 * continuations when it stands on the drain core. */
ScribeTurn ScribeWaitFor(const volatile uint32_t *holder, bool (*settled)(const void *ctx),
                         const void *ctx);

// ============================================================================
// Spare — a block stamped before anything names it
//
// THE SPARE RULE. No next_block on the medium names a block until
//   (1) the write of its stamp returned OK,
//   (2) need = VolumeFlushNeed() was read after that, and
//   (3) VolumeFlushFor(need) == OK.
// A power cut may keep any subset of the writes since the last kept flush, so
// a stamp written in the same flush epoch as the link to it may be lost while
// the link survives: the next mount then follows the link into ground that
// holds whatever it held. The stamp must be in an EARLIER epoch than the link,
// and a kept flush between them is the only thing that says so.
// ============================================================================

/*
 * One block from the allocator, stamped with an empty header carrying `magic`
 * — next_block 0, nothing in it, the shape volume-read calls a Spare — written
 * from `image`, the caller's own TAGFS_BLOCK_SIZE buffer. OK: *out_block is
 * stamped on the medium and *out_need is the need its link must wait for. A
 * refused write gives the block back. Called inside the door, under the
 * consumer's Scribe, with no TagFS lock held.
 */
error_t SpareStamp(uint32_t magic, void *image, uint32_t *out_block, uint64_t *out_need);

#endif /* TAGFS_SCRIBE_H */
