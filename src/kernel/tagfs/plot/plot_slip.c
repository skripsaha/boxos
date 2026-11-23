#include "plot_internal.h"
#include "scribe.h"
#include "../file_map/tail_slip.h"
#include "../ink/ink.h"
#include "../ink/docket.h"
#include "../mount/book_mount.h"
#include "../settle/binder.h"
#include "../settle/sewing.h"
#include "../block_map/block_map.h"
#include "tagfs_volume.h"

/*
 * A write that rides a Slip — the Plot (plot_window.c) choosing, at its Begin,
 * to lay no block.
 *
 * ‼ A SMALL WRITE'S BYTES GO WHERE THE CYCLE GOES ANYWAY. A logger's line of
 * 40 bytes laid as a block costs the medium a block of fresh ground and its
 * flush beside the stitch that names it: two pages a cycle where one would
 * do (§12.1). A write that lies wholly in the file's last block as it leaves
 * it, and leaves that block short of full, rides the stitch instead — its
 * bytes in this Plot's own TailSlip, folded by its Ink into the tail the
 * record keeps (ink_book.c), laid by the next stitch as the change since the
 * last one (TailSlipDelta). Nothing is written now; the anchor's one atom
 * carries it.
 *
 * And where it may not: a write that would leave the Slips two spans (the
 * change a stitch lays is one span of bytes memory holds, never a gap over a
 * DATA block it would have to read); a file the loader reads by its blocks —
 * one kept in a role by its record, or the kernel the Deed points it at (§9.1)
 * — whose tail must be in its block; and while a Tally is owed, which takes
 * no slice while a Slip lies (§16.5 п.14): the writer lays its block, the
 * Slips under it folded in, and so lays its own tail in a block (Engross by
 * the holder). Each of these lays blocks, as a write always did. The heap
 * that refuses the Slip its room is said, lays the block, and asks a Tally —
 * the event, no number of Slips counted beforehand (§16.5 п.15).
 *
 * ‼ AND NEVER A SLIP THE VOLUME COULD NOT LAY. Its tail goes into a block
 * before the next Tally, so a file's first Slip is given only with a block of
 * the free ground promised to it (block_map.h, AllotmentPromise) — where a
 * write of a block could have had one. Where none can be promised, the write
 * lays its block, and is refused ERR_TAGFS_NO_SPACE as such a write is.
 */

#define PLOT_BLOCK TAGFS_BLOCK_SIZE

/* Whether the file lays no Slip whatever the write: a Plot with no record,
 * a window past a write's first, a file the loader reads by its blocks, or a
 * Tally owed.
 *
 * ‼ THE WRITE IS THE CALLER'S, NOT THE MEDIUM'S COMMAND. The Helm cuts a write
 * into windows of the run the medium takes in one command (helm_work.c
 * HelmWindowBytes): eight blocks on a port, one on a legacy channel. Judged a
 * window at a time, a write of a block and 100 bytes would lay both blocks on
 * the one and ride its 100 bytes on a Slip on the other — the tail of every
 * write past a block left for an Engross before each Tally on the medium
 * whose command is smallest. A window past the first is a part of a write
 * that began in a block before it, which does not lie in the last alone. */
static bool PlotSlipBarred(TagFSVolume* vol, const Plot *p)
{
    return !p->rec || p->part || p->fid == vol->book.boot_fid || InkFileKept(p->meta.flags) ||
           BinderEngrossOwed(vol);
}

/* The block the tail of a file whose record holds no Slips yet will be laid in,
 * promised — afresh before it is refused, as a take of data's measure is
 * (plot_window.c, PlotFreshTake): whether it is. */
static bool PlotSlipPromise(TagFSVolume* vol, Plot *p)
{
    if (p->rec->slip || p->promised)
        return true;
    p->promised = AllotmentPromise(vol, EndpaperDataKeeps(vol, false), p->mount_epoch) ||
                  AllotmentPromise(vol, EndpaperDataKeeps(vol, true), p->mount_epoch);
    return p->promised;
}

