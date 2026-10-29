#ifndef TAGFS_SETTLE_ESCHEAT_H
#define TAGFS_SETTLE_ESCHEAT_H

#include "../tagfs.h"
#include "../mount/tally_read.h"

/*
 * Escheat — the act that lets go what a format-v2 book can never give back
 * (spec_v2 §16.5 п.20, §2.5, §4.5 п.3 (б)). A FILES range no copy of whose node
 * reads locks its files and holds every block they may own, and nothing
 * automatic ever lets them go: a block named by nothing that reads may be the
 * locked file's, and a reader may yet save it (volume-read). Escheat is the
 * one explicit act that ends that: asked by a cabin wearing system or god
 * alone — the warrant the guard of a file the system keeps asks (ink.h) — it
 * is owed to the next Tally, which names none of the rotten nodes, carries
 * none of the files locked with them, and is written whole; at its DONE the
 * files of the ranges are lost, said, their ids never given again (next_fid
 * stands past them) and answering as no file's from then on, and the blocks
 * held for them — and only for them: a file locked for another reason keeps
 * its ground held — rest in Fallow for the lag and go back. Nothing new is written: the Tally is the act's whole record
 * (§16.5 п.20), and a reader takes a Tally that leaves out a range the slot
 * before named rotten (tools/volumeread/binder.py, _LetGo). Refused, the Tally
 * gives everything back and the next one owes it again.
 *
 * The act lets one thing more go: a Slip's tail that can never be laid in its
 * block — its base unreadable — which holds every Tally back for that file
 * alone (helm/engross.c). Under the act the Helm's service cuts the file to
 * its last whole block with the act's warrant, said, and the Tally goes on —
 * each tail refused before the ask, none refused after it; the tail of the
 * file the loader reads the kernel by, which no call changes, waits, said. And it gives one thing
 * back: a DIGESTS leaf no copy of which reads is laid anew, each living
 * block's digest taken of its data (§16.5 п.19) — the one way it is.
 *
 *   EscheatMounted    the mount's census done: the held ground parted — what
 *                     a file locked for another reason names stays held —
 *                     and what the act would let go counted
 *   TagFSEscheat      asked, from any core with no TagFS lock: what the next
 *                     Tally lets go, or why not — ERR_PERMISSION_DENIED
 *                     without the warrant, ERR_OBJECT_NOT_FOUND with nothing
 *                     to let go, ERR_NO_MEMORY when the mount could not part
 *                     the held ground
 *   EscheatOwed       an ask no Tally answered yet
 *   EscheatTailsActing  a walk of the files begun: the asks it cuts tails
 *                     for, 0 when every ask had its walk; EscheatTailsReach:
 *                     the newest Ink an ask saw — a file whose record a later
 *                     Ink made is no tail of the act's; EscheatTailsWalked:
 *                     that walk done, having tried every file's tail, none
 *                     passed over nor refused for what may pass — the asks it
 *                     answered need no other (helm/engross.c)
 *   EscheatTake       TALLY_TAKE, before DogEar's: the ranges, the files and
 *                     the ground let go taken from the mount's lists, their
 *                     keys put into DogEar; EscheatTaken: the Tally under way
 *                     took it, and is written whole (binder_kept.c)
 *   EscheatDigestsLaid  the whole Tally's walk of the rotten DIGESTS leaves,
 *                     under the act: the leaf noted to be laid anew, true —
 *                     false without the act, or with no memory to note it
 *                     (the Tally names it as it lies)
 *   EscheatDigestOf   the walk of every live file's ground, a block the
 *                     Tally before cannot give the digest of: in a leaf
 *                     noted, its digest taken of its data and put in the
 *                     Gloss (*have); else nothing. OK, or memory's, the
 *                     medium's or the Gloss's refusal
 *   EscheatDone       the Tally's DONE: said, the ground let go to Fallow,
 *                     the ranges and files lost kept for EscheatLost
 *   EscheatLost       whether the act let `fid` go — from any core inside
 *                     the door: such an open answers no file, not a lock
 *   EscheatTakenBack  the Tally refused: everything back where it was
 *   EscheatLetGo      the mount let go of
 */

typedef struct {
    uint64_t ranges;    /* rotten ranges of FILES no Tally names after the act */
    uint64_t files;     /* files declared lost: the parents' counts, and those whose header lies before a range */
    uint64_t blocks;    /* blocks held for them, let go */
    uint64_t tails;          /* tails of Slips lost */
    uint64_t digest_ranges;  /* DIGESTS ranges — a leaf, or an inner node's whole range — known to read in
                              * no copy, laid anew of the data */
} EscheatSaid;

void    EscheatMounted(void);
error_t TagFSEscheat(InkWarrant warrant, EscheatSaid* out);
/* Whether the act is owed, the asks made — from any core; a tail the Helm's
 * service let go under it, counted (helm/engross.c, EngrossEscheat). */
bool     EscheatOwed(void);
uint64_t EscheatTailsActing(void);
uint64_t EscheatTailsReach(void);
void     EscheatTailsWalked(uint64_t asks);
void     EscheatTailLost(void);
void    EscheatTake(void);
bool    EscheatTaken(void);
bool    EscheatDigestsLaid(const TallyPlace* at);
error_t EscheatDigestOf(uint64_t block, uint64_t* digest, bool* have);
void    EscheatDone(uint64_t seq);
bool    EscheatLost(uint64_t fid);
void    EscheatTakenBack(void);
void    EscheatLetGo(void);

#endif /* TAGFS_SETTLE_ESCHEAT_H */
