#ifndef TAGFS_BLOCK_MAP_H
#define TAGFS_BLOCK_MAP_H

#include "../../../lib/kernel/ktypes.h"
#include "../../core/error/error.h"
#include "../lock_rank/lock_rank.h"

typedef struct TagFSVolume TagFSVolume;   /* volume/tagfs_volume.h */

/*
 * The block map — which blocks of the volume are taken — and the Allotment
 * that keeps a growing file's ground in hand.
 *
 * The map is derived from the names that own the volume's ground, never kept
 * on the medium (block_map_derived.h). It is counted in WINDOWS
 * of BLOCK_MAP_WINDOW_BLOCKS blocks, each summed in memory, so a volume of any
 * size costs its summary and the windows that are neither wholly taken nor
 * wholly free.
 */

/* The blocks one window of the map covers. */
#define BLOCK_MAP_WINDOW_BLOCKS  (TAGFS_SECTOR_SIZE * 8u)

/* The most blocks one take hands out: sixteen windows. A take measures runs a
 * window at a time under the state lock, so the length of a take is what its
 * hold of that lock spends — sixteen windows keep the hold short. Nothing in
 * memory is the shorter for it: a caller after more takes again from where
 * the run ended, and a file's runs that meet join into one Strip (strip.h). */
#define BLOCK_MAP_TAKE_MOST      (16u * BLOCK_MAP_WINDOW_BLOCKS)

/*
 * Allotment — ground a growing file keeps in hand, in memory only.
 *
 * A file that grows takes a window of free blocks sized to it — its own size
 * up to the most one take hands out (BLOCK_MAP_TAKE_MOST), and never less than
 * the growth asked for — and uses the front of it; the rest is kept for its
 * next growth, so two files growing side by side do not deal each other
 * alternate blocks. The window outlives the handle, which lives one
 * operation. It is never named on the medium: its blocks are free ground in
 * the book, and a volume cut in the middle finds no block owned by nobody. It
 * is given back when the file is deleted, when the volume is short of room —
 * every other file's window is taken back before anybody is told there is
 * none — and when the volume is let go.
 *
 * AllotmentRelease: the file's window, by a caller holding the state lock.
 */
void    AllotmentRelease(TagFSVolume* vol, uint64_t fid);

/*
 * MapLock: the lock the windows — and the map — are read and changed under,
 * the volume's state lock.
 * AllotmentTakeBackLocked, under it: every window but file `keep_fid`'s taken
 * back for a take short of room, said — whether there was one.
 * AllotmentDropAll: every window let go of with the volume, the door shut —
 * nothing given back, nothing written.
 */
TagFSLock *MapLock(TagFSVolume* vol);
bool       AllotmentTakeBackLocked(TagFSVolume* vol, uint64_t keep_fid);
void       AllotmentDropAll(TagFSVolume* vol);

/*
 * A file's data: a window lies on the derived map as free
 * ground no other file's take may have, nor the ribbon's Quire while another
 * run is free.
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
 * stay free for the versions that fill them. When nothing can be had — past
 * `keep` and again past the book's fresh measure when that is less
 * (EndpaperDataKeeps): ERR_WOULD_BLOCK while Fallow rests ground, the room
 * asked of the cycle, or while a Tally gives ground back, the Tally asked
 * (binder.h, BinderRoomWait); else ERR_TAGFS_NO_SPACE, said.
 * AllotmentTakeBookRun: one run of exactly `want` blocks for file `fid`'s
 * gather (plot_gather.c), from `hint` on — never a piece of it, whatever
 * window the file keeps — past `keep` and the promises, answered as
 * AllotmentTakeBook answers when none can be had.
 * AllotmentOthersIn, under the state lock: whether [start, start + count)
 * meets the window of a file other than `fid` (0: any file's) — *past where
 * the first such ends. AllotmentBookTaken: the blocks a book's data was
 * handed since the volume came up.
 */
error_t  AllotmentTakeBook(TagFSVolume* vol, uint64_t fid, uint32_t want, uint64_t file_blocks, uint64_t hint, bool aside,
                           uint64_t keep, bool own, uint64_t *start, uint32_t *got);
error_t  AllotmentTakeBookRun(TagFSVolume* vol, uint64_t fid, uint32_t want, uint64_t hint, uint64_t keep, uint64_t *start);
bool     AllotmentOthersIn(TagFSVolume* vol, uint64_t fid, uint64_t start, uint64_t count, uint64_t *past);
uint64_t AllotmentBookTaken(TagFSVolume* vol);

/*
 * The block a Slip promises (allotment.c): every file whose
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
uint64_t AllotmentPromised(TagFSVolume* vol);
uint64_t AllotmentBookMay(TagFSVolume* vol, uint64_t free, uint64_t keep, bool own);
bool     AllotmentPromise(TagFSVolume* vol, uint64_t keep, uint32_t epoch);
void     AllotmentPromiseLetGo(TagFSVolume* vol, uint32_t epoch);
uint64_t AllotmentFreeUnpromised(TagFSVolume* vol);

#ifdef CONFIG_VOLUME_TESTS
/* The window file `fid` keeps in hand: false when it keeps none. */
bool    AllotmentHeld(TagFSVolume* vol, uint64_t fid, uint64_t *start, uint32_t *count);
#endif

typedef struct {
    uint64_t fid;
    uint64_t start;
    uint32_t count;
} Allotment;

/* What one volume holds of allotment.c (TagFSVolume, volume/tagfs_volume.h). */
typedef struct {
    /* Sorted by start, and no two overlap. */
    Allotment* allot;
    uint32_t allot_len;
    uint32_t allot_cap;
    /* Every block a book's data was handed since the volume came up: the reserve
     * the book keeps grows with them between two of its measures (endpaper.c). */
    volatile uint64_t book_taken;
    volatile uint8_t book_short_said;  /* the refusal said, until a take is had: any core, no lock */
    /*
     * ‼ A SLIP PROMISES ITS BLOCK. A write that rides a Slip takes no ground now,
     * and its tail is laid in a block before the next Tally (Engross): given where
     * no block is left for that, the Engross would find none, the Tally wait for it
     * for ever and the ribbon grow. So every file whose record holds Slips keeps
     * one block of the free ground promised to it — counted, never chosen
     * (FileMapSlips) — and the Plot that gives a file its first Slip promises the
     * block first (AllotmentPromise), only where a write of a block of data could
     * have it: past what such a write leaves and every promise standing. Every
     * take of the book's ground leaves the promises free — a write of data's
     * (AllotmentBookMay), the ribbon's Quire and a Tally's (BlockMapTakeQuire) —
     * but the take that lays the block a file's Slips lie over, which has that
     * block as its own while one is free, whatever the book's reserve: the
     * reserve's measure grows with no ground taken (a file born, a tag coined),
     * and an Engross refused for it would leave the Tally that gives ground back
     * waiting for ever — while the take, and the record without the Slips it is
     * inked into, leave the free ground less the promised as it was. A Plot's
     * promise is of its mounting, and a mounting begins with none: its files'
     * promises are their records' (file_map.h).
     */
    uint64_t promises;  /* the Plots' promises not let go, of mounting allotment.promises_epoch */
    volatile uint32_t promises_epoch;
} AllotmentState;

#endif /* TAGFS_BLOCK_MAP_H */
