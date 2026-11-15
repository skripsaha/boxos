#ifndef TAGFS_MOUNT_PERAMBULATION_H
#define TAGFS_MOUNT_PERAMBULATION_H

#include "away.h"
#include "../plot/plot.h"

/*
 * Perambulation — the judge of a book whose medium came back (spec_v2 §3.8,
 * §16.5 пп.10-12, C14, C20): the bounds of what this kernel left walked
 * again, on the medium as it now is, before the open book is taken up.
 *
 * ‼ THE MARK IS SEARCHED FOR, NOT READ FROM ONE PLACE (M-3). A book changes
 * nothing at a fixed place in a cycle — the Spine changes only with a Tally —
 * so another machine that sewed stitches on the medium meanwhile leaves every
 * fixed place as it was; taking that book up, this kernel would sew its own
 * stitches over the other's with the same seqs (H-2, the branches spliced).
 * So the medium is held to BookLeftMark (settle/sewing.h): the last stitch
 * this kernel left still where it was sewn, by its seq and its Clasp — every
 * one past the slot the ribbon is replayed over, on a medium shown to lie
 * (Р-c2 (б)); no sector of the book with a seq past it anywhere another
 * writer's next stitch could begin — the rest of the Quire past it and the
 * next_quire it names — but this kernel's own attempt cut short, byte for
 * byte as sealed (Р-c1 (б)), or what its mount found there already; and its
 * newest slot the one this kernel knows — in a slot it holds unsure, byte for
 * byte as sealed. A seq is no proof where this kernel gave it and the medium
 * never showed it: another machine that never saw it may give it again (R11).
 * Anything else is AWAY_OTHER, the book given up and the medium mounted
 * again as it now is.
 *
 * ‼ WHAT A FLUSH SAID IS NOT WHAT THE MEDIUM HOLDS (§16.5 п.10, X3-1/Z11-7).
 * A medium that went with its power may have lost what it said it kept, and
 * one with no SYNC CACHE says it keeps everything (xhci_msd.c). So every
 * block of a file's data no stitch settled — the Docket's, every record a
 * cycle refused gave back to it: all written since the last kept flush, and
 * those a cycle's flush covered that its stitch never settled — is read back
 * off the medium and held to the digest it was written with (AwayProof): all
 * holding, the book is taken up whether a flush covered them or not — what
 * was read is what lies there; one not holding is AWAY_NOT_LANDED,
 * ERR_TAGFS_NOT_LANDED to every waiter and the book given up. The bytes of a
 * Slip are memory's alone and go with the next stitch: nothing of them is
 * read.
 *
 * A write that laid blocks before the medium left and inks them after it came
 * back is in no Docket at the return: its own Ink holds them to the digests it
 * wrote them with (AwayProofPlot), and refuses ERR_TAGFS_NOT_LANDED a write
 * whose blocks do not hold.
 *
 * Taken up, a book whose own attempt lies past its last stitch goes on past
 * it, as a retry of a refused stitch goes on (M-6, §16.4 п.4 (б′): RibbonTryPast)
 * — never over it; where no reader would take that stitch up, the book is
 * given up instead. The verdict the mount held (errata.h) stays held until the
 * session's first stitch settles; a book given up is mounted again, and the
 * mount builds its verdict again from what the medium holds.
 *
 * Reads the medium in `seat`, its ground from sector `base`, past the latch,
 * by the Boardroom's own doors — never through the volume's door, which
 * refuses while the medium is away, nor through the book's readers, which a
 * read of a block on any core reaches (tally_read.c, a DIGESTS leaf). By the
 * one attending the medium, with no TagFS lock held, the Settle held still
 * (StitchHoldStill) from the first read to the verdict.
 */

/*
 * What the last judgement found, for its line (away.c) and the stand: why the
 * book is not as left; the last stitch held where it was sewn and the newest
 * slot; the blocks read back and those that did not hold; whether its own
 * attempt lay past the last stitch; and whether the medium lied.
 *
 * ‼ A MEDIUM LIED WHEN IT LOST WHAT A FLUSH IT KEPT COVERED, AND NOTHING THERE
 * IS ANOTHER WRITER'S. Once a stitch of this mount settled, its last flush the
 * medium said it kept covers every stitch the marks hold and every slot this
 * kernel holds sure — what the mount read and all laid since. One of them gone
 * from where it lies is a medium that answered a flush it never kept (a cheap
 * stick, X3-1): a stitch whose place holds a sector that is no sector of the
 * book — zeros, an unreadable one aside — or one of an older seq, every other
 * sector there still of its own unit; or a newest slot older than the newest
 * this kernel knew, and one it knows in that very slot. A sector of the
 * stitch's seq or past it of any other unit, or a slot this kernel does not
 * know, may be another machine's — a writer that mounted the medium
 * meanwhile, or an older copy of it written back — and proves nothing of the
 * medium: the book is given up all the same, nothing remembered (AwayGiveUp,
 * AwayLiedLearned).
 */
typedef struct {
    const char* why;
    uint64_t    last;
    uint64_t    slot;
    uint64_t    proved;
    uint64_t    missed;
    bool        tried;
    bool        lied;
} PerambulationSaid;

/* The verdict on the volume found on `seat` from `base` — `same` whether its
 * Deed lays it out as the one held (AwayDeedSame): a FRAME of another layout
 * is another volume's book, AWAY_OTHER with nothing read. */
AwayVerdict              Perambulation(uint8_t seat, uint64_t base, bool same);
const PerambulationSaid* PerambulationLast(void);

/* away_proof.c: the Docket's blocks of data no stitch settled, each one the
 * Gloss names, read back off `seat` from `base` and held to its digest — the
 * blocks read in *proved, those not holding in *missed. OK, ERR_NO_MEMORY, or
 * the medium's refusal (ERR_READ_FAILED). With no TagFS lock held. */
error_t AwayProof(uint8_t seat, uint64_t base, uint64_t* proved, uint64_t* missed);

/* A book's write about to be inked, with no TagFS lock held: when the medium
 * left since the write of its first block was sent (AwayDepartures, taken
 * before it: a departure latched after the medium answered is one since),
 * every block it laid read back through the volume's door and held to the
 * digest it was written with. OK, and *departed the departures it holds at —
 * the Ink laid only if none came since; ERR_TAGFS_NOT_LANDED, said, for
 * blocks that do not hold; or the door's refusal. */
error_t AwayProofPlot(const Plot* p, uint64_t* departed);

#endif /* TAGFS_MOUNT_PERAMBULATION_H */
