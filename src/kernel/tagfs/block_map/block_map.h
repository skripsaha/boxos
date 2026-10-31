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
 * naming the block reaches the medium — no take's door writes its sector
 * (M4b). It may be paid earlier, never later: a give's door settles the map
 * (BlockMapSettle), and a fetch that finds every slot owing writes the least
 * lately used one whole. A name the cycle does not carry — a registry chain's
 * link — can land first; its block then reads free on the medium, and the next
 * mount's census takes it (a MISSING block). A give is never owed — a block
 * the medium still calls taken costs a block, never a file.
 */

/* Summary and cache for a data run of `data_blocks`, whose map begins at
 * volume sector `map_vlba`. ERR_NO_MEMORY, said with the numbers, when the
 * heap will not hold them; ERR_OUT_OF_RANGE, said, for a run of more windows
 * than the summary numbers. Every block it hands out is below `data_blocks` —
 * on a format v1 volume the layout's u32 count, so the structures of that
 * format that name a block in 32 bits (its chains, rows and Ledger) are never
 * handed one past them. */
error_t BlockMapMount(uint64_t data_blocks, uint64_t map_vlba);

/*
 * The mount's one pass over the medium's map. Every window is read once, the
 * summary is built from it, and — when `owns` is given — the window is held
 * against the owners' bits for it, which `owns` lays into `bits` (one bit per
 * block of window `window`, set where something owns the block): an ORPHAN is
 * taken on the medium and owned by nothing, a MISSING block is owned and reads
 * free. The first 32 runs of each are named, and every orphan run is handed
 * to `orphan` when it is given, lowest block first. The repair only ever adds:
 * a missing block is taken, in memory at once and on the medium by the next
 * Settle cycle, and an orphan stays taken. NULL `owns` builds the summary
 * alone. Nothing is written.
 */
typedef struct {
    uint64_t orphan;         /* blocks */
    uint64_t missing;        /* blocks */
    uint64_t free;           /* free on the map once the repair is in */
} BlockMapCensusCount;

/* The blocks one window of the map covers, and its owners' bits. */
#define BLOCK_MAP_WINDOW_BLOCKS  (TAGFS_SECTOR_SIZE * 8u)

/* The most blocks one take hands out: sixteen windows. A take's plan measures
 * runs a window past what it wants, and a best fit weighs a few windows of a
 * list with such a run beside each, so the length of a take is what its hold
 * of the state lock spends beyond a hold's budget (BlockMapStandHoldBound) —
 * sixteen windows keep that hold within twice the budget. Nothing in memory
 * is the shorter for it: a caller after more takes again from where the run
 * ended, and a file's runs that meet join into one Strip (strip.h). */
#define BLOCK_MAP_TAKE_MOST      (16u * BLOCK_MAP_WINDOW_BLOCKS)
typedef void (*BlockMapOwnsFn)(uint32_t window, uint8_t *bits, void *ctx);
typedef void (*BlockMapOrphanFn)(uint64_t start, uint32_t count, void *ctx);

error_t BlockMapCensus(BlockMapOwnsFn owns, BlockMapOrphanFn orphan, void *ctx,
                       BlockMapCensusCount *out);

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
 * would give back, or gives written down without their sectors wait for the
 * next cycle to lay them on, or ERR_TAGFS_NO_SPACE — said out loud, with the
 * numbers — when neither does; ERR_NO_MEMORY for no room to write the take
 * down. Its one taker is the pool's own Spare (PoolSpareBegin), and like
 * BlockMapTakeSpare it may have the blocks every other take leaves free for
 * the pool's reserve.
 *
 * Give: the run back. A run holding a block that is already free is refused
 * whole — ERR_TAGFS_EXTENT_ERROR, said with the block — and the map is left as
 * it is: freeing a free block is how one block comes to have two owners. Whole
 * as far as memory holds the run's sectors: a part whose sector is away is
 * written down and judged when the sector comes in, and a free block there
 * refuses that part alone, said then, after the give was answered OK
 * (BlockMapFetchRun brings in the two sectors at a door's run's ends).
 * BlockMapGiveHeldLocked is the same give asking the heap for nothing: the
 * room its deltas need comes from what BlockMapGiveRoomHold made before the
 * lock, and ERR_NO_MEMORY, unsaid, when that does not cover them — the caller
 * keeps the run and says what becomes of it.
 */
error_t BlockMapTakeLocked(uint32_t count, uint64_t hint, uint64_t *start);
error_t BlockMapGiveLocked(uint64_t start, uint32_t count);
error_t BlockMapGiveHeldLocked(uint64_t start, uint32_t count);

