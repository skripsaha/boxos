#ifndef TAGFS_BLOCK_MAP_H
#define TAGFS_BLOCK_MAP_H

#include "../../../lib/kernel/ktypes.h"
#include "../../core/error/error.h"

/*
 * The block map — which blocks of the data run are taken, and the allocator
 * that hands them out.
 *
 * The medium keeps one bit per block, a sector of the map for every 4096
 * blocks: a WINDOW. In memory the map is no longer held whole. What stays
 * resident is a summary of every window (how many blocks a take may have
 * there, a run of them and where it starts, the runs at its two edges), and
 * a small cache of the windows' own sectors. A volume of any size costs the
 * summary and the cache, never the map.
 *
 * ‼ NOTHING IS READ OR WRITTEN UNDER A LOCK. Every decision and every edit of
 * the map is made under the volume's state lock with the sectors already in
 * memory; a door that needs a sector fetches it with no lock held and decides
 * afterwards, and a caller that holds a TagFS lock is served without reading at
 * all — from a sector in memory, or from what the summary knows is free, the
 * edit written down and laid onto the sector when it next comes in.
 *
 * MapOwed: a take leaves the medium owed the sector its bit lives in, and the
 * Settle cycle pays it at S1 (BlockMapFlush), before any record, row or Ledger
 * naming the block reaches the medium — no door writes a sector for a take
 * (M4b). A name the cycle does not carry — a registry chain's link — can land
 * first; its block then reads free on the medium, and the next mount's census
 * takes it (a MISSING block). A give is never owed — a block the medium still
 * calls taken costs a block, never a file.
 */

/* Summary and cache for a data run of `data_blocks`, whose map begins at
 * volume sector `map_vlba`. ERR_NO_MEMORY, said with the numbers, when the
 * heap will not hold them. */
error_t BlockMapMount(uint32_t data_blocks, uint64_t map_vlba);

/*
 * The mount's one pass over the medium's map. Every window is read once, the
 * summary is built from it, and — when `owns` is given — the window is held
 * against the owners' bits for it, which `owns` lays into `bits` (one bit per
 * block of window `window`, set where something owns the block): an ORPHAN is
 * taken on the medium and owned by nothing, a MISSING block is owned and reads
 * free. The first 32 runs of each are named. The repair only ever adds: a
 * missing block is taken, in memory at once and on the medium by the next
 * Settle cycle, and an orphan stays taken. NULL `owns` builds the summary
 * alone. Nothing is written.
 */
typedef struct {
    uint32_t orphan;         /* blocks */
    uint32_t missing;        /* blocks */
    uint64_t free;           /* free on the map once the repair is in */
} BlockMapCensusCount;

/* The blocks one window of the map covers, and its owners' bits. */
#define BLOCK_MAP_WINDOW_BLOCKS  (TAGFS_SECTOR_SIZE * 8u)
typedef void (*BlockMapOwnsFn)(uint32_t window, uint8_t *bits, void *ctx);

error_t BlockMapCensus(BlockMapOwnsFn owns, void *ctx, BlockMapCensusCount *out);

/* Everything the mount holds let go, with not one byte written: what memory
 * knows of the map is from before whatever took the volume down. */
void    BlockMapUnmount(void);

/*
 * Taking and giving back, by a caller that holds the volume's state lock.
 * Nothing here reads or writes the medium.
 *
 * Take: `count` blocks in one run, the run nearest `hint` among those that
 * are long enough. OK and *start; when no run of `count` can be had without
 * reading the map, ERR_WOULD_BLOCK while Fallow rests blocks a Settle cycle
 * would give back, or ERR_TAGFS_NO_SPACE — said out loud, with the numbers —
 * when none rest; ERR_NO_MEMORY for no room to write the take down.
 *
 * Give: the run back. A run holding a block that is already free is refused
 * whole — ERR_TAGFS_EXTENT_ERROR, said with the block — and the map is left as
 * it is: freeing a free block is how one block comes to have two owners.
 */
error_t BlockMapTakeLocked(uint32_t count, uint32_t hint, uint32_t *start);
error_t BlockMapGiveLocked(uint32_t start, uint32_t count);

/*
 * The same take, by a door with no TagFS lock held: the sectors it needs are
 * fetched first. Held under a TagFS lock it takes without reading instead.
 * `site` names the caller in a LockRank line about the state lock. When no
 * run of `count` is free, after every other file's Allotment was taken back:
 * ERR_WOULD_BLOCK while Fallow rests blocks, else ERR_TAGFS_NO_SPACE.
 */
error_t BlockMapTake(uint32_t count, uint32_t hint, uint32_t *start, const char *site);

/*
 * The sectors of the windows at a run's two ends fetched into memory, with no
 * TagFS lock held, so that a give made under the state lock right after meets
 * them there and is judged at once. A caller holding a TagFS lock fetches
 * nothing; its give is judged when the sectors next come in.
 */
void    BlockMapFetchRun(uint32_t start, uint32_t count, const char *site);

/*
 * Pay what the map owes the medium: every edit made without its sector is laid
 * onto it, and every sector carrying a take the medium has not heard is
 * written. A give's door calls it with no TagFS lock held; under one it does
 * nothing.
 */
void    BlockMapSettle(void);

/* The Settle cycle's S1: settled, and every sector memory changed written too. OK,
 * or the first refusal — each one said. */
error_t BlockMapFlush(void);

/*
 * TagFSTakeRun — the longest run of free blocks up to `want`, as near `hint`
 * as a run that long lies: *got is `want` wherever a run of `want` exists, and
 * otherwise the longest the volume has. Where the hint itself is free and the
 * run from it is long enough, the take starts exactly there — a file that
 * grows grows in place. When nothing is free, after every Allotment was taken
 * back: ERR_WOULD_BLOCK while Fallow rests blocks, else ERR_TAGFS_NO_SPACE.
 */
error_t TagFSTakeRun(uint32_t want, uint32_t hint, uint32_t *start, uint32_t *got);

/*
 * Allotment — ground a growing file keeps in hand, in memory only.
 *
 * A file that grows takes a window of free blocks sized to it — its own size
 * up to one extent's width, and never less than the growth asked for — chosen
 * best-fit, and uses the front of it; the rest is kept for its next growth, so
 * two files growing side by side do not deal each other alternate blocks. The
 * window outlives the handle, which lives one operation. It is never named on
 * the medium: its blocks read free there, and a volume cut in the middle finds
 * no orphan. It is given back when the file is deleted, when the volume is
 * short of room — every other file's window is taken back before anybody is
 * told there is none — and when the volume is let go.
 *
 * AllotmentTake: up to `want` blocks for file `fid`, whose size is
 * `file_blocks`, near `hint`; as TagFSTakeRun answers.
 * AllotmentRelease: the file's window, by a caller holding the state lock.
 */
error_t AllotmentTake(uint32_t fid, uint32_t want, uint64_t file_blocks, uint32_t hint,
                      uint32_t *start, uint32_t *got);
void    AllotmentRelease(uint32_t fid);

#ifdef CONFIG_VOLUME_TESTS
/* The window file `fid` keeps in hand: false when it keeps none. */
bool    AllotmentHeld(uint32_t fid, uint32_t *start, uint32_t *count);
#endif

#ifdef CONFIG_VOLUME_TESTS
/* Whether a block is taken on the map, and the block a take of one block with
 * no hint is handed next — for the startup tests that predict a block. */
bool    BlockMapIsTaken(uint32_t block);
bool    BlockMapNextFree(uint32_t *block);
#endif

#endif /* TAGFS_BLOCK_MAP_H */
