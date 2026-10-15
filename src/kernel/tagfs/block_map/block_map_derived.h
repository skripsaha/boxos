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
 * Built by the mount with the door shut; read inside the door. Changed, once
 * it is up, under the volume's state lock — the lock format v1's map is changed
 * under (MapLock) — and by the ribbon's door alone in this kernel
 * (BlockMapTakeQuire): the page a change may need for a window it leaves mixed
 * is made before the lock (BlockMapDerivedStock), and under it only put in
 * place. Never while the volume stands under glass.
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
 * mixed gets no page. Once the map is up, a claim and a release are made
 * under the state lock, and their pages come from the stock made before it. */
error_t  BlockMapDerivedRelease(GroundOwner owner, uint64_t who, uint64_t start, uint64_t count,
                                uint64_t* free);

/* With no TagFS lock held: the pages one run's change may want — a window at
 * each of its ends — made and kept for a change of the live map under the
 * state lock. OK, or ERR_NO_MEMORY. */
error_t  BlockMapDerivedStock(void);

/*
 * A whole Quire for the ribbon (spec_v2 §3.6), its next_quire given ahead of
 * need: `blocks` free blocks in a row, the first on a multiple of `align` —
 * the medium's atom — inside the data run [first, the map's end), looked for
 * from `from` on and then from `first`; never taken where fewer than `keep`
 * blocks would be left free after it — the ground a whole Tally needs
 * (sewing.h, EndpaperKeeps). Claimed for the ribbon. With no TagFS lock held:
 * its pages are stocked first, and the state lock taken. OK and *start;
 * ERR_TAGFS_NO_SPACE when no such run is free or the reserve forbids it;
 * ERR_NO_MEMORY with no page for the map.
 */
error_t  BlockMapTakeQuire(uint64_t blocks, uint32_t align, uint64_t keep, uint64_t first, uint64_t from,
                           uint64_t* start);

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

/* The volume made small for the stand: the last free runs of the map claimed
 * until `leave` blocks are free — true when they are — and every run it took
 * let go of again. */
bool     BlockMapDerivedStandShrink(uint64_t leave);
void     BlockMapDerivedStandGrow(void);

/* Every window's summary held to its page — its free count, its longest run,
 * a window with no page whole free or whole taken — and the sum to the
 * volume's free count: true when all agree; *free and *mixed counted. */
bool     BlockMapDerivedStandHolds(uint64_t* free, uint32_t* mixed);
#endif

#endif /* TAGFS_BLOCK_MAP_DERIVED_H */
