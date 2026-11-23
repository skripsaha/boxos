#ifndef TAGFS_VOLUME_NAME_H
#define TAGFS_VOLUME_NAME_H

#include "../../../lib/kernel/ktypes.h"
#include "../../core/error/error.h"
#include "../../core/use_context/use_context.h"
#include "../tagfs.h"

/*
 * ‼ A VOLUME'S NAME IS THE PERSON'S WORD ABOUT IT, AND ITS BOOK KEEPS IT.
 *
 * Given or changed, the name is a demand on the Settle cycle, never a write of
 * its own: the next stitch carries it whole (RIBBON_VOLUME_NAME), the next
 * Tally keeps it (TALLY_USE, key (1, 0)), and every mount after reads it back
 * — on whichever machine the volume is met, so the name goes with the volume.
 * A stitch with no room for it is laid without it: the name asked is refused
 * (ERR_TAGFS_NO_SPACE) and the one the book holds stays — a name is not
 * forgotten for want of room, as no file is.
 *
 * Everything here is changed under the volume's state lock, and nothing is
 * read off or written to the medium, or asked of the heap, under it.
 */

/* The name asked of volume `vol` judged and made: `len` bytes at `text`,
 * spelled as a name (TallyNameSpelled, volume_tally.h) — 0 takes the name
 * away, *said NULL —, of a volume that takes a change. OK with the name held
 * in *said; ERR_INVALID_ARGUMENT, ERR_DEVICE_NOT_READY for a volume whose
 * medium left, ERR_TAGFS_READ_ONLY, ERR_NO_MEMORY — nothing held. */
error_t  VolumeNameAsk(TagFSVolume* vol, const char* text, size_t len, UseText** said);

/* Whether `vol` is called `said` already — NULL: nothing — and no other name
 * is owed: a name the book holds is not written again, nor a cycle asked. */
bool     VolumeNameHolds(TagFSVolume* vol, const UseText* said);

/* The name made by VolumeNameAsk owed to the next stitch of `vol`, the hold
 * handed over: the ticket its answer is asked by. A cycle whose TAKE comes
 * after it carries it (SettleWaitLinkCycle, TagFSSettle). Inside the door. */
uint64_t VolumeNameSay(TagFSVolume* vol, UseText* said);

/* Whether the name said under `ticket` was refused for want of room — the
 * book's kept —, asked once the cycle that carried it is over. */
bool     VolumeNameRefused(TagFSVolume* vol, uint64_t ticket);

/* How many bytes the name of `vol` is, and as many of them as fit into `out`
 * (`room`); 0 for a volume nobody named. */
size_t   VolumeNameOf(TagFSVolume* vol, char* out, size_t room);

#endif /* TAGFS_VOLUME_NAME_H */
