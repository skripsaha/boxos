#include "helm.h"
#include "../ink/ink.h"
#include "../settle/settle.h"

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
 * holder before it freed is not this one's to keep resting. */
static error_t HelmPlotLoad(TagFSFileHandle *handle, Plot *plot)
{
    if (HelmVacated(handle->ofe)) {
        memset(plot, 0, sizeof(*plot));
        return ERR_FILE_NOT_FOUND;
    }
    if (!handle->extents)
        HelmViewTake(handle);
    return PlotLoad(plot, handle->file_id);
}

/*
 * One write: the ground it needs reserved and the record's room checked before
 * a byte moves, the blocks written by the one read-modify-write rule, and what
 * landed inked as one record (TagFSInkPlot). A write the medium stops part way
 * keeps what landed before the stop and says why; ground it took past that goes
 * back (CR-13), and a write that inks nothing gives back everything it took
 * (CR-1) — a file given up meanwhile among them: its Ink finds no record.
 */
static error_t HelmWriteOnce(TagFSFileHandle *handle, const PlotSource *src,
                             uint64_t *out_written)
{
    Plot     plot;
    uint64_t at     = 0;
    uint64_t done   = 0;
    bool     append = (handle->flags & TAGFS_HANDLE_APPEND) != 0;
    error_t  rc     = HelmPlotLoad(handle, &plot);
    bool     loaded = rc == OK;

    if (rc == OK)
        rc = PlotBeginWrite(&plot, handle->offset, append, src->bytes, &at);
    if (rc == OK)
        rc = PlotWriteBlocks(&plot, at, src, &done);
    if (done > 0) {
        InkReceipt inked;
        error_t    kept = PlotEndWrite(&plot, at, done);
        if (kept == OK)
            kept = TagFSInkPlot(&plot, INK_MAKES_SPARES, &inked);
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
    *out_written = done;
    PlotRelease(&plot);
    return rc;
}

error_t HelmWrite(TagFSFileHandle *handle, const PlotSource *src, uint64_t *out_written)
{
    *out_written = 0;
    if (!(handle->flags & TAGFS_HANDLE_WRITE))
        return ERR_PERMISSION_DENIED;
    error_t rc;
    do {
        rc = HelmWriteOnce(handle, src, out_written);
    } while (*out_written == 0 && HelmRoomCameBack(&rc));
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
