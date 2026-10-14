#ifndef TAGFS_BLOCK_MAP_DERIVED_H
#define TAGFS_BLOCK_MAP_DERIVED_H

#include "block_map.h"
#include "ground_owner.h"

/*
 * The block map of a format-v2 volume — derived from its owners' names, never
 * read off the medium, never written to it (spec_v2 §4.6).
 *
 * A book keeps no bitmap: which blocks are taken is what the owners name
 * (ground_owner.h), every claim the mount finds inserted here by the census
 * (ground_census.c GroundCensusBook). In memory alone: a summary of every
 * window of BLOCK_MAP_WINDOW_BLOCKS blocks, sixteen bytes — how many of its
 * blocks are free, the longest run of them, and its page — and the page, a
 * bit a block (512 bytes), only for a window both free and taken. A window
 * whole free or whole taken costs its summary alone: 64 KiB for a 64 GiB
 * volume, 16 MiB for 16 TiB, and 512 bytes for every mixed window. A heap
 * that will not hold them refuses the mount out loud, with the number of
 * mixed windows it would take, and never leaves a map half made.
 *
 * It covers the volume's blocks [0, end), `end` the far Deed's: the front and
 * the Spine's slots are claimed by their owners like any other ground, so
 * what reads free is the data run's — the free count of the volume
 * (TagFSState.ledger.free_blocks), which this map keeps as format v1's keeps
 * it. Format v1's map is another (block_map.h: the medium's bitmap and its
 * cache): a mount builds the one its volume's format has, and the other is
 * never up.
 *
 * Built by the mount with the door shut; read inside the door; changed by
 * nothing while the volume stands under glass.
 */

/* The map begun for [0, end): every window free, no page made. ERR_NO_MEMORY
 * when the heap will not hold the summary, ERR_OUT_OF_RANGE for more windows
 * than it counts — *what the refusal, with the numbers. */
error_t  BlockMapDerivedBegin(uint64_t end, const char** what);

/*
 * `count` blocks from `start` claimed by `owner` (`who` which one of its kind:
 * a file's id; 0 where the kind has one), cut to the map's end. A block of
 * them taken already is a block two owners name: the first 32 claims that
 * meet one are named, each by its run and how many of it were taken, the rest
 * counted — the first of them says so — and the count said when the map is
 * let go; *taken how many. OK; ERR_NO_MEMORY,
 * nothing changed, when a window the claim leaves mixed gets no page — but
 * while the map is built, a page refused turns the build into a count of the
 * windows that would be mixed, for BlockMapDerivedBuilt to say, and the claim
 * is answered OK.
 */
error_t  BlockMapDerivedClaim(GroundOwner owner, uint64_t who, uint64_t start, uint64_t count,
                              uint64_t* taken);

/* The run let go by `owner`: a block of it free already is named as above,
 * *free how many. OK; ERR_NO_MEMORY, nothing changed, when a window it leaves
 * mixed gets no page. */
error_t  BlockMapDerivedRelease(GroundOwner owner, uint64_t who, uint64_t start, uint64_t count,
                                uint64_t* free);

/* The build ended: OK, the map up and its free count the volume's; or
 * ERR_NO_MEMORY, the map let go and *what the refusal, which names how many
 * mixed windows the heap would not hold. */
error_t  BlockMapDerivedBuilt(const char** what);

bool     BlockMapDerivedUp(void);

/* Whether `block` is free: false past the map's end. */
bool     BlockMapDerivedIsFree(uint64_t block);

/* The first run of at least `want` free blocks inside window `window`: true and
 * *start, or false when the window holds none. */
bool     BlockMapDerivedRunIn(uint32_t window, uint32_t want, uint64_t* start);

/* Everything the map holds let go. */
void     BlockMapDerivedLetGo(void);

#ifdef CONFIG_VOLUME_TESTS
/* The next build's first page is asked of a heap that refuses it: HEAPFAIL
 * armed on the builder's core right before it. false takes it down unspent. */
void     BlockMapDerivedStandRefuse(bool refused);

/* How many mixed windows the last refused build named; 0 when none was. */
uint32_t BlockMapDerivedStandShort(void);

/* Every window's summary held to its page — its free count, its longest run,
 * a window with no page whole free or whole taken — and the sum to the
 * volume's free count: true when all agree; *free and *mixed counted. */
bool     BlockMapDerivedStandHolds(uint64_t* free, uint32_t* mixed);
#endif

#endif /* TAGFS_BLOCK_MAP_DERIVED_H */
