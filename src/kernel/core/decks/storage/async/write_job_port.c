/*
 * The asynchronous write through the port (write_job.h): a WriteJob whose
 * every command goes to its seat (BoardroomReadAsync, BoardroomWriteAsync) —
 * an AHCI port's slots or a USB disk's queue — parked on each answer and
 * pumped between two where that seat's answers continue (wjob_continue). The
 * answer's side is intentionally minimal: stash status, set the next state,
 * post the continuation. The pump does everything that needs locks (the Plot,
 * the record's Ink, the Helm, Touch publish).
 *
 *   W_HELM_WAIT? → W_ALLOC → W_LOCATE
 *
 *   a stretch a command (PlotStretchAt, step 1.11): the part of the window
 *   that lies in one run on the medium, as many blocks as the job's DMA pages
 *   hold — the run the seat takes in one command:
 *     W_LOCATE → (its first or last block partly overlaid on the file's bytes:
 *                 W_READ_OLD → W_READ_DONE, once for each)
 *              → W_DMA_FILL → W_BLOCK_SUBMIT → (yield to the answer) → W_BLOCK_DONE
 *     W_BLOCK_DONE → if more bytes of the window: W_LOCATE
 *                  → else:                        W_LOG_META
 *
 *   the record, once a window:
 *     W_LOG_META → W_INK → (more windows: yield the core → W_ALLOC)
 *                        → (the last one:                W_DONE)
 *     W_INK with no Spare to close the pool's tail onto:
 *              → W_SPARE → (yield to the answer) → W_SPARE_DONE → W_INK
 *
 * The bytes come out of the program's pages a block at a time, into the DMA
 * pages they go out of — never copied whole into the kernel heap first. The
 * one read-modify-write rule (PlotFillFor) is only ever a stretch's first and
 * last block's. A stop inside a stretch — the read of its first or last block
 * refused or rotten, the program's pages — still lays the blocks before it, in
 * the one command they fill; a write the medium refuses counts the blocks the
 * medium says it kept before the sector it failed at (RoadsteadBerth.landed).
 */

#include "write_job_internal.h"
#include "boardroom.h"
#include "baton.h"
#include "tagfs.h"
#include "helm.h"        /* the file's Helm, and HelmReady for a USB disk's steps */
#include "ink.h"         /* TagFSInkPlot — the one way the file changes */
#include "meta_pool.h"   /* PoolSpareBegin / Adopt / Drop — a Spare stamped through the port */
#include "scribe.h"      /* VolumeFlushNeed, VolumeWriteSeen — what the landed blocks wait for */
#include "integrity.h"
#include "run.h"         /* TagFSDataCostLaid — what a file's bytes cost the medium */
#include "settle.h"      /* SettleV1OnBook — no Spare is stamped for a book */
#include "cpu_calibrate.h"  /* a stretch's time on the medium, for the cadence */
#include "klib.h"

static void wjob_read_old_complete(uint8_t port, uint8_t slot, error_t status, void *ctx);
static void wjob_block_complete(uint8_t port, uint8_t slot, error_t status, void *ctx);
static void wjob_spare_complete(uint8_t port, uint8_t slot, error_t status, void *ctx);

void wjob_continue(WriteJob *j)
{
    if (j->resumes_at_service)
        HelmReady(&j->helm);
    else
        BatonPass(&j->cq_node);
}

/* Page i of the job's DMA pages. */
static uint8_t *wjob_page(const WriteJob *j, uint32_t i)
{
    return (uint8_t *)j->dma_virt + (size_t)i * TAGFS_BLOCK_SIZE;
}

/* The write stops at a refusal. What landed of the window before it is made
 * the file's — one record, inked the way a whole window's is, the ground
 * reserved past it given back — and the refusal is the answer, with the bytes
 * the file now holds of the write. A stop the medium's leaving explains inks
 * nothing (RT-13): what landed may be in a cache that went with it. A port
 * that refused before the room emptied the seat is not seen as one — its
 * prefix is inked, and a flush that never covered it gives the volume's open
 * book up (mount/away.h). Answers whether the pump goes on. */
