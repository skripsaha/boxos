#ifndef TAGFS_MOUNT_GROUND_GROW_H
#define TAGFS_MOUNT_GROUND_GROW_H

#include "../tagfs.h"
#include "../deed/deed.h"
#include "deed_pen.h"

/*
 * A volume taking the ground behind it — the medium's part (ground_grow.c).
 * The growth stands on the census and the derived block map (book_grow.c,
 * MountBookGrow); the questions asked of the medium and the order the two
 * copies of the Deed are written in are these.
 *
 * ‼ THE ORDER IS THE CRASH SAFETY, and there is nothing else protecting it:
 * the far copy at the new end, read back; then the head, which is the commit.
 * Cut the power before the head and the volume is exactly what it was; after
 * it, grown, with both copies agreeing (logcheck.sh growcut).
 */

/* Whether there is ground behind a volume of `have` sectors, on ground the
 * partition table runs `ground_sectors` long, that may be taken: a whole
 * block of it at least, and a medium that says how far it runs and runs that
 * far. Each reason it is not said. */
bool GroundGrowRoom(TagFSVolume* vol, uint8_t seat, uint64_t ground_sectors, uint64_t have);

/* The Deed in `head`, taken into `pen` and amended there by its caller —
 * the volume's growth (book_grow.c), the kernel its maker laid naming
 * (volume_lay.c) — written for a volume of `sectors` whose far copy sits at
 * `tail`: the far copy, flushed and read back, then the head, flushed. True
 * once the head is written. False, the Deed's bytes put back from `keep` —
 * the head's as it was read — and why said, `what` naming the change, when
 * either did not take: the volume is as it was. */
bool DeedsAmendedWritten(TagFSVolume* vol, uint8_t seat, DeedCopy *head, DeedPen *pen, const uint8_t *keep,
                         uint64_t sectors, uint64_t tail, const char *what);

/* The far copy the growth left behind, inside the data run now: erased by
 * `blank`, DEED_PEN_BYTES of the caller's, and said either way. The caller
 * takes its buffer before the growth commits, and grows not at all without it
 * (book_grow.c). */
void GroundGrowOldTailErased(TagFSVolume* vol, uint8_t seat, uint64_t old_tail, uint8_t *blank);

#endif /* TAGFS_MOUNT_GROUND_GROW_H */
