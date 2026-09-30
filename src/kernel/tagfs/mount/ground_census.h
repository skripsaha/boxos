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
void    GroundClaim(GroundOwner owner, uint32_t start, uint32_t count, uint32_t who);

/* The claims against the medium's map, a window at a time: the orphans and the
 * missing named, the missing taken again in memory for the next cycle to
 * write, the free count derived; two owners of a block said. ERR_TAGFS_CORRUPTED
 * when the map would not read; ERR_NO_MEMORY when the repair could not be
 * written down. When a claim was refused memory, the map is only measured —
 * said — and nothing is taken again. */
error_t GroundCensusRun(BlockMapCensusCount* out);

/* The claims let go. */
void    GroundCensusEnd(void);

#endif /* TAGFS_GROUND_CENSUS_H */
