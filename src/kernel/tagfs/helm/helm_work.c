#include "helm.h"
#include "../ink/ink.h"
#include "../settle/settle.h"
#include "../run/run.h"  /* TagFSRunBlocks — how much the medium takes in one command */

/*
 * The work a Helm's holder does, synchronously (helm.h). The holder is the only
 * one who changes the file's extents while it works, so each loads the file's
 * record afresh once the Helm is its own and inks the one record its Plot
 * builds — nobody's Ink came between the load and its own.
 */

/*
 * A call that answered ERR_WOULD_BLOCK changed nothing: the volume is short of
 * ground Fallow still rests, which a Settle cycle gives back. The holder holds
 * no lock, so it has cycles run until some ground came back (SettleForRoom)
 * and makes the call again. True to make it again; otherwise *rc is the answer
 * — ERR_TAGFS_NO_SPACE once nothing rests.
 */
static bool HelmRoomCameBack(error_t *rc)
{
    if (*rc != ERR_WOULD_BLOCK)
        return false;
    *rc = SettleForRoom();
    return *rc == OK;
}

/* The file's record for this call's Plot: ERR_FILE_NOT_FOUND at once for a
 * file a Quitclaim gave up, with nothing loaded. A handle that carries no copy
 * of the record views the file from this load on, not from its open: what a
 * holder before it freed is not this one's to keep resting. The Plot is the
 * Helm's holder's, and the room its extents need in the record is claimed for
 * that Helm (RT-19, PlotReserve). */
static error_t HelmPlotLoad(TagFSFileHandle *handle, Plot *plot)
{
    if (HelmVacated(handle->ofe)) {
        memset(plot, 0, sizeof(*plot));
        return ERR_FILE_NOT_FOUND;
    }
    if (!handle->extents)
        HelmViewTake(handle);
    error_t rc = PlotLoad(plot, handle->file_id);
    if (rc == OK)
        plot->helm = &handle->ofe->helm;
    return rc;
}

/* A window is the run the medium takes in one command, in whole blocks and at
 * least one — the unit the stream moves (plot_write.c) is the unit a record is
 * inked for, and a window that lies in one run on the medium goes out in one
 * command — and it ends at a block's end of the file, so the next window
 * begins a block and reads nothing back of this one. */
uint64_t HelmWindowBytes(uint64_t at)
{
    return (uint64_t)TagFSRunBlocks() * TAGFS_BLOCK_SIZE - at % TAGFS_BLOCK_SIZE;
}

/* The part of the source the window beginning at file offset `at` writes: from
 * its byte `from`, up to the window's end. */
static PlotSource HelmWindowOf(const PlotSource *src, uint64_t from, uint64_t at)
{
    PlotSource window = *src;
    uint64_t   most   = HelmWindowBytes(at);
    uint64_t   left   = src->bytes - from;

    window.bytes = left < most ? left : most;
    if (src->kernel)
        window.kernel = src->kernel + from;
    else
        window.user = src->user + (uintptr_t)from;
    return window;
}

/*
 * One window: the ground it needs reserved and the record's room claimed
 * before a byte moves, the blocks written by the one read-modify-write rule,
 * and what landed inked as one record (TagFSInkPlot). A window the medium stops
 * part way keeps what landed before the stop and says why; ground it took past
 * that goes back (CR-13), and a window that inks nothing gives back everything
 * it took (CR-1) — a file given up meanwhile among them: its Ink finds no
 * record.
 */
static error_t HelmWriteWindowOnce(TagFSFileHandle *handle, const PlotSource *src,
                                   uint64_t *inked)
{
    Plot       plot;
    PlotSource window = { 0 };
    uint64_t   at     = 0;
    uint64_t   done   = 0;
    error_t    rc     = HelmPlotLoad(handle, &plot);
    bool       loaded = rc == OK;

    if (rc == OK) {
        at     = (handle->flags & TAGFS_HANDLE_APPEND) ? plot.size : handle->offset;
        window = HelmWindowOf(src, *inked, at);
        rc     = PlotBeginWrite(&plot, at, false, window.bytes, &at);
    }
    if (rc == OK)
        rc = PlotWriteBlocks(&plot, at, &window, &done);
    if (done > 0) {
        InkReceipt receipt;
        error_t    kept = PlotEndWrite(&plot, at, done);
        if (kept == OK)
            kept = TagFSInkPlot(&plot, INK_MAKES_SPARES, &receipt);
        if (kept != OK) {
            if (rc == OK)
                rc = kept;
            done = 0;
        }
    }

    if (loaded) {
        handle->file_size = plot.size_before;
        if (done > 0)
            handle->offset = at + done;
    }
    *inked += done;
    PlotRelease(&plot);
    return rc;
}

