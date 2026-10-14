#ifndef TAGFS_GROUND_CENSUS_H
#define TAGFS_GROUND_CENSUS_H

#include "../tagfs.h"
#include "ground_owner.h"
#include "../block_map/block_map.h"

/*
 * GroundCensus — every owner of the data run, against the medium's map.
 *
 * The mount asks each owner in ground_owner.h for its ground and each answers
 * from what the mount already holds in memory — the chains it walked, the
 * files' records, the Ledger's fields, the Deed's BOOT run — so the census
 * reads nothing but the map itself. The claims are kept as runs, not as a map
 * of the data run: the memory is the owners', whatever the size of the
 * volume, and the medium's map passes a window at a time beside the owners'
 * bits for that window (BlockMapCensus).
 *
 * What it decides is owner decision 3's first half: a block an owner names and
 * the map calls free is taken again (missing — the repair only adds); a block
 * the map calls taken and no owner names is an orphan, REPORTED and kept
 * taken, until the census is shown to agree with tools/volume-read.py's on
 * the same image. Two exclusive owners of one block are said aloud — a lie of
 * the medium deeper than the Fallow lag covers, or a defect of a writer.
 *
 * The mount's alone: one core, the door shut, from Begin to End.
 */

void    GroundCensusBegin(void);

/* `count` blocks from `start` are `owner`'s — `who` names which one of its
 * kind (a file id; 0 where the kind has one). Runs past the data run are cut
 * to it. Memory refused is remembered: the census then decides nothing. */
void    GroundClaim(GroundOwner owner, uint64_t start, uint32_t count, uint64_t who);

/* The claims against the medium's map, a window at a time: the orphans and the
 * missing named — every orphan run handed to `orphan` when it is given — the
 * missing taken again in memory for the next cycle to write, the free count
 * derived; two owners of a block said. ERR_TAGFS_CORRUPTED when the map would
 * not read; ERR_NO_MEMORY when the repair could not be written down. When a
 * claim was refused memory, the map is only measured — said — and nothing is
 * taken again, and no orphan is named. */
error_t GroundCensusRun(BlockMapOrphanFn orphan, BlockMapCensusCount* out);

/*
 * A format-v2 volume's census, which keeps no bitmap to hold the claims
 * against (spec_v2 §4.6): every block an owner names is taken, and the only
 * finding is a block two owners name — said where they meet, as above. The
 * GROUND_HELD claims are what may be held for what rotted; held is what of
 * them no other owner names — BOOT, which lies over a file by design, aside
 * — and *held their blocks. OK, or ERR_NO_MEMORY with nothing held.
 */
error_t GroundCensusBook(uint64_t* held);

/* Whether a block of [start, start + count) is claimed already by `owner`. */
bool    GroundClaimedBy(GroundOwner owner, uint64_t start, uint64_t count);

/* The claims let go. */
void    GroundCensusEnd(void);

#ifdef CONFIG_VOLUME_TESTS
/* The owner of the first claim made on `block` since Begin, or
 * GROUND_OWNER_KINDS when none names it: the stand asks what one owner
 * claims, between a Begin and an End of its own, with no mount running. */
GroundOwner GroundClaimOf(uint64_t block);
#endif

#endif /* TAGFS_GROUND_CENSUS_H */
