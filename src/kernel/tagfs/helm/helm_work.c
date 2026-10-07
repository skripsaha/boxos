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
 * ground a Settle cycle gives back. A caller in the kernel standing in its
 * call has the cycles run until some came back (SettleForRoom) and makes the
 * call again: true to make it again; otherwise *rc is the answer — the
 * allocator's own once nothing is on its way.
 */
bool HelmRoomCameBack(error_t *rc)
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
 * Helm's holder's, guarded with the warrant the handle carries, and the room
 * its extents need in the record is claimed for that Helm (RT-19,
 * PlotReserve). */
static error_t HelmPlotLoad(TagFSFileHandle *handle, Plot *plot)
{
    if (HelmVacated(handle->ofe)) {
        memset(plot, 0, sizeof(*plot));
        return ERR_FILE_NOT_FOUND;
    }
    if (!handle->extents)
        HelmViewTake(handle);
    error_t rc = PlotLoad(plot, handle->file_id, handle->warrant);
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
 * record. A stop the medium's leaving explains declares nothing of the window
 * written (RT-13): what landed of it may be in a cache that went with the
 * medium.
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
    if (rc == ERR_DEVICE_NOT_READY)
        done = 0;
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
    return HelmWriteWindowOnce(handle, src, inked);
}

error_t HelmWrite(TagFSFileHandle *handle, const PlotSource *src, uint64_t *out_written)
{
    error_t rc;
    *out_written = 0;
    do {
        rc = HelmWriteWindow(handle, src, out_written);
    } while ((rc == OK && *out_written < src->bytes) || HelmRoomCameBack(&rc));
    return rc;
}

// ----------------------------------------------------------------------------
// span — a file made exactly as long as asked
//
// It replaces the truncate this kernel had, which only ever shrank: a file
// could be cut, and it grew only by being written at its end. span also grows
// one across a gap, and the gap reads as zeros, written to ground taken for
// it — a write can never make a gap on its own (ERR_TAGFS_BEYOND_SIZE).
//
// ‼ A SPAN GROWS A WINDOW AT A TIME, AS A WRITE DOES (R24). Growth is zeros
// laid on ground taken for them, and a span of gigabytes was one piece: every
// block of it reserved before the first zero moved, and the holder's core held
// for all of it — on one core with interrupts off. Each window now is its own
// load, reservation, zeros and Ink, so a span never holds more than a window's
// ground unnamed, and its holder may give its core back between two. Before
// the first block of a window is taken the volume is asked whether it could
// give the whole growth at all, so a span past every block the volume has is
// refused before it eats them. A cut is one window: it lays no block.
// ----------------------------------------------------------------------------

/* The span's next window under the Plot loaded for it: the file grown by a
 * window towards `size`, or cut to it, and inked. *out_ended once the file is
 * `size` long — or, for `grow_only`, once it is no shorter, which a file
 * already longer is at once and is left as it is. */
static error_t HelmSpanStep(Plot *plot, uint64_t size, bool grow_only, bool *out_ended)
{
    if (plot->size == size || (grow_only && plot->size > size)) {
        *out_ended = true;
        return OK;
    }

    error_t rc = OK;
    if (size < plot->size) {
        rc         = PlotTrimTo(plot, size / TAGFS_BLOCK_SIZE + (size % TAGFS_BLOCK_SIZE != 0));
        plot->size = size;
        *out_ended = true;
    } else {
        uint64_t end = plot->size + HelmWindowBytes(plot->size);
        if (end > size || end < plot->size)
            end = size;
        rc = PlotRoomFor(plot, size);
        if (rc == OK)
            rc = PlotExtendZero(plot, end);
        *out_ended = end == size;
    }
    InkReceipt inked;
    if (rc == OK)
        rc = TagFSInkPlot(plot, INK_MAKES_SPARES, &inked);
    return rc;
}

