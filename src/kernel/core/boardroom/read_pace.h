#ifndef READ_PACE_H
#define READ_PACE_H

#include "ktypes.h"

/*
 * ReadPace — what the volume's medium is seen to take to read (spec_v2 §3.3,
 * §16.5 п.2).
 *
 * A book's cycle pays the flush of its data before its stitch only when the
 * mount's check of that data would cost more than the last cycle did
 * (settle/sewing.c, StitchToll), and what a check costs is the medium's own
 * measure: every read command of the volume's seat the medium answered, timed
 * from the Boardroom handing it to the seat's driver to the answer — what TagFS
 * found in its own cache never reaches here, and is no measure of the medium.
 * Over the period from one cycle's S0 to the next, the worst of them:
 *
 *   rate     the least speed of a read of one atom or more (§1.5: the larger
 *            of 4096 and the medium's physical block), bytes a second
 *   seek_us  the most a single read took past its bytes at that speed — of a
 *            read of an atom or more that is never above 0, so it is a read
 *            shorter than an atom that sets it; kept as the longest of them
 *            less the fewest bytes one carried at that speed, which is never
 *            below the letter's own measure
 *
 * A period with no read keeps the measure of the one before; a volume no read
 * was ever measured on has none, and every cycle of it with fresh data pays
 * the flush (rate 0). The worst and not the mean: a price taken too high only
 * adds a flush, one taken too low would lengthen the mount past what was
 * promised. The model of two points by the size of a read is owed once a
 * board's perf_trace has measured it (the living list) — not here.
 *
 * Lock-free: a read answered on any core folds into the period by a
 * compare-and-swap that only ever lowers the rate or raises the longest
 * read, and S0 takes the period by exchange, so no read is lost between two
 * periods — a race only ever errs to a flush more.
 */

typedef struct {
    uint64_t rate;       /* bytes a second; 0: no measure, the flush is paid */
    uint64_t seek_us;
} ReadPaceSeen;

/* The seat whose reads are measured — the volume's, as a book's mount begins:
 * the mount's own reads are the first measure, and whatever an earlier mount
 * measured is forgotten. */
void ReadPaceWatch(uint8_t seat);

/* A read of `sectors` sectors on `seat` the medium answered, `tsc` after it
 * was handed to the seat's driver. Any core. */
void ReadPaceRead(uint8_t seat, uint32_t sectors, uint64_t tsc);

/* By the Settle's holder at S0: the measure the cycle judges its stitch by —
 * the period's worst, or the last one's where the period read nothing — and
 * the period begun anew. A pace the stand named stands in its place
 * (block_fail.h, BlockFailReadPace). */
void ReadPaceTake(ReadPaceSeen* out);

#endif /* READ_PACE_H */
