/*
 * WriteJob — the one automaton of a file's changes a program asks for
 * (write_job.h).
 *
 * A synchronous job runs under the file's Helm — the caller's call when the
 * Helm was free, a service point when it was handed on: a span and a defrag in
 * one piece (HelmSpan, HelmDefrag), a write a window at a time
 * (HelmWriteWindow). An asynchronous write is a state machine pumped from
 * K-Core continuations posted by the AHCI IRQ. The IRQ side is intentionally
 * minimal: stash status, set the next state, post the continuation. The K-Core
 * side does everything that needs locks (the Plot, the record's Ink, the Helm,
 * Touch publish).
 *
 * The asynchronous write's states:
 *
 *   W_HELM_WAIT? → W_ALLOC → W_LOCATE
 *
 *   per block, by the one read-modify-write rule (PlotFillFor):
 *     W_LOCATE → (the file's bytes, partly overlaid → W_READ_OLD → W_DMA_FILL)
 *              → (a block that held nothing of the file → W_DMA_FILL)  [zeros]
 *              → (covered whole                         → W_DMA_FILL)
 *     W_DMA_FILL → W_AHCI_SUBMIT → (yield to IRQ) → W_AHCI_DONE
 *     W_AHCI_DONE → if more bytes of the window: W_LOCATE
 *                 → else:                        W_LOG_META
 *
 *   the record, once a window:
 *     W_LOG_META → W_INK → (more windows: yield to the K-Core → W_ALLOC)
 *                        → (the last one:                     W_DONE)
 *     W_INK with no Spare to close the pool's tail onto:
 *              → W_SPARE → (yield to IRQ) → W_SPARE_DONE → W_INK
 *
 * ‼ A WRITE IS INKED A WINDOW AT A TIME (RT-16), ON EITHER ROAD. The window is
 * the run the medium takes in one command (HelmWindowBytes): its ground is
 * reserved and the record's room claimed before a byte of it moves (RT-19),
 * what landed of it is inked as one record (TagFSInkPlot), and between two
 * windows the job gives its core back while it keeps the Helm — a synchronous
 * job is listed for the next service point (HelmReady), its caller parked, an
 * asynchronous one posts itself behind what the drain core has waiting. A
 * write of any length holds no more than a window's ground unnamed, and a
 * K-Core for no longer than one window.
 *
 * ‼ ONE ANSWER TO EVERY WRITE (W-ERR): the reason, the bytes the file's inked
 * record holds of the write (u64), and the size it left — in the out crate
 * [u64 written][u64 size] on every answer, a refusal included, and said on the
 * log for a refusal. A refusal on the way — a read of an overlaid block, a
 * block's write, the program's pages, the record — stops the job there: the
 * windows inked before it are the file's, what landed of the window it stopped
 * in is inked and counted, and the ground reserved past it goes back. Whenever
 * the file changed the listeners hear of it. A job that inks nothing gives
 * back everything it took (CR-1), and never a block twice (W-DFREE): what it
 * took is the Plot's Stakes, given back once. A file given up meanwhile is not
 * written further: its Helm is vacated, and the job answers ERR_FILE_NOT_FOUND
 * with its Stakes given back. A job waiting for the Helm when the medium left
 * is answered ERR_DEVICE_NOT_READY, nothing of it done (RT-13).
 *
 * ‼ NO FLUSH OF ITS OWN. What a job inked reaches the medium with a Settle
 * cycle — the cycle's first flush covers the data before any record names it
 * (settle.h). A flush after every write was a SYNC CACHE per fwrite and proved
 * nothing about the record, which no cycle had written.
 *
 * ‼ A SPARE IS MADE THE ASYNCHRONOUS JOB'S OWN WAY. A continuation may not
 * stand over the medium, so an Ink with no Spare to close the tail onto answers
 * ERR_WOULD_BLOCK (INK_ASKS_SPARES) and the job stamps one through the port
 * like any block of its own (W_SPARE), hands it to the pool and inks again.
 * And a volume short of ground Fallow still rests turns the job synchronous:
 * a continuation cannot wait for the Settle cycle that gives it back, and a
 * service point can (HelmWriteWindow), from the last window inked.
 *
 * The bytes come out of the program's pages a block at a time, into the DMA
 * page they go out of — never copied whole into the kernel heap first.
 *
 * Nothing here copies a block for a snapshot: a format-v1 volume takes none
 * (TagFS_SnapshotRefused), so every block is written where the file has it.
 */