/* With no TagFS lock held: room made for `gives` deltas of gives and held for
 * them — a take never uses it — until the next call says how much to hold.
 * ERR_NO_MEMORY when the heap would not give it; the gives then draw on what
 * room there is. */
error_t BlockMapGiveRoomHold(uint32_t gives);

/* With the state lock held: whether gives wait as deltas for sectors not in
 * memory — ground that is free and still unseen, which the next cycle's S1
 * lays on. A take that met no other room is told it is on its way (MapShort),
 * so an asker for room runs that cycle rather than answering at once. */
bool    BlockMapGivesUnseenLocked(void);

/*
 * The same take, by a door with no TagFS lock held: the sectors it needs are
 * fetched first. Held under a TagFS lock it takes without reading instead.
 * `site` names the caller in a LockRank line about the state lock. When no
 * run of `count` is free, after every other file's Allotment was taken back
 * and every give written down was laid on: ERR_WOULD_BLOCK or
 * ERR_TAGFS_NO_SPACE, as BlockMapTakeLocked answers them.
 *
 * ‼ THE POOL'S RESERVE IS MADE WHOLE FIRST (meta_pool.h). While the pool holds
 * fewer Spares than its reserve, this take and every one below leave free the
 * blocks the reserve lacks: a take that would eat into them is shorter for
 * them, or short as above. One door is not held to it — BlockMapTakeSpare, a
 * block for one of the pool's own Spares — so ground a cycle gives back on a
 * full volume goes to the Spares a delete and Glean stand on before any write
 * waiting for room can have it.
 */
error_t BlockMapTake(uint32_t count, uint64_t hint, uint64_t *start, const char *site);
error_t BlockMapTakeSpare(uint64_t *start, const char *site);

/*
 * A write's ground: the same take for file `fid` (AllotmentTake takes for a
 * file as well). Its run is left free in every image of the map the medium
 * is sent until the S1 of the cycle whose snapshot holds the Ink that names
 * it (BlockMapInkedLocked) — or until it is given back, never to be named: a
 * cycle settled under its write leaves no ground that nothing owns.
 */
error_t BlockMapTakeFor(uint64_t fid, uint32_t count, uint64_t hint, uint64_t *start,
                        const char *site);

/* Under the state lock, by the Ink `gen` of file `fid`'s record: every run
 * taken for the file that no Ink named yet is named by this one. */
void    BlockMapInkedLocked(uint64_t fid, uint64_t gen);

/*
 * The sectors of the windows at a run's two ends fetched into memory, with no
 * TagFS lock held, so that a give made under the state lock right after meets
 * them there and is judged at once. A caller holding a TagFS lock fetches
 * nothing; its give is judged when the sectors next come in.
 */
void    BlockMapFetchRun(uint64_t start, uint32_t count, const char *site);

/*
 * Pay what the map owes the medium: every edit made without its sector is laid
 * onto it, and every sector carrying a take the medium has not heard is
 * written. A give's door calls it with no TagFS lock held; under one it does
 * nothing.
 */
void    BlockMapSettle(void);

/* The Settle cycle's S1, for the cycle whose snapshot holds every Ink up to
 * `target`: the runs those Inks named owed and written taken, settled, and
 * every sector memory changed written too. A write another core has on its
 * way with a debt in it is waited for, and its refusal is S1's. OK only once
 * every debt it came for is on the medium; otherwise the first refusal, each
 * one said — ERR_DEVICE_NOT_READY for a write whose core a halt stopped, and
 * ERR_WOULD_BLOCK for one on its way beneath this very caller. */
error_t BlockMapFlush(uint64_t target);

/*
 * A sector goes out to the medium leaving free every block memory holds taken
 * that nothing the medium holds names yet: a block of the pool no link names
 * (PoolUnnamedLeftOut, meta_pool.h), a block the pool's head no longer names
 * that rests in Fallow (FallowUnnamedLeftOut, fallow.h), and a write's run
 * no settled record names (BlockMapTakeFor). A sector that left one out is
 * let go of only once it was written whole, so a sector read back in is
 * always the medium's whole word. Under the state lock, when `block` starts or
 * stops being one nothing names — a Spare put in the stock, whose take a write
 * may have carried already; a block whose link the cycle at S0 is about to
 * write; a block the head is about to stop naming — its sector is owed to the
 * medium again, and the next settle of the map writes it as it now must read.
 */
void    BlockMapOweLocked(uint64_t block);

