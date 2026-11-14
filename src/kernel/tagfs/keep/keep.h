#ifndef TAGFS_KEEP_H
#define TAGFS_KEEP_H

#include "../tagfs.h"

/*
 * Keep — a deleted file is kept for a term, and when the term passes the
 * volume lets it go by itself.
 *
 * A person deletes a file on purpose, and what he is told about it — the hub
 * warns — is not the volume's to say. What the volume owns is the file's state:
 * its record says it is deleted and until when (TAGFS_FILE_DELETED, and the
 * time beside it, FileRecord.keep_until), no query meets it, and until that
 * time it can be brought back (InkKeep with a term 0, tagfs_restore_file).
 * The TERM is the deleter's: the volume has no setting of its own, so the call
 * that deletes carries it, in seconds — the hub reads it from its Settings —
 * and a term of 0 is the file let go at once, as before there was a term.
 *
 * ‼ THE TERM ENDS BY THE VOLUME'S OWN SERVICE, AND AT THE MOUNT. A file whose
 * term has passed is let go by a Quitclaim, its blocks to Fallow like any
 * file's, by KeepSweepNow: the service (KeepServe, a look at the kept files
 * every KEEP_LOOK_US of the machine's time, run at the service point every
 * idle core and every core's loop goes through — TagFSServiceIfPending beside
 * the Helm's and the Settle's) and the first look after a mount, whose gate
 * KeepMountReset opens, which finds the terms that passed while the machine
 * was off. A file wearing a term that has passed is no longer one to bring
 * back, whether or not the sweep has reached it (tagfs_restore_file).
 *
 * The clock is the machine's wall clock, the one a file's times are of: a
 * clock set back keeps a file longer, and one set forward lets it go sooner —
 * the same trust every file's modified time puts in it.
 */

/* How often the service looks at the kept files, in the machine's microseconds. */
#define KEEP_LOOK_US  10000000ull

/* The wall clock in nanoseconds, as a file's times are; and the time a term of
 * `term_seconds` seconds from now ends at — held at 2^64 - 1, never wrapped. */
uint64_t KeepNowNs(void);
uint64_t KeepUntil(uint64_t term_seconds);

/* The kept files whose term has passed at `now_ns`, each let go: how many went.
 * One the volume refuses to give up now — no room for the Quitclaim yet — is
 * left for the next look. Inside the door; the call takes it itself. */
uint32_t KeepSweepNow(uint64_t now_ns);

/* The service: a sweep, at most one at a time and at most one a KEEP_LOOK_US. */
void KeepServe(void);

/* A mount's start: the next look is the first thing the volume does. */
void KeepMountReset(void);

#endif /* TAGFS_KEEP_H */