#include "write_job.h"
#include "chit.h"        /* ChitGive / ChitDue — an answer put off is an answer promised */
#include "boardroom.h"
#include "baton.h"
#include "crate_stage.h"
#include "crate_io.h"
#include "op_registry.h"
#include "tagfs.h"
#include "helm.h"        /* the file's Helm, and the work its holder does */
#include "ahci.h"
#include "ahci_async.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "kring.h"
#include "kresult.h"
#include "klib.h"
#include "atomics.h"
#include "amp.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "touch.h"
#include "integrity.h"
#include "ink.h"         /* TagFSInkPlot — the one way the file changes */
#include "meta_pool.h"   /* PoolSpareBegin / Adopt / Drop — a Spare stamped through the port */
#include "scribe.h"      /* VolumeFlushNeed — what the landed blocks wait for */
#ifdef CONFIG_VOLUME_TESTS
#include "block_fail.h"  /* BLOCKFAIL — the port's commands refused on the stand */
#endif

/* ---- forward decls ---- */
static void wjob_pump(void *job_);
static int  wjob_finalize(WriteJob *j, error_t rc);
static void wjob_ahci_complete(uint8_t port, uint8_t slot, error_t status, void *ctx);

static inline void wjob_to(WriteJob *j, WriteJobState s)
{
    atomic_store_u32((volatile uint32_t *)&j->state, (uint32_t)s);
}

/* =========================================================================
 *  The Helm
 * ========================================================================= */

static void wjob_defer(WriteJob *j, const OpContext *ctx);

/* The write's next window, synchronously, under the Helm the job holds: from
 * the first byte no window inked yet, where the last one ended. */
static error_t wjob_window(WriteJob *j)
{
    j->handle->offset = j->bytes_inked ? j->start_offset + j->bytes_inked : j->asked_offset;
    error_t rc = HelmWriteWindow(j->handle, &j->source, &j->bytes_inked);
    if (j->bytes_inked)
        j->start_offset = j->handle->offset - j->bytes_inked;
    j->final_size = j->handle->file_size;
    return rc;
}

/* A synchronous job's turn under the Helm it holds: a span or a defrag whole,
 * a write one window. A write with windows left gives its core back — its
 * caller parked if it was not yet, the job listed for the next service point,
 * and the turn ends there; a caller in the kernel has no core to give back,
 * and its windows run on. The handler's answer when the job ended inside its
 * caller's call, else ERR_WOULD_BLOCK: the answer is the job's to push. */
static int wjob_turn(WriteJob *j)
{
    if (j->kind == WRITE_JOB_SPAN)
        return wjob_finalize(j, HelmSpan(j->handle, j->span_size, &j->span_was));
    if (j->kind == WRITE_JOB_DEFRAG)
        return wjob_finalize(j, HelmDefrag(j->handle, j->defrag_target));

    for (;;) {
        error_t rc = wjob_window(j);
        if (rc != OK || j->bytes_inked >= j->source.bytes)
            return wjob_finalize(j, rc);
        if (!j->target)
            continue;
        if (!j->deferred)
            wjob_defer(j, j->ctx);
        HelmReady(&j->helm);   /* the job is the service point's now: not touched again here */
        return ERR_WOULD_BLOCK;
    }
}

/* A synchronous job handed the Helm, or come back for its next window, is run
 * at a service point. */
static void wjob_run_ready(HelmWaiter *w)
{
    (void)wjob_turn(container_of(w, WriteJob, helm));
}

/* The medium left while the job waited for the Helm: nothing of it was done,
 * and it is answered so, with what left with the medium. */
static void wjob_gone(HelmWaiter *w, uint64_t lost)
{
    WriteJob *j = container_of(w, WriteJob, helm);
    j->medium_left = true;
    j->lost        = lost;
    (void)wjob_finalize(j, ERR_DEVICE_NOT_READY);
}

/* HelmHand's call, on the core that let go, with no TagFS lock held. */
static void wjob_handed(HelmWaiter *w)
{
    WriteJob *j = container_of(w, WriteJob, helm);
    j->holds_helm = true;
    if (j->async) {
        wjob_to(j, W_ALLOC);
        BatonPass(&j->cq_node);   /* never-drop; routes to the drain core */
    } else {
        HelmReady(w);
    }
}

/* =========================================================================
 *  State handlers — the asynchronous write
 * ========================================================================= */

/* The write stops at a refusal. What landed of the window before it is made
 * the file's — one record, inked the way a whole window's is, the ground
 * reserved past it given back — and the refusal is the answer, with the bytes
 * the file now holds of the write. Answers whether the pump goes on. */
static bool wjob_stop(WriteJob *j, error_t cause)
{
    if (j->bytes_done > j->bytes_inked && j->loaded) {
        j->stopped = cause;
        wjob_to(j, W_LOG_META);
        return true;
    }
    wjob_finalize(j, cause);
    return false;
}

/* The file's record loaded under the Helm, for the job's first window: the
 * Plot is the Helm holder's, and the room its extents need is claimed for it
 * (RT-19); where the write begins is asked of the growth rule once. */
