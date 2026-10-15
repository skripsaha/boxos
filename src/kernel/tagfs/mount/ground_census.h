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

/* The same claim, made by a slot of a format-v2 volume's Spine — a node of the
 * Tally it names (`who` the node's seq), a Quire its ribbon goes on in (0):
 * one name two slots give alike is one owner (GroundCensusBook). */
void    GroundClaimBySlot(GroundOwner owner, uint64_t start, uint32_t count, uint64_t who, uint32_t slot);

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
 * GROUND_FALLOW claims are the ground the ribbon let go (M-1); fallow is what
 * of them no other owner but the held names, *fallow their blocks. The
 * GROUND_HELD claims are what may be held for what rotted; held is what of
 * them no other owner names — the fallow among them, BOOT, which lies over a
 * file by design, aside — and *held their blocks.
 *
 * A claim is a name — its owner, its `who`, its run, and the slot that gives
 * it. One name two slots give alike is one owner: an unchanged node two
 * Tallies name (§2.9), a Quire two ribbons go on in — as many times over as
 * the slot that gives it most. Any other two that meet are two owners: two
 * nodes of one Tally on one block, two runs of one file, of two files, a
 * node and a file (volume-read's census keeps the same rule, bookcensus.py
 * Named).
 *
 * Then, `derive`, every claim is laid into the volume's block map, derived
 * from them (block_map_derived.h): what none names is free. Not `derive`, no
 * map is made — the claims are not every owner's, and a map of them would
 * read free what an owner the census does not see holds. OK; ERR_NO_MEMORY
 * with nothing held, or the map's refusal — *what says which.
 */
error_t GroundCensusBook(uint64_t* held, uint64_t* fallow, bool derive, const char** what);

/* Whether a block of [start, start + count) is claimed already by `owner`. */
bool    GroundClaimedBy(GroundOwner owner, uint64_t start, uint64_t count);

/* The claims of `owner` as they stand — after GroundCensusBook, the fallow and
 * the held cut to what it leaves them — into `out` as DATA Strips, `room` of
 * them at the most: how many there are. */
uint64_t GroundClaimsOf(GroundOwner owner, Strip* out, uint64_t room);

/* The claims let go. */
void    GroundCensusEnd(void);

#ifdef CONFIG_VOLUME_TESTS
/* The owner of the first claim made on `block` since Begin, or
 * GROUND_OWNER_KINDS when none names it: the stand asks what one owner
 * claims, between a Begin and an End of its own, with no mount running. */
GroundOwner GroundClaimOf(uint64_t block);
#endif

#endif /* TAGFS_GROUND_CENSUS_H */
