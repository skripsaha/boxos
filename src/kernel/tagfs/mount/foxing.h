#ifndef TAGFS_MOUNT_FOXING_H
#define TAGFS_MOUNT_FOXING_H

#include "../tagfs.h"

/*
 * Foxing — the copies of the book's Tally nodes a mount found spoiled, kept
 * for that mount alone (spec_v2 §16.6 п.10). A copy whose unit does not hold
 * (book_unit.h: a sector the medium will not read, a sum that fails, a seq
 * or a length that is not its pointer's) holds in no read of the same
 * sectors as a unit of the same Sheets and seq: the answer is the bytes'
 * own, and the medium's, which stands until the sectors are written over —
 * and nothing writes them while a mount reads its book, its door shut. So a
 * second read of it in the same mount is asked of memory, not of the medium:
 * the walk of the Tally stood on, Facsimile's look in each older slot that
 * names the same node where it lies (facsimile.c), the ground of the older
 * slots (book_ground.c) — every one of them a drive's recovery of every
 * sector, seconds a sector on a real disk, where the first answer is the one
 * it would give again. Kept by the copy, not the node: of a node one copy of
 * which is spoiled, the other copy is read as before, and holds or not on
 * its own. A copy that reads whole but does not parse in the range its
 * parent's separators give it is not kept — another parent may give it
 * another range (tally_walk.c TallyNodeParse).
 *
 * The mount's alone: kept and asked only between FoxingBegin and FoxingEnd,
 * which the reading of the book makes with its door shut (book_mount.c
 * MountReadBook). A reader inside the door — TallyDigestFind, the Binder's
 * proof of a copy (TallyCopyHolds) — never meets a mount reading its book,
 * and finds nothing kept: every read of it asks the medium.
 *
 * volume-read reads an image, whose answer to a read does not change: it has
 * nothing to keep, and the slot it takes a range from is the one this mount
 * takes — Foxing changes what is asked of the medium, never what is found.
 *
 *   FoxingBegin  the mount's reading of its book begins: nothing kept
 *   FoxingSays   whether the copy at `block`, `sheets` long, of seq `seq`
 *                was found spoiled in this reading: true with the words its
 *                read said, in `why`
 *   FoxingNoted  a copy found spoiled kept, with the words its read said;
 *                memory refusing the room, it is not kept, and its next read
 *                asks the medium, as every read did before
 *   FoxingEnd    the reading ended, whatever its end: everything kept let go
 */

void FoxingBegin(void);
bool FoxingSays(uint64_t block, uint64_t sheets, uint64_t seq, char* why, size_t room);
void FoxingNoted(uint64_t block, uint64_t sheets, uint64_t seq, const char* why);
void FoxingEnd(void);

#endif /* TAGFS_MOUNT_FOXING_H */