static error_t wjob_load(WriteJob *j)
{
    if (HelmVacated(j->handle->ofe))
        return ERR_FILE_NOT_FOUND;
    HelmViewTake(j->handle);   /* it views the file from this load on (HelmWriteWindow's rule) */
    error_t rc = PlotLoad(&j->plot, j->file_id);
    j->loaded = rc == OK;
    if (rc == OK) {
        j->plot.helm    = &j->handle->ofe->helm;
        j->start_offset = j->append ? j->plot.size : j->asked_offset;
    }
    return rc;
}

/* W_ALLOC — the window's blocks reserved and the record's room claimed before
 * any byte of it moves; every window after the first goes on from where the
 * last one's Ink left the Plot. A volume short of ground Fallow rests turns
 * the job synchronous: the service point that runs it may wait for the cycle
 * that gives the ground back, and goes on from the last window inked. */
static bool w_alloc(WriteJob *j)
{
    error_t  rc     = j->loaded ? OK : wjob_load(j);
    uint64_t at     = j->start_offset + j->bytes_inked;
    uint64_t window = 0;
    if (rc == OK) {
        uint64_t left = j->source.bytes - j->bytes_inked;
        window = HelmWindowBytes(at);
        if (window > left)
            window = left;
        rc = PlotBeginWrite(&j->plot, at, false, window, &at);
    }
    if (rc == ERR_WOULD_BLOCK) {
        PlotRelease(&j->plot);
        j->loaded = false;
        j->async  = false;
        HelmReady(&j->helm);
        return false;
    }
    if (rc != OK) {
        wjob_finalize(j, rc);
        return false;
    }
    j->window_end = j->bytes_inked + window;
    wjob_to(j, W_LOCATE);
    return true;
}

/* W_LOCATE — the block the cursor is in, and what the rest of it needs: a
 * read of the file's bytes, zeros, or nothing. A file given up meanwhile is
 * written no further. */
static bool w_locate(WriteJob *j)
{
    if (HelmVacated(j->handle->ofe)) {
        wjob_finalize(j, ERR_FILE_NOT_FOUND);
        return false;
    }

    uint64_t pos   = j->start_offset + j->bytes_done;
    uint64_t k     = pos / TAGFS_BLOCK_SIZE;
    uint32_t off   = (uint32_t)(pos % TAGFS_BLOCK_SIZE);
    uint64_t rest  = j->window_end - j->bytes_done;
    uint32_t chunk = TAGFS_BLOCK_SIZE - off;
    if ((uint64_t)chunk > rest)
        chunk = (uint32_t)rest;

    /* The run the last block came from while it lasts, a search when it ends
     * or a window begins — the Ink before it may have cut the run: every chunk
     * after a window's first begins a block. */
    if (j->bytes_done > j->bytes_inked && j->if_left > 0) {
        j->if_disk_block++;
        j->if_left--;
    } else if (!PlotFind(j->plot.runs, j->plot.run_count, k, &j->if_disk_block, &j->if_left)) {
        kprintf("[Storage] write to file %u: no extent of its plot holds block %llu, which the "
                "write reserved — the write stops at byte %llu\n", j->file_id,
                (unsigned long long)k, (unsigned long long)pos);
        return wjob_stop(j, ERR_TAGFS_EXTENT_ERROR);
    }

    j->if_off_in_blk = off;
    j->if_chunk      = chunk;
    switch (PlotFillFor(&j->plot, k, off, chunk)) {
    case PLOT_FILL_READ:
        wjob_to(j, W_READ_OLD);
        return true;
    case PLOT_FILL_FRESH:
        memset(j->dma_virt, 0, TAGFS_BLOCK_SIZE);
        break;
    case PLOT_FILL_WHOLE:
        break;
    }
    wjob_to(j, W_DMA_FILL);
    return true;
}

/* One block of this job to the port. "Every command slot on the port is
 * taken" is not "the disk failed", and it is not "come back later" either: the
 * job's berth carries the request, and the port issues it now or moors it in
 * its roadstead — under the same lock that found no slot, so a slot freed a
 * moment later calls it in — until a slot comes free or the door a cache flush
 * shut opens again. Either way `answered` runs, exactly once; an error here
 * means the port is not there, or the request is malformed. */
static error_t wjob_submit(WriteJob *j, bool write, RoadsteadAnswered answered)
{
    RoadsteadBerth *b = &j->berth;
    memset(b, 0, sizeof(*b));
    b->lba      = tagfs_block_to_sector(j->if_disk_block);
    b->sectors  = TAGFS_BLOCK_SECTORS;
    b->dma_phys = j->dma_phys;
    b->answered = answered;
    b->ctx      = j;

    j->if_reading = !write;
    uint8_t port = BoardroomSeatIndex(tagfs_get_seat());
#ifdef CONFIG_VOLUME_TESTS
    /* BLOCKFAIL, asked the way the Boardroom asks it of an asynchronous read:
     * the command is taken and answered through the callback with ERR_IO, as
     * a real completion can already arrive before the submit returns. Nothing
     * is issued. */
    if (BlockFailStrikes(tagfs_get_seat(), write ? BLOCK_FAIL_WRITE : BLOCK_FAIL_READ,
                         b->lba, b->sectors)) {
        answered(port, 0, ERR_IO, j);
        return OK;
    }
#endif
    return write ? ahci_submit_write_async(port, b) : ahci_submit_read_async(port, b);
}