static bool wjob_stop(WriteJob *j, error_t cause)
{
    if (cause == ERR_DEVICE_NOT_READY || TagFSMediumGone()) {
        wjob_finalize(j, ERR_DEVICE_NOT_READY);
        return false;
    }
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
    error_t rc = PlotLoad(&j->plot, j->file_id, j->handle->warrant);
    j->loaded = rc == OK;
    if (rc == OK) {
        j->plot.helm    = &j->handle->ofe->helm;
        j->start_offset = j->append ? j->plot.size : j->asked_offset;
    }
    return rc;
}

/* The volume is short of ground a Settle cycle gives back, and a continuation
 * cannot wait for the cycle: the job turns synchronous, and the service point
 * that takes its turn waits (write_job.c) — the window it was in written again
 * from the last one inked, the ground it reserved given back meanwhile, what
 * landed of it with that ground. */
static void wjob_to_service(WriteJob *j)
{
    PlotRelease(&j->plot);
    j->loaded     = false;
    j->bytes_done = j->bytes_inked;
    j->stopped    = OK;
    j->async      = false;
    HelmReady(&j->helm);
}

/* W_ALLOC — the window's blocks reserved and the record's room claimed before
 * any byte of it moves; every window after the first goes on from where the
 * last one's Ink left the Plot. Room on its way turns the job synchronous
 * (wjob_to_service). */
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
        wjob_to_service(j);
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

/* W_LOCATE — the stretch the cursor is in: its first and last block zeroed
 * where they held nothing of the file, and marked to be read where they hold
 * the file's bytes around the write. A file given up meanwhile is written no
 * further. */
static bool w_locate(WriteJob *j)
{
    if (HelmVacated(j->handle->ofe)) {
        wjob_finalize(j, ERR_FILE_NOT_FOUND);
        return false;
    }

    uint64_t pos = j->start_offset + j->bytes_done;
    if (!PlotStretchAt(&j->plot, pos, j->window_end - j->bytes_done, j->dma_blocks, &j->st)) {
        kprintf("[Storage] write to file %llu: no extent of its plot holds block %llu, which the "
                "write reserved — the write stops at byte %llu\n", j->file_id,
                (unsigned long long)(pos / TAGFS_BLOCK_SIZE), (unsigned long long)pos);
        return wjob_stop(j, ERR_TAGFS_EXTENT_ERROR);
    }

    uint32_t last = j->st.count - 1;
    j->st_staged  = j->st.count;
    j->st_stop    = OK;
    j->st_reads   = 0;
    if (j->st.first == PLOT_FILL_READ)
        j->st_reads |= WJOB_READ_FIRST;
    else if (j->st.first == PLOT_FILL_FRESH)
        memset(wjob_page(j, 0), 0, TAGFS_BLOCK_SIZE);
    if (last > 0 && j->st.last == PLOT_FILL_READ)
        j->st_reads |= WJOB_READ_LAST;
    else if (last > 0 && j->st.last == PLOT_FILL_FRESH)
        memset(wjob_page(j, last), 0, TAGFS_BLOCK_SIZE);
    wjob_to(j, j->st_reads ? W_READ_OLD : W_DMA_FILL);
    return true;
}

/* `blocks` blocks from `block` on to or from the job's DMA pages from `page`
 * on, as one command to the port. "Every place on the medium is taken" —
 * every command slot of an AHCI port, the one turn of a USB disk — is not "the
 * disk failed", and it is not "come back later" either: the job's berth
 * carries the request, and the seat issues it now or moors it in its roadstead
 * — under the same lock that found no place, so a place freed a moment later
 * calls it in — until a place comes free or the door a cache flush shut opens
 * again. Either way `answered` runs, exactly once, perhaps before this
 * returns; an error here means the seat is not there, or the request is
 * malformed — or the volume's medium is away, and its seat is not the
 * volume's to be written through. */
