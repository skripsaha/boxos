#ifndef TAGFS_RETIRE_H
#define TAGFS_RETIRE_H

#include "../tagfs.h"

/*
 * Retire — a program left, and its own tags left with it.
 *
 * A program's own tags (tagfs.h, TagOwner) are its labels for itself, and a
 * program is the file it was started from. Once that file is given up for
 * good — deleted with no term, its term in the trash ended (keep/keep.h),
 * declared lost by Escheat, or found gone at a mount — every tag the program
 * owned comes off every file that wears it, the live ones and those kept
 * deleted alike, and is then RETIRED: its id is never given again, its text
 * and its owner are forgotten, and a Coined RETIRED of it lies in the next
 * stitch (tag_registry.h). While the file is only in the trash its tags live
 * and restoring it brings it back whole. And it is taken out of every list
 * of the programs that showed it their tags (FileRecord.shown_to); the list
 * its own card held went with the card.
 *
 * ‼ THE DUTY IS ONE RULE, DERIVED, NEVER A LIST OF WHAT HAPPENED. A program's
 * tag whose program's file is not in the volume — no record, and no file
 * locked by name — is owed its retirement. Whoever gives a file up says only
 * that something went (RetireOwed); the pass looks at every program's tag and
 * finishes what is owed, so a retirement a power cut broke off is finished by
 * the first look after the mount, which is the same pass, and a tag a
 * process of a program already gone puts on afterwards is caught by the next.
 *
 * ‼ NEVER IN THE CALLER'S CONTEXT. The pass runs at the service point every
 * idle core and every core's loop goes through (TagFSServiceIfPending, beside
 * the Keep's), one at a time. A tag is retired in four steps (tag_registry.h):
 * closed — no file may put it on —, taken off the files by one Ink a file
 * (InkTagsOff), laid retired by an Ink of its own with none wearing it
 * (InkTagsRetire) — whichever step meets a volume or a ribbon that cannot take
 * it now is tried again once a cycle settled, and what was done stays done.
 */

/* How often the service looks without being told, in the machine's
 * microseconds: a tag put on by a process of a program already gone is never
 * told of. */
#define RETIRE_LOOK_US  10000000ull

/* Whether file `fid` is gone for good: no record in the volume — a file kept
 * deleted has one — and none locked by name, which stands only unreadable; a
 * file Escheat lost is neither (BookFileLocked). Inside the door. */
bool RetireFileGone(uint64_t fid);

/* Something was given up: the next service point looks. From any context. */
void RetireOwed(void);

/* Every program's tag whose program is gone retired, as far as the volume
 * takes it now: *retired how many tags were laid retired. OK, or the first
 * refusal that stopped the look — the ribbon's room (ERR_WOULD_BLOCK), the
 * volume's, ERR_NO_MEMORY — or ERR_BUSY when a file put a tag on meanwhile.
 * Inside the door; the call takes it itself. */
error_t RetireSweepNow(uint32_t* retired);

/* The service: a sweep when something is owed — and at most one at a time —
 * once a cycle settled after a refusal, and every RETIRE_LOOK_US besides. */
void RetireServe(void);

/* The stand's hold on the service: no look is begun while held, and one under
 * way is waited out. Returns whether it was held. */
bool RetireHold(bool held);

/* A mount's start: the next look is the first thing the volume does. */
void RetireMountReset(void);

#endif /* TAGFS_RETIRE_H */