/* W_READ_OLD — async read of the block a partial write overlays into DMA,
 * so the user bytes fold over what the block holds. IRQ jumps state to
 * W_DMA_FILL via wjob_read_old_complete. */
static void wjob_read_old_complete(uint8_t port, uint8_t slot,
                                   error_t status, void *ctx);

static bool w_read_old(WriteJob *j)
{
    error_t err = wjob_submit(j, false, wjob_read_old_complete);
    if (err != OK)
        return wjob_stop(j, err);
    return false;  /* parked until IRQ */
}

/* W_DMA_FILL — the write's bytes for this block, out of the program's pages,
 * laid over what is staged in the DMA page. */
static bool w_dma_fill(WriteJob *j)
{
    error_t rc = PlotSourcePull(&j->source, j->bytes_done,
                                (uint8_t *)j->dma_virt + j->if_off_in_blk, j->if_chunk);
    if (rc != OK) {
        kprintf("[Storage] write to file %u: the bytes from %llu on could not be taken from "
                "the pages they are in (error %u) — %llu of %llu written\n", j->file_id,
                (unsigned long long)j->bytes_done, (unsigned)rc,
                (unsigned long long)j->bytes_done, (unsigned long long)j->source.bytes);
        return wjob_stop(j, rc);
    }
    wjob_to(j, W_AHCI_SUBMIT);
    return true;
}

/* W_AHCI_SUBMIT — fire write, yield to IRQ. */
static bool w_ahci_submit(WriteJob *j)
{
    error_t err = wjob_submit(j, true, wjob_ahci_complete);
    if (err != OK)
        return wjob_stop(j, err);
    return false;  /* parked until IRQ */
}

/* W_AHCI_DONE — IRQ retired. Advance and decide next phase. A refused read of
 * the block being overlaid stops the write before the block is written: the
 * bytes around the write are the file's, and zeros laid over them would be a
 * write nobody asked for (S1). */
static bool w_ahci_done(WriteJob *j)
{
    if (j->if_status != OK) {
        if (j->if_reading)
            kprintf("[Storage] write to file %u: block %u would not read, so the %u byte(s) "
                    "meant for it are not laid over the rest of it — %llu of %llu written\n",
                    j->file_id, j->if_disk_block, j->if_chunk,
                    (unsigned long long)j->bytes_done, (unsigned long long)j->source.bytes);
        else
            kprintf("[Storage] write to file %u: block %u would not be written — %llu of %llu "
                    "bytes had landed before the medium stopped answering\n", j->file_id,
                    j->if_disk_block, (unsigned long long)j->bytes_done,
                    (unsigned long long)j->source.bytes);
        return wjob_stop(j, j->if_status);
    }

    /* The async path writes straight to the AHCI port, bypassing
     * tagfs_write_block — so drop the just-written block from the read-ahead
     * cache ourselves, or a later read could serve pre-write data. */
    tagfs_readahead_invalidate(j->if_disk_block);
    IntegrityUpdate(j->if_disk_block, j->dma_virt);

    j->bytes_done += j->if_chunk;
    wjob_to(j, j->bytes_done >= j->window_end ? W_LOG_META : W_LOCATE);
    return true;
}

/* W_LOG_META — what landed of the window made ready to be the file's: the
 * flush need its blocks hold, read after their completions were seen, and the
 * ground reserved past them cut off. */
static bool w_log_meta(WriteJob *j)
{
    j->plot.data_need = VolumeFlushNeed();
    error_t rc = PlotEndWrite(&j->plot, j->start_offset + j->bytes_inked,
                              j->bytes_done - j->bytes_inked);
    if (rc != OK) {
        wjob_finalize(j, rc);
        return false;
    }
    wjob_to(j, W_INK);
    return true;
}

/* W_INK — the window's record the Plot builds, inked; with no Spare for the
 * pool's tail, one is stamped first (W_SPARE). A file given up meanwhile has
 * no record to ink over, and the answer is ERR_FILE_NOT_FOUND. With windows
 * left, the job posts itself behind what the drain core has waiting before it
 * reserves the next one. */
static bool w_ink(WriteJob *j)
{
    InkReceipt inked;
    error_t    rc = TagFSInkPlot(&j->plot, INK_ASKS_SPARES, &inked);
    if (rc == ERR_WOULD_BLOCK) {
        wjob_to(j, W_SPARE);
        return true;
    }
    if (rc != OK) {
        wjob_finalize(j, rc);
        return false;
    }
    j->bytes_inked = j->bytes_done;
    j->final_size  = j->plot.size;
    if (j->stopped != OK || j->bytes_inked >= j->source.bytes) {
        wjob_to(j, W_DONE);
        return true;
    }
    wjob_to(j, W_ALLOC);
    BatonPass(&j->cq_node);   /* the next window, after what the drain core has waiting */
    return false;
}