static error_t wjob_submit(WriteJob *j, bool write, uint64_t block, uint32_t blocks,
                           uint32_t page, RoadsteadAnswered answered)
{
    if (TagFSMediumGone())
        return ERR_DEVICE_NOT_READY;
    if (write && TagFSIsUnderGlass())
        return TagFSGlassRefused("the port's write");

    uint8_t  seat    = tagfs_get_seat();
    uint64_t lba     = tagfs_block_to_sector(block);
    void    *dma     = (uint8_t *)j->dma_phys + (size_t)page * TAGFS_BLOCK_SIZE;
    uint32_t sectors = blocks * TAGFS_BLOCK_SECTORS;
    return write ? BoardroomWriteAsync(seat, lba, sectors, dma, answered, j, &j->berth)
                 : BoardroomReadAsync(seat, lba, sectors, dma, answered, j, &j->berth);
}

/* The block the rest of the stretch's first or last block is read from — the
 * block itself on format v1, the old block it lay on on fresh ground (format
 * v2, plot_window.c). */
static uint64_t WriteJobReadFrom(const WriteJob *j)
{
    return j->st_reading == 0 ? j->st.from_first : j->st.from_last;
}

/* W_READ_OLD — the stretch's first block, then its last, read into its own
 * page, so the write's bytes fold over what the block holds. Its answer moves
 * the state to W_READ_DONE (wjob_read_old_complete). */
static bool w_read_old(WriteJob *j)
{
    j->st_reading = (j->st_reads & WJOB_READ_FIRST) ? 0 : j->st.count - 1;
    error_t err = wjob_submit(j, false, WriteJobReadFrom(j), 1, j->st_reading, wjob_read_old_complete);
    if (err != OK)
        return wjob_stop(j, err);
    return false;  /* parked until the answer */
}

/* W_READ_DONE — the read answered. A block the medium would not give, or that
 * does not hold what was written to it, stops the stretch there: bytes laid
 * over the rest of it would be zeros nobody asked for (S1), or a rot written
 * back under a fresh digest and the file's for good (RT-22). The blocks before
 * it are still laid. A write that covers a block whole reads nothing and is
 * never refused this way. */
static bool w_read_done(WriteJob *j)
{
    uint32_t i     = j->st_reading;
    uint64_t block = WriteJobReadFrom(j);
    error_t  why   = j->if_status;
    if (why == OK && !IntegrityVerify(block, wjob_page(j, i)))
        why = ERR_CHECKSUM;

    j->st_reads &= (uint8_t)~(i == 0 ? WJOB_READ_FIRST : WJOB_READ_LAST);
    if (why != OK) {
        kprintf("[Storage] write to file %llu: block %llu %s (error %u), so the byte(s) meant for "
                "it are not laid over the rest of it — the write stops there\n", j->file_id,
                (unsigned long long)block,
                why == ERR_CHECKSUM ? "does not hold what was written to it" : "would not read",
                (unsigned)why);
        j->st_staged = i;
        j->st_stop   = why;
        j->st_reads  = 0;
    }
    wjob_to(j, j->st_reads ? W_READ_OLD : W_DMA_FILL);
    return true;
}

/* W_DMA_FILL — the write's bytes for each block staged, out of the program's
 * pages, laid over what its page holds. A page the program gave up since
 * stops the stretch at that block. */
static bool w_dma_fill(WriteJob *j)
{
    for (uint32_t i = 0; i < j->st_staged; i++) {
        uint64_t at    = PlotStretchBytes(&j->st, i);
        uint32_t chunk = (uint32_t)(PlotStretchBytes(&j->st, i + 1) - at);
        uint32_t off   = i == 0 ? j->st.head : 0;
        error_t  rc    = PlotSourcePull(&j->source, j->bytes_done + at, wjob_page(j, i) + off,
                                        chunk);
        if (rc != OK) {
            kprintf("[Storage] write to file %llu: the bytes from %llu on could not be taken from "
                    "the pages they are in (error %u) — the write stops there\n", j->file_id,
                    (unsigned long long)(j->bytes_done + at), (unsigned)rc);
            j->st_staged = i;
            j->st_stop   = rc;
            break;
        }
    }
    if (j->st_staged == 0)
        return wjob_stop(j, j->st_stop);
    wjob_to(j, W_BLOCK_SUBMIT);
    return true;
}

