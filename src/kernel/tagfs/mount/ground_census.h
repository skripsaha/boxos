#ifndef TAGFS_GROUND_CENSUS_H
#define TAGFS_GROUND_CENSUS_H

#include "../tagfs.h"
#include "ground_owner.h"
#include "../plot/strip.h"

/*
 * GroundCensus — every owner of the volume's ground.
 *
 * The mount asks each owner in ground_owner.h for its ground and each answers
 * from what the mount already holds in memory — the files' records, the
 * Spine's slots and their Tallies, the ribbon's Quires, the Deed's BOOT run —
 * so the census reads nothing. The claims are kept as runs, not as a map of
 * the volume: the memory is the owners', whatever the size of the volume.
 * Two exclusive owners of one block are said aloud — a lie of the medium
 * deeper than the Fallow lag covers, or a defect of a writer.
 *
 * The mount's alone: one core, the door shut, from Begin to End.
 */

void    GroundCensusBegin(TagFSVolume* vol);

/* `count` blocks from `start` are `owner`'s — `who` names which one of its
 * kind (a file id; 0 where the kind has one). Runs past the volume are cut to
 * it. Memory refused is remembered: the census then decides nothing. */
void    GroundClaim(TagFSVolume* vol, GroundOwner owner, uint64_t start, uint32_t count, uint64_t who);

/* The same claim, made by a slot of the Spine — a node of the
 * Tally it names (`who` the node's seq), a Quire its ribbon goes on in (0):
 * one name two slots give alike is one owner (GroundCensusBook). */
void    GroundClaimBySlot(TagFSVolume* vol, GroundOwner owner, uint64_t start, uint32_t count, uint64_t who, uint32_t slot);

/*
 * The census, which keeps no bitmap to hold the claims against: every block an
 * owner names is taken, and the only finding is a block two owners name — said
 * where they meet, as above; a run SHARED (GROUND_SHARED) is no second owner
 * beside its block's DATA run or another run SHARED, and is one on any other
 * owner's ground (§8.2). Before the cut below, how many name each block a run
 * SHARED names is counted into Commonhold (ink/commonhold.h, ground_shares.c).
 * The GROUND_FALLOW claims are the ground the ribbon let go (M-1); fallow is
 * what of them no other owner but the held names, *fallow their blocks. The
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
error_t GroundCensusBook(TagFSVolume* vol, uint64_t* held, uint64_t* fallow, bool derive, const char** what);

/* Every block of a Quire of `quire` blocks a Tally's node lies in that no
 * claim names claimed the Tallies' (Р-d3: a Tally-Quire held whole while one
 * node in it lives) — after every claim, before GroundCensusBook. OK, or
 * ERR_NO_MEMORY said. */
error_t GroundQuiresHeld(TagFSVolume* vol, uint64_t quire);

/* Whether a block of [start, start + count) is claimed already by `owner`. */
bool    GroundClaimedBy(TagFSVolume* vol, GroundOwner owner, uint64_t start, uint64_t count);

/* The claims of `owner` as they stand — after GroundCensusBook, the fallow and
 * the held cut to what it leaves them — into `out` as DATA Strips, `room` of
 * them at the most: how many there are. */
uint64_t GroundClaimsOf(TagFSVolume* vol, GroundOwner owner, Strip* out, uint64_t room);

/* A run an owner names that its caller cuts to the volume before it claims
 * what is left — a locked file's run outside it, a held or fallow run
 * past it: counted in the reach as it is named, and claimed by no one. */
void    GroundCensusNamed(TagFSVolume* vol, uint64_t start, uint64_t count);

/* Past the farthest block any owner but the far Deed named since Begin — as
 * it named it, a claim before it is cut to the volume and a run told by
 * GroundCensusNamed alike — 0 when none did: the proof that the ground
 * behind the volume is no one's before it is taken (mount/book_grow.c). */
uint64_t GroundCensusReach(TagFSVolume* vol);

/* The blocks the census said two owners name (GroundCensusBook), since
 * Begin. */
uint64_t GroundCensusDoubled(TagFSVolume* vol);

/* The claims let go. */
void    GroundCensusEnd(TagFSVolume* vol);

#ifdef CONFIG_VOLUME_TESTS
/* The owner of the first claim made on `block` since Begin, or
 * GROUND_OWNER_KINDS when none names it: the stand asks what one owner
 * claims, between a Begin and an End of its own, with no mount running. */
GroundOwner GroundClaimOf(TagFSVolume* vol, uint64_t block);
#endif

#endif /* TAGFS_GROUND_CENSUS_H */
