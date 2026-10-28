#ifndef TAGFS_MOUNT_ERRATA_H
#define TAGFS_MOUNT_ERRATA_H

#include "ktypes.h"
#include "error.h"

/*
 * Errata — the mount's verdict, held in memory until the session's first
 * stitch lays it down (spec_v2 §3.9, §2.8 Undone, §16.5 п.4-7).
 *
 * A mount that rolls a file back — its data in the newest stitch never landed
 * (NOT_LANDED, why 1), or a record of it did not fit the file (the rule of
 * rot, why 2) — writes nothing: the roll-back is a verdict, and a verdict the
 * next mount must find even once the stitch it undoes is no longer the newest
 * (H-1). So the mount keeps it here — a list in order of (the seq of the
 * stitch undone, why, the files it rolls back, rising) — and the first stitch
 * of the session carries an Undone of each, right after its StitchHead,
 * before any other record, with its seq_back reckoned from that attempt's own
 * seq (sewing.c). Each file the verdict rolls back and that lives wears
 * settle:undone, put on by an ordinary Ink as that cycle begins, so the stitch
 * that lays the verdict down lays the tag down with it and a program finds
 * every file whose last write did not land by a query by the tag (M-10). A
 * file born in a stitch undone is not: there is no file, its id is burned
 * (§16.5 п.7). A file the rule of rot rolls back onto ground another file
 * holds is locked, as ever — the next mount locks it again by the same rule —
 * and leaves the verdict.
 *
 * Built by the mount, behind the shut door, and let go of whole by the next
 * (MountBookLetGo): a mount of the same medium again finds the same stitch
 * newest and comes to the same verdict, so no Undone is ever laid twice. From
 * the door's opening it is the Settle's holder's alone: read as its stitches
 * are built, worn as its first cycle begins, let go once the stitch that
 * carried it settles; a cycle refused keeps it for the next.
 */

/* By the mount: file `fid` rolled back from stitch `seq` for `why` — one
 * entry a stitch, its why the first found — ERR_NO_MEMORY. A file that leaves
 * the verdict (locked instead). And the replay done: the list put in order by
 * stitch, each file once a stitch, the
 * files the tag is owed listed and the verdict said — ERR_NO_MEMORY, which
 * refuses the mount as any replay short of memory does. */
error_t  ErrataAdd(uint64_t seq, uint64_t why, uint64_t fid);
void     ErrataLeaves(uint64_t fid);
error_t  ErrataClosed(void);

/* Whether a verdict is held: the session's first cycle owes it. */
bool     ErrataHeld(void);

/* The first cycle of the session asked for at once — the verdict laid down
 * without waiting for a Lull (§3.5): at the door's opening, and again by a
 * medium that came back as it was left with the verdict not yet laid. */
void     ErrataAsk(void);

/* By the Settle's holder as its cycle begins, no lock held: settle:undone put
 * on each living file the verdict rolls back, by an ordinary Ink, so the
 * stitch that lays the verdict down lays the tag with it. OK once every one
 * wears it, or is no file. A refusal of the moment — the heap (ERR_NO_MEMORY,
 * the cycle waits for pages), the ribbon's room (ERR_TAGFS_NO_SPACE, tried a
 * window on), a volume that takes no change now — refuses the cycle, and the
 * next puts the rest on: never left out. Only a file that can never take the
 * tag goes without, said by its id. */
error_t  ErrataWorn(void);

/* The Undones the verdict lays: how many, their bytes at the most with their
 * heads, and the longest body. */
uint32_t ErrataUndones(void);
uint64_t ErrataBytes(void);
uint64_t ErrataBodyMost(void);

/* The Undones laid into the stream `out` from `at` for a stitch of seq `seq`,
 * each one's offset into starts[(*n)++] — `body` room for the longest: where
 * the stream goes on. */
uint32_t ErrataLay(uint8_t* out, uint32_t at, uint64_t seq, uint8_t* body, uint32_t* starts, uint32_t* n);

/* The stitch of seq `seq` that carried the verdict settled: the verdict is
 * the book's now, and memory lets it go, said. */
void     ErrataSettled(uint64_t seq);

/* The verdict let go of with the mount that built it (MountBookLetGo). */
void     ErrataLetGo(void);

#endif /* TAGFS_MOUNT_ERRATA_H */
