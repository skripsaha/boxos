#ifndef TAGFS_USE_MEMORY_H
#define TAGFS_USE_MEMORY_H

#include "../../../lib/kernel/ktypes.h"
#include "../../core/error/error.h"
#include "../../core/use_context/use_context.h"

/*
 * The volume's memory of the Use Context — what its person was doing when
 * they left (use_context.h), carried whole by a stitch as a UseContext and
 * held in the book's Tally (spec_v2 §7).
 *
 * ‼ A CHANGE IS A DEMAND ON THE SETTLE CYCLE, NOT A WRITE. What the person
 * says is handed over (tagfs_remember_use_context) and a cycle carries it in
 * its stitch. What the medium already holds is never written again, and a
 * stitch with no room for the word forgets the old one rather than
 * remembering a word the person no longer says (the owner's rule, 09-08).
 *
 * Everything here is changed under the volume's state lock and nothing is read
 * off or written to the medium, or asked of the heap, under it.
 */

// ----------------------------------------------------------------------------
// The person's word, and the volume's
// ----------------------------------------------------------------------------

/*
 * remember: say `said` (the comma-joined text; NULL says none), the `saying`-th
 * word the machine has said, to the volume. What the medium already holds is
 * not written again; otherwise it is a demand, and a Settle cycle that began
 * after it carries it. A word older than one the volume was already told
 * changes nothing: two sayings that race leave the later one whole.
 * *remembered reports whether the volume now holds exactly this text — false
 * with no room for it, a cycle refused, or a later word standing. Inside the
 * volume's door with no TagFS lock held.
 *
 * recall: what the volume remembers, in one take — a reference the caller
 * drops (UseTextDrop), or NULL when it remembers none or this machine could not
 * hold it — and in *out_saying (optional) which word it is, 0 for the medium's
 * own.
 */
error_t  tagfs_remember_use_context(UseText *said, uint64_t saying, bool *remembered);
UseText *tagfs_recall_use_context(uint64_t *out_saying);

/* remember's first half, which never waits: the word said to the volume's
 * memory, and what the medium makes of it — a later word stands; the medium
 * holds exactly it already; or it is a demand a cycle that begins after it
 * carries (OWED), whose number goes to *ticket. A program's `use` waits for an
 * OWED one through its Chit (SettleWaitLinkCycle). Inside the door, with no
 * TagFS lock held. */
typedef enum {
    USE_SAID_SUPERSEDED = 0,
    USE_SAID_LANDED,
    USE_SAID_OWED,
} UseSaid;
UseSaid  UseMemorySay(UseText *said, uint64_t saying, uint64_t *ticket);

/* remember's second half, once a cycle that began after demand `ticket` has
 * settled: whether the volume now holds exactly `said` — false when it had no
 * room for it, or a later word stands; and whether demand `ticket` was the
 * last a stitch had no room for, and refused. With no TagFS lock held. */
bool     UseMemoryKept(uint64_t ticket, const UseText *said);
bool     UseMemoryRefused(uint64_t ticket);

// ----------------------------------------------------------------------------
// The Settle cycle's part (settle/sewing.c), by the Settle's holder
// ----------------------------------------------------------------------------

/* The stitch carries the word whole as a UseContext (spec_v2 §7). Take, at
 * the cycle's TAKE under the state lock: whether a word is owed the medium, and then the word — held, the
 * caller's to drop; NULL says none — and its demand's number. Settled, at DONE
 * under the state lock: the volume holds `said` (the reference is the
 * volume's from here), the demand `ticket` is answered, and the word it held
 * before is handed back to be dropped once the lock is let go. Refused, at
 * DONE under the state lock, when the stitch had no room for the word and was
 * laid without it: the demand is answered as refused, the word no longer owed
 * where no later one stands, and — `forgot`, the stitch laid an empty
 * UseContext in its place — the volume holds no word, the one it held handed
 * back to be dropped (the owner's rule, 09-08); without, it keeps that word,
 * and NULL is handed back. */
bool     UseMemoryStitchTake(UseText **said, uint64_t *ticket);
UseText *UseMemoryStitchSettled(UseText *said, uint64_t ticket);
UseText *UseMemoryStitchRefused(uint64_t ticket, bool forgot);

// ----------------------------------------------------------------------------
// The mount (with the door shut)
// ----------------------------------------------------------------------------

/* The book's Use Context, as its Tally holds it (spec_v2 §7 Recall): `len`
 * bytes of `text`, or none. It is the medium's memory, landed. OK, or
 * ERR_NO_MEMORY. */
error_t  UseMemoryRecall(const uint8_t *text, uint64_t len);

/* The volume is let go of: everything held of its memory, with not one byte
 * written. */
void     UseMemoryLetGo(void);

#endif /* TAGFS_USE_MEMORY_H */
