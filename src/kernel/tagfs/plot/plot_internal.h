#ifndef TAGFS_PLOT_INTERNAL_H
#define TAGFS_PLOT_INTERNAL_H

#include "plot.h"

/*
 * Plot's own parts, shared by plot.c (the Plot and its runs), plot_write.c
 * (writing blocks and growing with zeros) and plot_gather.c (moving extents
 * together). Nothing outside plot/ calls these.
 */

/* [start, start + count) of ground after runs[*n - 1]: merged into it when it
 * begins where that one ends and the two together are no longer than a Strip
 * counts, else a Strip of its own. The array has room for one more. */
void     PlotRunsPut(Strip *runs, uint32_t *n, uint64_t start, uint32_t count);

/* A run this call took, recorded as a Stake: OK or ERR_NO_MEMORY. */
error_t  PlotStake(Plot *p, uint64_t start, uint32_t count);

/* Ground a record named, come to rest in Fallow once a record that no longer
 * names it is inked (TagFSInkPlot): OK or ERR_NO_MEMORY. */
error_t  PlotLetGoLater(Plot *p, uint64_t start, uint32_t count);

/* Room for `need` elements of `size` bytes in *array, which holds `have`
 * and has room for *room: false for no memory, the array as it was. */
bool     PlotRoom(void **array, uint32_t *room, uint32_t have, uint64_t need, size_t size);

/* The Stake holding `block`, or stake_count when no Stake does. */
uint32_t PlotStakeHolding(const Plot *p, uint64_t block);

/* [start, start + count) no longer the file's: what of it this call staked
 * goes back now, what a record named comes to rest once the Ink is laid. */
error_t  PlotLetGo(Plot *p, uint64_t start, uint32_t count);

/* Ground this call took given back to the allocator it came from — format
 * v2's derived map for a Plot of a book (`rec`), format v1's map else: 0, or
 * the allocator's refusal negated. */
int      PlotGiveBack(const Plot *p, uint64_t start, uint32_t count);

/*
 * PlotWindow (plot_window.c), the Plot of a book — `rec` held:
 *
 *   PlotWindowLoad      the Plot loaded from `rec`, whose hold it takes: no run
 *                       copied, the window empty at the file's end
 *   PlotWindowLetGo     the record, the plan and the digests let go
 *   PlotWindowCover     the window grown to whole Strips over [lo, hi)
 *   PlotWindowBegin     PlotBeginWrite's ground: fresh ground for the blocks
 *                       written over and for the growth, and the ribbon's room
 *                       for the change's record asked
 *   PlotWindowStretchAt PlotStretchAt on fresh ground, the rest of a block
 *                       read from the old block it lay on
 *   PlotWindowEnd       the write's fresh ground named where its bytes landed
 *                       (PlotRestake), the rest given back
 *   PlotWindowTrim      the window grown over the tail a cut takes off
 *   PlotWindowGrow      PlotExtendZero: a hole, the last block laid again
 *   PlotWindowRoomFor   PlotRoomFor: the one block a growth may lay
 *   PlotWindowRoomAsked the ribbon's room for the change's record at its most
 *                       held, before a byte of its data moves (P21): OK, or
 *                       ERR_WOULD_BLOCK while room can come, ERR_TAGFS_NO_SPACE
 *                       when none can — nothing more held
 *
 * Cover, Begin, Trim, Grow and RoomFor — the doors a change of the file
 * comes in by — refuse ERR_FILE_NOT_FOUND once the Plot holds no record: its
 * file given up between two of its windows (InkBookPlotNext).
 */
void     PlotWindowLoad(Plot *p, FileRecord *rec);
void     PlotWindowLetGo(Plot *p);
error_t  PlotWindowCover(Plot *p, uint64_t lo, uint64_t hi);
error_t  PlotWindowBegin(Plot *p, uint64_t at, uint64_t bytes);
bool     PlotWindowStretchAt(const Plot *p, uint64_t pos, uint64_t left, uint32_t most, PlotStretch *out);
error_t  PlotWindowEnd(Plot *p, uint64_t at, uint64_t done);
error_t  PlotWindowTrim(Plot *p, uint64_t keep_blocks);
error_t  PlotWindowGrow(Plot *p, uint64_t size);
error_t  PlotWindowRoomFor(const Plot *p, uint64_t end);
error_t  PlotWindowRoomAsked(Plot *p);

/* A hole of `count` blocks after the file's last run (plot_window.c), and the
 * file's last block k — the bytes of it that are the file's `tail` (1..4096)
 * — laid again on fresh ground: its base read (zeros for a hole), the Slips
 * over it laid on that, zeros past the tail. OK, or the refusal of the ground,
 * the read, the write or the ribbon's room. */
error_t  PlotAppendHole(Plot *p, uint64_t count);
error_t  PlotWindowTail(Plot *p, uint64_t k, uint32_t tail);

/*
 * The Slip of a write (plot_slip.c, spec_v2 §3.7, §16.5 п.13-15):
 *   PlotSlipBegin   the choice, at PlotWindowBegin: a write that lies wholly in
 *                   the file's last block as it leaves it, leaves that block
 *                   short of full, keeps the Slips one span, of a file that
 *                   wears no system, boot or god and is not the loader's
 *                   kernel, while no Tally is owed, with a block promised
 *                   to its tail (AllotmentPromise) — rides a Slip: no ground
 *                   taken, a hole put in for a new last block, *chosen; any
 *                   other lays blocks, and the heap that refuses the Slip its
 *                   room asks a Tally (§16.5 п.15)
 *   PlotSlipWrite   the write's bytes into its Slip: no block moves
 *   PlotSlipMost    the bytes the Slip may take in its record at the most
 * and PlotSlipLetGo (plot.h), with the Plot or its Ink.
 */
error_t  PlotSlipBegin(Plot *p, uint64_t at, uint64_t bytes, bool *chosen);
error_t  PlotSlipWrite(Plot *p, uint64_t at, const PlotSource *src, uint64_t *out_done);
uint64_t PlotSlipMost(const Plot *p);

/* Whether the record the Plot would ink fits format v1. */
bool     PlotRecordFits(const Plot *p);

/* A buffer of up to *most blocks from the heap, halved while the heap will not
 * give it — *most says how many it holds: NULL only when not one block fits. */
uint8_t *PlotRunBuffer(uint32_t *most);

#endif /* TAGFS_PLOT_INTERNAL_H */