static void wjob_spare_complete(uint8_t port, uint8_t slot, error_t status, void *ctx);

/* W_SPARE — a block taken for the pool and its empty stamp written through
 * the port, from the job's DMA page like any block of the write. A volume
 * with no block for it ends the job with the allocator's answer: nothing is
 * inked, and the ground the write took goes back. */
static bool w_spare(WriteJob *j)
{
    error_t rc = PoolSpareBegin(&j->spare_block, &j->spare_image);
    if (rc != OK) {
        kprintf("[Storage] write to file %u: the pool has no Spare to take its record and "
                "none could be made (error %u) — the write is refused, the file as it was\n",
                j->file_id, (unsigned)rc);
        wjob_finalize(j, rc);
        return false;
    }
    memcpy(j->dma_virt, j->spare_image, TAGFS_BLOCK_SIZE);
    j->if_disk_block = j->spare_block;
    rc = wjob_submit(j, true, wjob_spare_complete);
    if (rc != OK) {
        PoolSpareDrop(j->spare_block, j->spare_image);
        j->spare_image = NULL;
        wjob_finalize(j, rc);
    }
    return false;  /* parked until IRQ */
}

/* W_SPARE_DONE — the stamp landed: the Spare is the pool's, and the job inks
 * again. */
static bool w_spare_done(WriteJob *j)
{
    MetaPoolBlock *image = j->spare_image;
    j->spare_image = NULL;
    if (j->if_status != OK) {
        kprintf("[Storage] write to file %u: block %u would not take the stamp of a Spare "
                "for the pool (error %u) — the write is refused, the file as it was\n",
                j->file_id, j->spare_block, (unsigned)j->if_status);
        PoolSpareDrop(j->spare_block, image);
        wjob_finalize(j, j->if_status);
        return false;
    }
    error_t rc = PoolSpareAdopt(j->spare_block, image, VolumeFlushNeed());
    if (rc != OK) {
        wjob_finalize(j, rc);
        return false;
    }
    wjob_to(j, W_INK);
    return true;
}

/* =========================================================================
 *  The end — one for every job, however it ran
 * ========================================================================= */

/* A change to a file's bytes, said on every tag it wears — one 32-byte shape
 * for every road that changes them, so one listener reads them all: op 1 a
 * write (the bytes written at offset), op 3 a span (the bytes cut or added
 * from offset, the old or the new size, whichever is less). A defrag moves the
 * bytes and changes none of them, and says nothing. */
static void wjob_publish(const WriteJob *j, error_t rc)
{
    uint8_t  op;
    uint64_t offset, bytes, final_size;
    switch (j->kind) {
    case WRITE_JOB_WRITE:
        if (j->bytes_inked == 0)
            return;
        op = 1, offset = j->start_offset, bytes = j->bytes_inked, final_size = j->final_size;
        break;
    case WRITE_JOB_SPAN:
        if (rc != OK || j->span_size == j->span_was)
            return;
        op         = 3;
        offset     = j->span_size < j->span_was ? j->span_size : j->span_was;
        bytes      = j->span_size < j->span_was ? j->span_was - j->span_size
                                                : j->span_size - j->span_was;
        final_size = j->span_size;
        break;
    default:
        return;
    }
    if (!TouchHasAnyListeners())
        return;

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (tagfs_get_metadata(j->file_id, &meta) != OK)
        return;
    struct {
        uint32_t file_id;
        uint8_t  op;
        uint8_t  _pad[3];
        uint64_t offset;
        uint64_t bytes;
        uint64_t final_size;
    } __attribute__((packed)) ev = {
        .file_id    = j->file_id,
        .op         = op,
        .offset     = offset,
        .bytes      = bytes,
        .final_size = final_size,
    };
    uint32_t pid = j->target ? j->target->pid : 0;
    for (uint32_t ti = 0; ti < meta.tag_count; ti++)
        TouchPublishId(meta.tag_ids[ti], &ev, sizeof(ev), pid, TOUCH_FLAG_TAGFS);
    tagfs_metadata_free(&meta);
}

/* What the job's out crate carries: a write's [u64 written][u64 size] — the
 * bytes an inked record names, all of them, a prefix a refusal stopped, or
 * none: answering "0 bytes" to an appending writer whose bytes are in the file
 * is exactly what would make it write the same payload twice — and a defrag's
 * [u32 score] once it moved the file. The length, 0 for nothing. */