error_t PlotSlipBegin(TagFSVolume* vol, Plot *p, uint64_t at, uint64_t bytes, bool *chosen)
{
    uint64_t size = at + bytes > p->size ? at + bytes : p->size;
    uint64_t k    = at / PLOT_BLOCK;
    uint32_t off  = (uint32_t)(at % PLOT_BLOCK);
    *chosen = false;
    if (bytes == 0 || size % PLOT_BLOCK == 0 || k != (size - 1u) / PLOT_BLOCK || PlotSlipBarred(vol, p))
        return OK;
    if (k >= p->blocks_before && p->blocks_before && TailSlipOn(p->rec, p->blocks_before - 1u))
        return OK;      /* the full block the Slips lie over goes into a block first (PlotWindowBegin) */
    const TailSlip *under = TailSlipOn(p->rec, k) ? p->rec->slip : NULL;
    if (!TailSlipJoins(under, off, (uint32_t)bytes) || !PlotSlipPromise(vol, p))
        return OK;
    TailSlip *slip = TailSlipBlank();
    if (!slip) {
        PlotSlipLetGo(vol, p);
        kprintf("[TagFS] file %llu: the heap has no room for the Slip of its %llu byte(s) at %llu — they are laid "
                "as a block, and a Tally is asked: it lays every tail in a block and lets their memory go\n",
                p->fid, (unsigned long long)bytes, (unsigned long long)at);
        BinderDemanded(vol);
        return OK;
    }
    error_t rc = OK;
    if (k >= p->blocks_before) {
        /* The write begins the file's next block: a hole, the Slip over it. */
        rc = PlotWindowCover(p, p->blocks_before, p->blocks_before);
        if (rc == OK)
            rc = PlotAppendHole(p, k + 1u - p->blocks);
    }
    if (rc != OK) {
        TailSlipLetGo(slip);
        return rc;
    }
    p->slip     = slip;
    p->slip_off = off;
    p->slip_len = (uint32_t)bytes;
    *chosen     = true;
    return PlotWindowRoomAsked(vol, p);
}

error_t PlotSlipWrite(Plot *p, uint64_t at, const PlotSource *src, uint64_t *out_done)
{
    *out_done = 0;
    if (at % PLOT_BLOCK != p->slip_off || src->bytes != p->slip_len) {
        kprintf("[TagFS] DEFECT: file %llu: a write of %llu byte(s) at %llu is not the one its Slip was chosen for "
                "(%u at %u of the block) — none of it is the file's\n", p->fid, (unsigned long long)src->bytes,
                (unsigned long long)at, p->slip_len, p->slip_off);
        return ERR_TAGFS_EXTENT_ERROR;
    }
    error_t rc = PlotSourcePull(src, 0, TailSlipBytes(p->slip) + p->slip_off, p->slip_len);
    if (rc != OK) {
        kprintf("[TagFS] file %llu: the bytes to write from %llu on could not be taken from the pages they are "
                "in (error %u) — none of the write is the file's\n", p->fid, (unsigned long long)at, (unsigned)rc);
        return rc;
    }
    TailSlipCovers(p->slip, p->slip_off, p->slip_len);
    *out_done = p->slip_len;
    return OK;
}

void PlotSlipLetGo(TagFSVolume* vol, Plot *p)
{
    TailSlipLetGo(p->slip);
    if (p->promised)
        AllotmentPromiseLetGo(vol, p->mount_epoch);
    p->slip     = NULL;
    p->slip_off = 0;
    p->slip_len = 0;
    p->promised = false;
}

/* The tail it leaves spans no more than the Slips under it and the write. */
uint64_t PlotSlipMost(const Plot *p)
{
    uint32_t lo = 0;
    if (!p->slip)
        return 0;
    uint64_t k    = p->size_before ? (p->size_before - 1u) / PLOT_BLOCK : 0;
    uint32_t span = p->rec && TailSlipOn(p->rec, k) ? TailSlipHull(p->rec->slip, &lo) : 0;
    uint32_t a    = span && lo < p->slip_off ? lo : p->slip_off;
    uint32_t b    = span && lo + span > p->slip_off + p->slip_len ? lo + span : p->slip_off + p->slip_len;
    return b - a;
}

error_t PlotEngross(TagFSVolume* vol, Plot *p, bool *laid)
{
    *laid = false;
    if (!p->rec || !p->size || !TailSlipOn(p->rec, (p->size - 1u) / PLOT_BLOCK))
        return OK;
    uint64_t k    = (p->size - 1u) / PLOT_BLOCK;
    error_t  rc   = PlotWindowCover(p, k, p->rec->blocks);
    if (rc == OK)
        rc = PlotWindowTail(vol, p, k, (uint32_t)(p->size - k * PLOT_BLOCK));
    if (rc == OK) {
        p->data_need = VolumeFlushNeed(vol);
        *laid        = true;
#ifdef CONFIG_TAGFS_SETTLE_CUT_POINT
        p->engross = true;
#endif
    }
    return rc;
}