error_t HelmSpanWindow(TagFSFileHandle *handle, uint64_t size, bool grow_only,
                       uint64_t *out_found, bool *out_ended)
{
    Plot    plot;
    error_t rc     = HelmPlotLoad(handle, &plot);
    bool    loaded = rc == OK;

    *out_ended = false;
    if (loaded) {
        *out_found = plot.size;
        rc         = HelmSpanStep(&plot, size, grow_only, out_ended);
        handle->file_size = plot.size_before;
    }
    PlotRelease(&plot);
    return rc;
}

error_t HelmSpan(TagFSFileHandle *handle, uint64_t size, uint64_t *out_was)
{
    uint64_t found = UINT64_MAX;
    bool     ended = false;
    error_t  rc;

    *out_was = 0;
    do {
        uint64_t at = UINT64_MAX;
        rc = HelmSpanWindow(handle, size, false, &at, &ended);
        if (found == UINT64_MAX && at != UINT64_MAX)
            *out_was = found = at;
    } while ((rc == OK && !ended) || HelmRoomCameBack(&rc));
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

void HelmDefragLetGo(HelmDefragging *d)
{
    PlotRelease(&d->plot);
    memset(d, 0, sizeof(*d));
}

/* The defrag's first window: the file's record loaded and its ground past
 * its size cut off, and — unless it lies in one run already, which is inked
 * only when the cut freed ground — the run it is gathered into taken. Room on
 * its way leaves nothing of it held: the next turn loads afresh. */
static error_t HelmDefragBegin(TagFSFileHandle *handle, HelmDefragging *d, bool *out_ended)
{
    Plot   *plot = &d->plot;
    error_t rc   = HelmPlotLoad(handle, plot);
    d->loaded = true;
    if (rc == OK)
        rc = PlotTrimTo(plot, plot->size / TAGFS_BLOCK_SIZE + (plot->size % TAGFS_BLOCK_SIZE != 0));
    if (rc == OK && HelmRunsAreOne(plot->runs, plot->run_count)) {
        InkReceipt inked;
        if (plot->freed_count > 0)
            rc = TagFSInkPlot(plot, INK_MAKES_SPARES, &inked);
        *out_ended = rc == OK;
    } else if (rc == OK) {
        rc = PlotGatherBegin(plot, 0, plot->runs[0].start_block, &d->gather);
    }
    if (rc == ERR_WOULD_BLOCK)
        HelmDefragLetGo(d);
    return rc;
}

/*
 * Defrag is a gather of every extent (PlotGather, shed 0) under the file's
 * Helm, so no write and no span of the file runs beside it (D7-defrag): the
 * blocks are copied into one run taken through the block map near where the
 * file begins — a window a step, the holder free to give its core back between
 * two (R24) — made durable, and named by one inked record, and only then does
 * the old ground come to rest in Fallow. Ground the record named past the
 * file's size goes with it. A file the system keeps is not moved (PlotGather,
 * B10). A delete beside it cannot bring the file back (CR-9): the next window
 * finds the file given up, or its Quitclaim leaves no record for this Ink to
 * lay the gathered run over, and the run goes back.
 */
error_t HelmDefragWindow(TagFSFileHandle *handle, HelmDefragging *d, bool *out_ended)
{
    *out_ended = false;
    if (!d->loaded) {
        error_t rc = HelmDefragBegin(handle, d, out_ended);
        if (rc != OK || *out_ended)
            return rc;
    } else if (HelmVacated(handle->ofe)) {
        return ERR_FILE_NOT_FOUND;
    }

    bool    laid = false;
    error_t rc   = PlotGatherStep(&d->plot, &d->gather, &laid);
    if (rc == OK && laid) {
        InkReceipt inked;
        rc         = TagFSInkPlot(&d->plot, INK_MAKES_SPARES, &inked);
        *out_ended = rc == OK;
    }
    return rc;
}

error_t HelmDefrag(TagFSFileHandle *handle)
{
    HelmDefragging d;
    bool           ended = false;
    error_t        rc;

    memset(&d, 0, sizeof(d));
    do {
        rc = HelmDefragWindow(handle, &d, &ended);
    } while ((rc == OK && !ended) || HelmRoomCameBack(&rc));
    HelmDefragLetGo(&d);
    return rc;
}