static uint32_t wjob_out_bytes(const WriteJob *j, error_t rc, uint64_t written,
                               uint8_t out[16])
{
    if (!j->out_crate)
        return 0;
    if (j->kind == WRITE_JOB_WRITE && j->out_crate->capacity >= 16) {
        memcpy(out + 0, &written,       sizeof(uint64_t));
        memcpy(out + 8, &j->final_size, sizeof(uint64_t));
        return 16;
    }
    if (j->kind == WRITE_JOB_DEFRAG && rc == OK && j->out_crate->capacity >= sizeof(uint32_t)) {
        uint32_t score = tagfs_get_fragmentation_score(NULL, NULL);
        memcpy(out, &score, sizeof(score));
        return sizeof(score);
    }
    return 0;
}

/* The answer pushed to the parked caller. The out crate reaches the program's
 * pages first, then the descriptors — the dispatcher handed the staged kbuf
 * to the job when it parked — and only then the Result: a ferry that frees its
 * submission block on completion cannot race it. j->target is pinned for the
 * job's lifetime and commit_out walks the page tables physically (no target
 * CR3 switch). */
static void wjob_answer_pushed(WriteJob *j, error_t rc, uint64_t written)
{
    uint8_t  out[16];
    uint32_t n    = wjob_out_bytes(j, rc, written, out);
    vmm_context_t *vmm = (j->target && j->target->cabin) ? j->target->cabin->vmm : NULL;
    if (n && vmm && vmm_user_buf_commit_out(vmm, (uintptr_t)j->out_crate->addr, out, n) == OK)
        j->out_crate->size = n;
    if (j->crates_kbuf)
        crate_stage_commit_and_release(j->crates_kbuf, j->crate_count, vmm, j->crates_uaddr);

    Result r;
    memset(&r, 0, sizeof(r));
    /* The error the job actually ended on, never a blanket ERR_IO: the
     * caller tells ERR_TAGFS_BEYOND_SIZE from ERR_TAGFS_RECORD_FULL from the
     * medium's own answer by it. */
    r.error_code  = (uint32_t)rc;
    r.data_length = (uint32_t)written;
    r.sender_pid  = 0;
    /* Ф26e: a waybilled (box::ferry) write echoes its correlation token in
     * data_addr and flies the KCTX_STORAGE flag so boxlib routes it to the
     * ferry station; a plain answer keeps KCTX_GUIDE / data_addr==0. */
    if (j->waybill) { r.context = KCTX_STORAGE; r.data_addr = j->waybill; }
    else            { r.context = KCTX_PACK24(KCTX_GUIDE, j->submit_cookie); }

    if (j->target) {
        /* The answer is determined; only its delivery remains, and that is
         * the kernel's own debt. Due here, kept by the push. */
        ChitDue(j->target, j->submit_cookie);
        KResultPush(j->target, &r);
    }
}

/* The answer of a job that ran inside its caller's call: the out crate
 * through the call's own crates, and the handler's return. A ferry is
 * answered on its own channel even so. Best-effort: an out crate the program
 * unmapped meanwhile is left empty, and the work stands. */
static int wjob_answer_now(WriteJob *j, error_t rc, uint64_t written)
{
    uint8_t  out[16];
    uint32_t n = wjob_out_bytes(j, rc, written, out);
    if (n)
        (void)crate_write(j->out_crate, j->ctx, out, n);
    if (j->waybill)
        return storage_sync_ferry_finalize(j->ctx, j->crates_kbuf, rc, written, j->waybill);
    return (int)rc;
}

/* A refusal said once, on the job's way out: a write's with the one answer
 * every write gives (W-ERR), and a job the medium left before it was handed
 * the Helm with what left with the medium. */
static void wjob_say_refused(const WriteJob *j, error_t rc)
{
    static const char *const what[] = {
        [WRITE_JOB_WRITE]  = "write",
        [WRITE_JOB_SPAN]   = "span",
        [WRITE_JOB_DEFRAG] = "defrag",
    };
    if (rc == OK)
        return;
    if (j->medium_left)
        kprintf("[Storage] file %u: the medium left while this %s waited for the file's Helm — "
                "nothing of it was done, and %llu change(s) inked since the last settled cycle "
                "left with the medium\n", j->file_id, what[j->kind],
                (unsigned long long)j->lost);
    else if (j->kind == WRITE_JOB_WRITE)
        kprintf("[Storage] write to file %u refused (error %u): %llu of its %llu byte(s) are the "
                "file's, and the file is %llu bytes\n", j->file_id, (unsigned)rc,
                (unsigned long long)j->bytes_inked, (unsigned long long)j->source.bytes,
                (unsigned long long)j->final_size);
}

/* What the job took goes back first — the Stakes nobody inked, on every way
 * out, once — then the listeners hear of what changed, the Helm goes to the
 * next waiter, and the answer is given; the door is let go of last of what
 * the volume lent. The handler's return when the job ran inside its call. */
