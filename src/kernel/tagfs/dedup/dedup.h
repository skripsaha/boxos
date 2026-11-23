#ifndef TAGFS_DEDUP_DEDUP_H
#define TAGFS_DEDUP_DEDUP_H

#include "../tagfs.h"

/*
 * Dedup — Glean's own step after each Tally of the book: the blocks written
 * since the Tally before, held to every block the volume holds the same digest
 * for, and each copy laid on its original by Conjugate (conjugate.h), which
 * holds every pair byte for byte — equal digests are only a candidate.
 *
 * ‼ WHERE IT LOOKS (§16.7 п.7). The ground the live files took since the Tally
 * before and still hold — DogEar's, as the Tally took it (dog_ear.h), the
 * digests the Gloss's until that Tally's DONE lets them go — comes into
 * Commonplace (commonplace.c), memory's index of digest to block, never
 * written to the medium. Its blocks are held to one another there, and to the
 * blocks the book's DIGESTS names, walked a leaf at a time from where the last
 * walk stopped — a block of the volume, a cursor no Tally moves — as many
 * Sheets a pass as the Tally laid and the blocks it weighs take on the
 * medium: finding a copy reads no more of the book than the copy wrote. A
 * block stays in Commonplace until the walk has gone round the whole book
 * once since it came, until the pass lays it, or until its file lets it go —
 * so in steady writing it holds about one block in 512 of the volume
 * (DIGESTS is 8 bytes a block), whatever the rate.
 *
 * ‼ THE OLDER IS THE ORIGINAL. Of blocks whose digests agree, the one every
 * other is laid on is the oldest: the run born in the earliest stitch, then
 * the file made first, then the earlier block of it. Who names a block is
 * asked of the mirror itself, the live files walked a few a turn
 * (dedup_owners.c): no map of block to owner is kept.
 *
 * ‼ WHAT IT NEVER TOUCHES (§16.7 п.8). A file the system keeps in a role (system,
 * boot, kernel), the kernel the Deed points the loader at, a file the mount locked,
 * and every block one of them names, are neither
 * laid nor laid on; a copy is laid only from its plain ground (DATA, no Slip
 * over it). Each is passed over before an act is asked: the pass asks nothing
 * it knows will be refused. No tag of a file asks it out of Dedup: a file's
 * tags are its user's words, and the user never thinks of the kernel.
 *
 * ‼ A SERIES OR NOTHING. A copy is laid only as a series: DEDUP_SERIES_LEAST
 * blocks of it in a row in its file on as many of one original in a row in
 * the original's — a shorter one is left as it is, and so is a block alone,
 * so no file is cut into runs SHARED of a block or two to save as much (a
 * block of zeros has thousands of twins and no original worth a seek). And a
 * file is never laid on itself: what repeats inside one file is its own
 * shape, and a file laid on its own blocks only reads them out of order.
 * No act leaves a run of a series behind it shorter than a series, where the
 * series is long enough to share out between its acts.
 *
 * ‼ A SERVICE OF THE HELM, A STEP A TURN (helm.h), as Engross is (engross.h):
 * a turn of a service point takes the candidates in, or reads one leaf of
 * DIGESTS, or walks a few files, or asks one act, and gives its core back. An
 * act lays at most as many blocks as the seat takes in one command
 * (TagFSRunBlocks), so no file's Helm and no core is held long. A pass ends
 * with one line: what it weighed, matched, laid, passed over, and why an act
 * was refused.
 *
 *   DedupGleanAt  by the Settle's holder at Tally `tally`'s DONE, before the
 *                 Gloss lets go of the digests the Tally names, with no TagFS
 *                 lock held: the ground the Tally took in handed to the
 *                 service, `sheets` the Sheets the Tally laid
 *   DedupWeigh    by the Settle's holder at a cycle's DONE: a pass that waits
 *                 for the ribbon's room goes on
 *   DedupHold     no pass begun from a Tally's DONE while held, and what
 *                 Commonplace holds let go — the stand's runner holds it while
 *                 the tests run, as it holds the cadence: true when it was held
 *   DedupLetGo    a mount begun, the volume let go of: everything let go, the
 *                 door shut
 */

#define DEDUP_SERIES_LEAST  16u     /* blocks of a copy in a row the pass lays, and no fewer */

void DedupGleanAt(TagFSVolume* vol, uint64_t tally, uint64_t sheets);
void DedupWeigh(TagFSVolume* vol);
bool DedupHold(TagFSVolume* vol, bool held);
void DedupLetGo(TagFSVolume* vol);

/* What passes said, summed: the passes ended, the blocks they weighed, the
 * copies laid on an original that was itself written since the Tally before
 * (`among`) or that the walk of DIGESTS read (`book`), the blocks laid, the
 * blocks an act refused, the blocks passed over before any act, and the
 * copies of a series shorter than DEDUP_SERIES_LEAST left as they are. */
typedef struct {
    uint64_t passes;
    uint64_t weighed;
    uint64_t among;
    uint64_t book;
    uint64_t laid;
    uint64_t refused;
    uint64_t passed;
    uint64_t scant;
} DedupSaid;

#ifdef CONFIG_VOLUME_TESTS
/* The stand's: every pass asked so far run to its end — on this core's
 * service points, a cycle settled for a pass that waits for the ribbon's room
 * — bounded; what the passes said since the last ask in *out. False when they
 * did not end within the bound. */
bool DedupStandPassed(TagFSVolume* vol, DedupSaid* out);
#endif

#endif /* TAGFS_DEDUP_DEDUP_H */