error_t HelmWriteWindow(TagFSFileHandle *handle, const PlotSource *src, uint64_t *inked)
{
    if (!(handle->flags & TAGFS_HANDLE_WRITE))
        return ERR_PERMISSION_DENIED;
    if (*inked >= src->bytes)
        return OK;
    uint64_t before = *inked;
    error_t  rc;
    do {
        rc = HelmWriteWindowOnce(handle, src, inked);
    } while (*inked == before && HelmRoomCameBack(&rc));
    return rc;
}

error_t HelmWrite(TagFSFileHandle *handle, const PlotSource *src, uint64_t *out_written)
{
    error_t rc;
    *out_written = 0;
    do {
        rc = HelmWriteWindow(handle, src, out_written);
    } while (rc == OK && *out_written < src->bytes);
    return rc;
}

// ----------------------------------------------------------------------------
// span — a file made exactly as long as asked
//
// It replaces the truncate this kernel had, which only ever shrank: a file
// could be cut, and it grew only by being written at its end. span also grows
// one across a gap, and the gap reads as zeros, written to ground taken for
// it — a write can never make a gap on its own (ERR_TAGFS_BEYOND_SIZE).
// ----------------------------------------------------------------------------

static error_t HelmSpanOnce(TagFSFileHandle *handle, uint64_t size, uint64_t *out_was)
{
    Plot    plot;
    error_t rc = HelmPlotLoad(handle, &plot);
    if (rc == OK)
        *out_was = plot.size;
    if (rc == OK && size != plot.size) {
        if (size < plot.size) {
            rc = PlotTrimTo(&plot, (size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE);
            plot.size = size;
        } else {
            rc = PlotExtendZero(&plot, size);
        }
        InkReceipt inked;
        if (rc == OK)
            rc = TagFSInkPlot(&plot, INK_MAKES_SPARES, &inked);
    }
    if (rc == OK)
        handle->file_size = size;
    PlotRelease(&plot);
    return rc;
}

error_t HelmSpan(TagFSFileHandle *handle, uint64_t size, uint64_t *out_was)
{
    error_t rc;
    *out_was = 0;
    do {
        rc = HelmSpanOnce(handle, size, out_was);
    } while (HelmRoomCameBack(&rc));
    return rc;
}

// ----------------------------------------------------------------------------
// Defrag
// ----------------------------------------------------------------------------

/* Whether the runs are one run on the medium, each beginning where the one
 * before it ends. */
static bool HelmRunsAreOne(const FileExtent *runs, uint32_t count)
{
    for (uint32_t i = 1; i < count; i++)
        if (runs[i].start_block != runs[i - 1].start_block + runs[i - 1].block_count)
            return false;
    return true;
}

/*
 * Defrag is a gather of every extent (PlotGather, shed 0) under the file's
 * Helm, so no write and no span of the file runs beside it (D7-defrag): the
 * blocks are copied into one run taken through the block map near the target,
 * made durable, and named by one inked record, and only then does the old
 * ground come to rest in Fallow. Ground the record named past the file's size
 * goes with it. A file the loader reads by its blocks is not moved. A delete
 * beside it cannot bring the file back (CR-9): its Quitclaim leaves no record
 * for this Ink to lay the gathered run over, and the run goes back.
 */
static error_t HelmDefragOnce(TagFSFileHandle *handle, uint32_t target_block)
{
    Plot    plot;
    bool    moved = false;
    error_t rc    = HelmPlotLoad(handle, &plot);
    if (rc == OK)
        rc = PlotTrimTo(&plot, (plot.size + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE);
    if (rc == OK && !HelmRunsAreOne(plot.runs, plot.run_count)) {
        rc    = PlotGather(&plot, 0, target_block);
        moved = rc == OK;
    }
    InkReceipt inked;
    if (rc == OK && (moved || plot.freed_count > 0))
        rc = TagFSInkPlot(&plot, INK_MAKES_SPARES, &inked);
    PlotRelease(&plot);
    return rc;
}

error_t HelmDefrag(TagFSFileHandle *handle, uint32_t target_block)
{
    error_t rc;
    do {
        rc = HelmDefragOnce(handle, target_block);
    } while (HelmRoomCameBack(&rc));
    return rc;
}