static int wjob_finalize(WriteJob *j, error_t rc)
{
    uint64_t written = j->bytes_inked;
    if (written == 0)
        j->final_size = j->loaded ? j->plot.size_before : j->handle->file_size;

    PlotRelease(&j->plot);
    wjob_say_refused(j, rc);
    wjob_publish(j, rc);
    if (j->holds_helm)
        HelmLetGo(j->handle->ofe, &j->helm);

    if (j->dma_phys)
        pmm_free(j->dma_phys, 1);
    tagfs_close(j->handle);
    tagfs_leave();

    int answer = OK;
    if (j->deferred)
        wjob_answer_pushed(j, rc, written);
    else
        answer = wjob_answer_now(j, rc, written);

    if (j->target)
        process_ref_dec(j->target);
    kfree(j);
    return answer;
}

/* =========================================================================
 *  Pump
 * ========================================================================= */

/* The job holds the volume's door from its making to its end, so the volume
 * its Plot's block numbers belong to is the one mounted for as long as it
 * pumps: a take-down waits for the job, and no second mount begins beneath it
 * on another K-Core (W-DOOR). */
static void wjob_pump(void *job_)
{
    WriteJob *j = (WriteJob *)job_;

    for (;;) {
        WriteJobState s = (WriteJobState)atomic_load_u32((volatile uint32_t *)&j->state);
        switch (s) {
            case W_ALLOC:         if (!w_alloc(j))         return; break;
            case W_LOCATE:        if (!w_locate(j))        return; break;
            case W_READ_OLD:      if (!w_read_old(j))      return; break;
            case W_DMA_FILL:      if (!w_dma_fill(j))      return; break;
            case W_AHCI_SUBMIT:   if (!w_ahci_submit(j))   return; break;
            case W_AHCI_DONE:     if (!w_ahci_done(j))     return; break;
            case W_LOG_META:      if (!w_log_meta(j))      return; break;
            case W_INK:           if (!w_ink(j))           return; break;
            case W_SPARE:         if (!w_spare(j))         return; break;
            case W_SPARE_DONE:    if (!w_spare_done(j))    return; break;
            case W_DONE:          wjob_finalize(j, j->stopped); return;
            case W_INIT:
            case W_HELM_WAIT:
                return;
        }
    }
}

/* IRQ callback for W_READ_OLD. Drops state directly to W_DMA_FILL —
 * the read populated DMA with the block's content, ready for the
 * user-bytes overlay; a refused read goes to W_AHCI_DONE, which stops the
 * write before the block is written.
 *
 * Runs in AHCI completion IRQ context. It only stashes status and posts
 * the job's embedded completion node (BatonPass) to the drain
 * core — no allocation, no lock, and NEVER dropped: the node lives inside
 * the WriteJob, so there is no slot to run out of. The heavy half (the
 * overlay / finalize / next block) runs later in the K-Core pump. */
static void wjob_read_old_complete(uint8_t port, uint8_t slot,
                                   error_t status, void *ctx)
{
    (void)port; (void)slot;
    WriteJob *j = (WriteJob *)ctx;
    if (status != OK) {
        j->if_status = status;
        wjob_to(j, W_AHCI_DONE);
    } else {
        wjob_to(j, W_DMA_FILL);
    }
    BatonPass(&j->cq_node);   /* never-drop; heavy half on the K-Core pump */
}

/* IRQ callback for W_SPARE, as lean as the others. */
static void wjob_spare_complete(uint8_t port, uint8_t slot, error_t status, void *ctx)
{
    (void)port; (void)slot;
    WriteJob *j = (WriteJob *)ctx;
    j->if_status = status;
    wjob_to(j, W_SPARE_DONE);
    BatonPass(&j->cq_node);
}

/* IRQ callback. Lean — only stash status + defer continuation. See
 * wjob_read_old_complete above for the IRQ-safety rationale. */
static void wjob_ahci_complete(uint8_t port, uint8_t slot,
                                error_t status, void *ctx)
{
    (void)port; (void)slot;
    WriteJob *j = (WriteJob *)ctx;
    j->if_status = status;
    wjob_to(j, W_AHCI_DONE);
    BatonPass(&j->cq_node);   /* never-drop; heavy half on the K-Core pump */
}

/* =========================================================================
 *  Public entry — WriteJobSubmit
 * ========================================================================= */

/* A write goes through the port, the job parked on each block's answer, only
 * where the volume is on AHCI and a K-Core pumps its continuations: on a
 * single core the BSP runs the program and never pumps, so the job would
 * strand, and the other seats' machines are not built on AHCI's slots. A job
 * that cannot have its DMA page is synchronous. */
static void wjob_choose_async(WriteJob *j)
{
    if (j->kind != WRITE_JOB_WRITE || !j->target || g_amp.total_cores <= 1 ||
        BoardroomSeatKind(tagfs_get_seat()) != BOARD_AHCI)
        return;
    j->dma_phys = pmm_alloc(1, PHYS_TAG_DMA32);
    if (!j->dma_phys)
        return;
    j->dma_virt = vmm_phys_to_virt((uintptr_t)j->dma_phys);
    j->async    = true;
}