/*
 * TagFSTakeRun — the longest run of free blocks up to `want`, as near `hint`
 * as a run that long lies: *got is `want` wherever a run of `want` exists, and
 * otherwise the longest the volume has. Where the hint itself is free and the
 * run from it is long enough, the take starts exactly there — a file that
 * grows grows in place. No take is longer than BLOCK_MAP_TAKE_MOST: a caller
 * after more takes again from where the run ended, and its runs join in
 * memory. When nothing is free, after every Allotment was taken back:
 * ERR_WOULD_BLOCK while Fallow rests blocks, else ERR_TAGFS_NO_SPACE.
 */
error_t TagFSTakeRun(uint32_t want, uint64_t hint, uint64_t *start, uint32_t *got);

/*
 * Allotment — ground a growing file keeps in hand, in memory only.
 *
 * A file that grows takes a window of free blocks sized to it — its own size
 * up to the most one take hands out (BLOCK_MAP_TAKE_MOST), and never less than
 * the growth asked for — chosen best-fit, and uses the front of it; the rest is
 * kept for its next growth, so two files growing side by side do not deal each
 * other alternate blocks. The window outlives the handle, which lives one
 * operation. It is never named on the medium: its blocks read free there, and
 * a volume cut in the middle finds no orphan. It is given back when the file
 * is deleted, when the volume is
 * short of room — every other file's window is taken back before anybody is
 * told there is none — and when the volume is let go.
 *
 * AllotmentTake: up to `want` blocks for file `fid`, whose size is
 * `file_blocks`, near `hint` — never more than one take hands out at a time;
 * as TagFSTakeRun answers. What it hands out is the file's write's, as
 * BlockMapTakeFor's run is.
 * AllotmentRelease: the file's window, by a caller holding the state lock.
 */
error_t AllotmentTake(uint64_t fid, uint32_t want, uint64_t file_blocks, uint64_t hint,
                      uint64_t *start, uint32_t *got);
void    AllotmentRelease(uint64_t fid);

/*
 * A book's data (spec_v2 §3.1): its map is derived from the names that own its
 * ground (block_map_derived.h), and a window lies on it as free ground no other
 * file's take may have, nor the ribbon's Quire while another run is free.
 *
 * AllotmentTakeBook: up to `want` blocks for file `fid`, never leaving fewer
 * than `keep` free — the book's reserve (endpaper.c), which data never takes
 * (§3.6) — and the blocks promised to the tails Slips lie over (below), but
 * for one block of them a take `own` has: the take that lays the block its
 * file's Slips lie over, promised to it — in one run: the block `hint` names,
 * exact, when it is free
 * (Allotment 1: the version that fills a block continues the file's run);
 * else the front of the file's window; else a new window sized to the file,
 * the first run that long from `hint` on (Allotment 2: blocks written over in
 * one call land in one run). A take `aside` — a block that will not be full
 * — lies away from the blocks the file goes on into after `hint`, so they
 * stay free for the versions that fill them. As
 * TagFSTakeRun answers when nothing can be had — past `keep` and again past
 * the book's fresh measure when that is less (EndpaperDataKeeps): ERR_WOULD_BLOCK
 * while Fallow rests ground, the room asked of the cycle, or while a Tally
 * gives ground back, the Tally asked (binder.h, BinderRoomWait); else
 * ERR_TAGFS_NO_SPACE, said.
 * AllotmentTakeBookRun: one run of exactly `want` blocks for file `fid`'s
 * gather (plot_gather.c), from `hint` on — never a piece of it, whatever
 * window the file keeps — past `keep` and the promises, answered as
 * AllotmentTakeBook answers when none can be had.
 * AllotmentOthersIn, under the state lock: whether [start, start + count)
 * meets the window of a file other than `fid` (0: any file's) — *past where
 * the first such ends. AllotmentBookTaken: the blocks a book's data was
 * handed since the volume came up.
 */
error_t  AllotmentTakeBook(uint64_t fid, uint32_t want, uint64_t file_blocks, uint64_t hint, bool aside,
                           uint64_t keep, bool own, uint64_t *start, uint32_t *got);
error_t  AllotmentTakeBookRun(uint64_t fid, uint32_t want, uint64_t hint, uint64_t keep, uint64_t *start);
bool     AllotmentOthersIn(uint64_t fid, uint64_t start, uint64_t count, uint64_t *past);
uint64_t AllotmentBookTaken(void);

