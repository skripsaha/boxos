#ifndef TAGFS_MOUNT_H
#define TAGFS_MOUNT_H

#include "../tagfs.h"

/*
 * The mount — what the volume's records say, read into memory, standing on
 * the copy of the Ledger whose seal the medium bears out.
 *
 * tagfs_init stands on the ground, reads the Deed and both copies of the
 * Ledger, and brings up what an earlier kernel left (the DiskBook, copy-on-
 * write); this reads the rest, in the order each part needs the one before:
 *
 *   the seals      the newest intact copy's seal, and the other copy's; a
 *                  newest copy with none was written last by a kernel before
 *                  the seal, and the volume is read as that kernel left it —
 *                  said once, in the line of address below;
 *   the registry   up to what the seal says it holds (a torn last block rolls
 *                  back to the blocks before it);
 *   the pool       walked once, up to the seal it stands on (meta_pool.h);
 *   the table      under a seal, derived from the pool's span — a file is the
 *                  newest record of its id, a row with no record in the span
 *                  is a cycle that did not settle and is let go, counted in one
 *                  line; with no seal, the table says which files there are;
 *   the fid        the next file id is past every id the medium has shown —
 *                  the Ledger's, every row's, every record's, past the seal
 *                  too — so no id it ever saw is given out again;
 *   the tags       J1: no tag id a record wears is given out again;
 *   the census     every owner of the data run against the medium's map
 *                  (ground_census.h) — the Use Context's chains among them,
 *                  read with the Ledger (use_memory.h), and the orphans an
 *                  older kernel's Ledger stopped naming looked through for
 *                  one.
 *
 * With the door shut, on one core.
 */

/* OK: every part above is up and the Ledger stands on the copy the medium
 * bears out. Otherwise the refusal, and in *what the words tagfs_init's
 * refusal says (mount_refused), which also takes down what came up. */
error_t MountReadVolume(const char** what);

/* The next file id a mount derives: past the highest id of every row and of
 * every record the medium showed, and never short of the Ledger's own count;
 * a Ledger counting 0 — one an earlier kernel let wrap — stays as it is, for
 * LedgerIssueFileId to refuse; past 0xFFFFFFFE it is every id issued. */
uint32_t MountNextFileId(uint32_t ledger_next, uint32_t top_row, uint32_t top_record);

/* After the door has opened and the integrity map is up (RT-22): on a mount
 * whose seal was not written by a machine stopping cleanly, the digests of
 * the blocks the last cycle's records name are set back to unknown — a block
 * such a file wrote in place after that cycle may have landed without its
 * digest, or its digest without it, and neither is rot. */
void    MountAfterDoor(void);

/* Whether the volume's last stop is known to have been clean: the seal the
 * mount stood on says a machine stopping wrote it. False for any other seal,
 * and for a volume an older kernel left with none — a block whose digest does
 * not match may then be a write in place that did not land before the stop,
 * which format v1 cannot tell from rot. *out_cycle: the seal's cycle, 0 with
 * none. */
bool    MountStopWasClean(uint64_t* out_cycle);

#endif /* TAGFS_MOUNT_H */