/* The job for `ask`, its file open for writing and its caller pinned: OK, or
 * the refusal with nothing held. Inside the volume's door. */
static error_t wjob_make(const WriteJobAsk *ask, Crate *crates, const OpContext *ctx,
                         WriteJob **out)
{
    uint32_t flags = TAGFS_HANDLE_WRITE |
                     (ask->kind == WRITE_JOB_WRITE && ask->append ? TAGFS_HANDLE_APPEND : 0);
    WriteJob *j = kmalloc(sizeof(WriteJob));
    if (!j)
        return ERR_NO_MEMORY;
    memset(j, 0, sizeof(*j));

    j->handle = tagfs_open(ask->file_id, flags);
    if (!j->handle) {
        kfree(j);
        return ERR_FILE_NOT_FOUND;
    }
    if (ctx && ctx->proc) {
        j->target = process_find_ref(ctx->proc->pid);
        if (!j->target) {
            tagfs_close(j->handle);
            kfree(j);
            return ERR_INVALID_ARGUMENT;
        }
    }

    j->kind          = ask->kind;
    j->ctx           = ctx;
    j->file_id       = ask->file_id;
    j->out_crate     = ask->out_crate;
    j->source        = ask->source;
    j->asked_offset  = ask->offset;
    j->append        = ask->append;
    j->waybill       = ask->waybill;
    j->span_size     = ask->size;
    j->defrag_target = ask->target_block;
    j->submit_cookie = ctx ? ctx->submit_cookie : 0;
    j->final_size    = j->handle->file_size;
    j->helm.hand     = wjob_handed;
    j->helm.run      = wjob_run_ready;
    j->helm.gone     = wjob_gone;
    j->cq_node.run   = wjob_pump;
    j->cq_node.ctx   = j;
    j->crates_kbuf   = crates;
    j->crate_count   = ctx ? ctx->crate_count : 0;
    j->crates_uaddr  = ctx ? ctx->crates_uaddr : 0;
    wjob_choose_async(j);
    *out = j;
    return OK;
}

/* The caller is parked and the answer becomes the job's to push — the chit,
 * and with it the staged crates, before the job can be handed the Helm: a job
 * handed it runs at once, and can finish on another core before this call
 * returns. */
static void wjob_defer(WriteJob *j, const OpContext *ctx)
{
    static const char *const holder[] = {
        [WRITE_JOB_WRITE]  = "storage.write",
        [WRITE_JOB_SPAN]   = "storage.span",
        [WRITE_JOB_DEFRAG] = "system.fs.defrag",
    };
    j->deferred = true;
    j->ctx      = NULL;
    process_set_state(ctx->proc, PROC_WAITING);
    ChitGive(ctx, holder[j->kind], j->file_id);
}

int WriteJobSubmit(const WriteJobAsk *ask, Crate *crates, const OpContext *ctx)
{
    if (!ask || (ask->kind == WRITE_JOB_WRITE && ask->source.bytes == 0))
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter()) {
        return (ask->waybill && ctx)
                   ? storage_sync_ferry_finalize(ctx, crates, ERR_TAGFS_NOT_INITIALIZED, 0,
                                                 ask->waybill)
                   : (int)ERR_TAGFS_NOT_INITIALIZED;
    }

    WriteJob *j  = NULL;
    error_t   rc = wjob_make(ask, crates, ctx, &j);
    if (rc != OK) {
        tagfs_leave();
        return (ask->waybill && ctx) ? storage_sync_ferry_finalize(ctx, crates, rc, 0, ask->waybill)
                                     : (int)rc;
    }
    OpenFileEntry *ofe = j->handle->ofe;

    if (!j->async && HelmTry(ofe, &j->helm)) {
        j->holds_helm = true;
        return wjob_turn(j);
    }
    if (!j->target) {
        kprintf("[Storage] file %u is being changed by the holder of its Helm, and a caller "
                "with no program to park cannot wait behind it — refused\n", j->file_id);
        return wjob_finalize(j, ERR_BUSY);
    }

    wjob_defer(j, ctx);
    wjob_to(j, W_HELM_WAIT);
    if (!HelmAsk(ofe, &j->helm))
        return ERR_WOULD_BLOCK;   /* the job is the Helm's now, or answered: not touched again here */

    j->holds_helm = true;
    if (j->async) {
        /* The first pump in this call's own context: it takes every lock the
         * machine needs and skips a queue round-trip, and parks at the first
         * submit. What it posts from here goes to the drain core. */
        wjob_to(j, W_ALLOC);
        wjob_pump(j);
    } else {
        (void)wjob_turn(j);
    }
    return ERR_WOULD_BLOCK;
}