/*
 * The block a Slip promises (allotment.c, spec_v2 §16.6 п.1): every file whose
 * record holds Slips keeps one block of the free ground for the Engross that
 * lays its tail, and every take of a book's ground but the one that lays that
 * block leaves it free; that one has it whatever the book's reserve.
 *
 * AllotmentPromised: the blocks promised — a file's whose record holds Slips
 * (FileMapSlips), and a Plot's that gives a file its first Slip until its Ink
 * puts the record that holds them, or it is let go. With no lock, and never
 * fewer than are promised: a promise passing from a Plot to its record may be
 * counted twice, and one a tail's block was taken for counted until its record
 * holds the Slips no more.
 * AllotmentBookMay: the blocks a take of a book's data may have of `free`,
 * as AllotmentTakeBook judges `keep` and `own`.
 * AllotmentPromise: one block promised to a Plot of mounting `epoch` giving a
 * file its first Slip, the free ground having one past `keep` — what a write
 * of data leaves (EndpaperDataKeeps) — and every promise standing; false,
 * nothing promised, when it has none or the mounting is gone: the write lays
 * its block, and is refused as a write of blocks is. AllotmentPromiseLetGo:
 * that promise let go, by the Plot of mounting `epoch` — its record holds
 * Slips now, or the write laid none.
 * AllotmentFreeUnpromised: the free ground less what is promised — the free
 * a program is told.
 */
uint64_t AllotmentPromised(void);
uint64_t AllotmentBookMay(uint64_t free, uint64_t keep, bool own);
bool     AllotmentPromise(uint64_t keep, uint32_t epoch);
void     AllotmentPromiseLetGo(uint32_t epoch);
uint64_t AllotmentFreeUnpromised(void);

#ifdef CONFIG_VOLUME_TESTS
/* The window file `fid` keeps in hand: false when it keeps none. */
bool    AllotmentHeld(uint64_t fid, uint64_t *start, uint32_t *count);
#endif

#ifdef CONFIG_VOLUME_TESTS
/* Whether a block is taken on the map, and the block a take of one block with
 * no hint is handed next — for the startup tests that predict a block. */
bool    BlockMapIsTaken(uint64_t block);
bool    BlockMapNextFree(uint64_t *block);

/* Whether the medium's own sector of the map calls a block taken, read past
 * memory; false when the sector would not read. With no TagFS lock held. */
bool    BlockMapMediumSays(uint64_t block, bool *taken);

/* The sector of the window holding `block` out of memory — written whole
 * first when it owes the medium anything — so the next edit of that window is
 * written down without it. With no TagFS lock held. */
void    BlockMapSectorAway(uint64_t block);

/* The next room BlockMapGiveRoomHold must ask the heap for is asked of a heap
 * that refuses it: HEAPFAIL armed on the holder's core right before it. false
 * takes the plan down unspent. */
void    BlockMapGiveRoomRefused(bool refused);

/* The most steps one hold of the state lock gave a take's plan, and the most
 * deltas one edit of the map looked at — each since it was last asked, which
 * starts it again. */
uint32_t BlockMapHoldMost(void);
uint32_t BlockMapEditMost(void);

/* The next write of the sector of the window holding `block` runs the
 * Settle's S1 of the map (BlockMapFlush) on its way — with the lock let go,
 * right before the medium hears it — and keeps S1's answer, which
 * BlockMapStandFlushedOnItsWay hands back: false while no write ran it. */
void    BlockMapStandFlushOnItsWay(uint64_t block);
bool    BlockMapStandFlushedOnItsWay(error_t *said);

/*
 * A plan the way a take's door makes it — a best fit or the near rule, for
 * `want` blocks near block 0 — on a volume of `windows` windows that lives in
 * memory alone, with no sector of it read: SCATTERED is every window holding
 * a few short runs and the last 20 free whole; RUNS is runs of 15 whole free
 * windows between full ones. Weighed as a door that lets the lock go weighs
 * it: how many holds, the most steps one took and how long the longest took,
 * and the steps of all of them.
 * Made under the state lock, the volume's own map set aside and put back.
 * False when there was no memory for it.
 */
typedef enum { BLOCK_MAP_APART_SCATTERED = 0, BLOCK_MAP_APART_RUNS } BlockMapApartShape;

typedef struct {
    uint32_t holds;
    uint32_t hold_most;
    uint64_t hold_most_us;   /* the longest of them, in microseconds */
    uint32_t steps;
    bool     found;
    uint64_t at;
    uint32_t got;
} BlockMapApartPlan;

bool    BlockMapStandPlanApart(uint32_t windows, BlockMapApartShape shape, bool best_fit,
                               uint32_t want, BlockMapApartPlan *out);

/* The most steps one hold of a plan for `want` blocks may take: a hold's
 * budget, the window that spends it, and the run that window measures. */
uint32_t BlockMapStandHoldBound(uint32_t want);
#endif

#endif /* TAGFS_BLOCK_MAP_H */