/* W_BLOCK_SUBMIT — the staged blocks' write to the port as one command, and a
 * yield to its answer. The answer can come on another core before this
 * returns: nothing of the job is read after the submit. */
static bool w_block_submit(WriteJob *j)
{
    uint32_t staged = j->st_staged;
    j->if_submit_tsc = rdtsc();
    error_t err = wjob_submit(j, true, j->st.block, staged, 0, wjob_block_complete);
    if (err != OK)
        return wjob_stop(j, err);
    TagFSDataCostLaid(staged);
    return false;  /* parked until the answer */
}

/* W_BLOCK_DONE — the port answered the stretch's write. The blocks that landed
 * — all of it, or on a refusal what the medium says it kept — are the
 * write's: no read-ahead window hands out what they held, and each has its
 * digest recorded, since the port wrote past the block door. A stop inside
 * the stretch ends the write after them. */
static bool w_block_done(WriteJob *j)
{
    uint32_t staged = j->st_staged;
    uint32_t landed = j->if_status == OK ? staged : j->berth.landed / TAGFS_BLOCK_SECTORS;
    if (landed > staged)
        landed = staged;
    for (uint32_t i = 0; i < landed; i++) {
        tagfs_readahead_invalidate(j->st.block + i);
        IntegrityUpdate(j->st.block + i, wjob_page(j, i));
        if (PlotWindowLanded(&j->plot, j->st.block + i, wjob_page(j, i), 1) != OK) {
            landed = i;      /* a block whose digest is not kept is no block of the file's */
            if (j->if_status == OK)
                j->if_status = ERR_NO_MEMORY;
            break;
        }
    }
    if (landed)
        VolumeWriteSeen();
    j->plot.data_us += cpu_tsc_to_us(rdtsc() - j->if_submit_tsc);
    j->bytes_done   += PlotStretchBytes(&j->st, landed);

    if (j->if_status != OK) {
        kprintf("[Storage] write to file %llu: blocks %llu..%llu would not be written (error %u) — "
                "%u of them had landed before the medium stopped answering, %llu of %llu "
                "bytes\n", j->file_id, (unsigned long long)j->st.block,
                (unsigned long long)(j->st.block + staged - 1), (unsigned)j->if_status, landed,
                (unsigned long long)j->bytes_done, (unsigned long long)j->source.bytes);
        return wjob_stop(j, j->if_status);
    }
    if (j->st_stop != OK)
        return wjob_stop(j, j->st_stop);
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
 * left, the job posts itself behind what its seat's continuations have waiting
 * before it reserves the next one. */
static bool w_ink(WriteJob *j)
{
    InkReceipt inked;
    error_t    rc = TagFSInkPlot(&j->plot, INK_ASKS_SPARES, &inked);
    if (rc == ERR_WOULD_BLOCK && j->plot.book) {
        wjob_to_service(j);      /* a book has no Spare: the room on its way is the cycle's */
        return false;
    }
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
    wjob_continue(j);   /* the next window, after what its core has waiting */
    return false;
}

/* W_SPARE — a block taken for the pool and its empty stamp written through
 * the port, from the job's first DMA page like any block of the write. Room
 * on its way turns the job synchronous (wjob_to_service); a volume with no
 * block for it at all ends the job with the allocator's answer: nothing is
 * inked, and the ground the write took goes back. */
static bool w_spare(WriteJob *j)
{
    if (SettleV1OnBook("a Spare stamped for an asynchronous write")) {
        wjob_finalize(j, ERR_TAGFS_READ_ONLY);
        return false;
    }
    error_t rc = PoolSpareBegin(&j->spare_block, &j->spare_image);
    if (rc == ERR_WOULD_BLOCK) {
        wjob_to_service(j);
        return false;
    }
    if (rc != OK) {
        kprintf("[Storage] write to file %llu: the pool has no Spare to take its record and "
                "none could be made (error %u) — the write is refused, the file as it was\n",
                j->file_id, (unsigned)rc);
        wjob_finalize(j, rc);
        return false;
    }
    memcpy(wjob_page(j, 0), j->spare_image, TAGFS_BLOCK_SIZE);
    rc = wjob_submit(j, true, j->spare_block, 1, 0, wjob_spare_complete);
    if (rc != OK) {
        PoolSpareDrop(j->spare_block, j->spare_image);
        j->spare_image = NULL;
        wjob_finalize(j, rc);
    }
    return false;  /* parked until the answer */
}

/* W_SPARE_DONE — the stamp landed: the Spare is the pool's, and the job inks
 * again. */
static bool w_spare_done(WriteJob *j)
{
    MetaPoolBlock *image = j->spare_image;
    j->spare_image = NULL;
    if (j->if_status != OK) {
        kprintf("[Storage] write to file %llu: block %u would not take the stamp of a Spare "
                "for the pool (error %u) — the write is refused, the file as it was\n",
                j->file_id, j->spare_block, (unsigned)j->if_status);
        PoolSpareDrop(j->spare_block, image);
        wjob_finalize(j, j->if_status);
        return false;
    }
    VolumeWriteSeen();
    error_t rc = PoolSpareAdopt(j->spare_block, image, VolumeFlushNeed());
    if (rc != OK) {
        wjob_finalize(j, rc);
        return false;
    }
    wjob_to(j, W_INK);
    return true;
}

/* The job holds the volume's door from its making to its end, so the volume
 * its Plot's block numbers belong to is the one mounted for as long as it
 * pumps: a take-down waits for the job, and no second mount begins beneath it
 * on another K-Core (W-DOOR). */
void wjob_pump(void *job_)
{
    WriteJob *j = (WriteJob *)job_;

    for (;;) {
        WriteJobState s = (WriteJobState)atomic_load_u32((volatile uint32_t *)&j->state);
        switch (s) {
            case W_ALLOC:         if (!w_alloc(j))         return; break;
            case W_LOCATE:        if (!w_locate(j))        return; break;
            case W_READ_OLD:      if (!w_read_old(j))      return; break;
            case W_READ_DONE:     if (!w_read_done(j))     return; break;
            case W_DMA_FILL:      if (!w_dma_fill(j))      return; break;
            case W_BLOCK_SUBMIT:  if (!w_block_submit(j))  return; break;
            case W_BLOCK_DONE:    if (!w_block_done(j))    return; break;
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

/* The answers. Each runs where the seat answers — an AHCI port's completion
 * interrupt, a USB disk's command step on the drain core — and only stashes
 * the status and posts the job's next step (wjob_continue): no allocation, no
 * medium, and NEVER dropped, since the node it posts lives inside the
 * WriteJob. The heavy half runs later, in the pump. */
static void wjob_answered(WriteJob *j, error_t status, WriteJobState next)
{
    j->if_status = status;
    wjob_to(j, next);
    wjob_continue(j);
}

static void wjob_read_old_complete(uint8_t port, uint8_t slot, error_t status, void *ctx)
{
    (void)port; (void)slot;
    wjob_answered((WriteJob *)ctx, status, W_READ_DONE);
}

static void wjob_block_complete(uint8_t port, uint8_t slot, error_t status, void *ctx)
{
    (void)port; (void)slot;
    wjob_answered((WriteJob *)ctx, status, W_BLOCK_DONE);
}

static void wjob_spare_complete(uint8_t port, uint8_t slot, error_t status, void *ctx)
{
    (void)port; (void)slot;
    wjob_answered((WriteJob *)ctx, status, W_SPARE_DONE);
}

#ifdef CONFIG_VOLUME_TESTS
/* The stand's knock answered: there is no job behind it to go on. */
static void wjob_stand_door_answered(uint8_t index, uint8_t slot, error_t status, void *ctx)
{
    (void)index;
    (void)slot;
    (void)status;
    (void)ctx;
}

error_t WriteJobStandWriteDoor(uint64_t block)
{
    WriteJob *j = kmalloc(sizeof(WriteJob));
    if (!j)
        return ERR_NO_MEMORY;
    memset(j, 0, sizeof(*j));
    error_t rc = wjob_submit(j, true, block, 1, 0, wjob_stand_door_answered);
    kfree(j);
    return rc;
}
#endif
