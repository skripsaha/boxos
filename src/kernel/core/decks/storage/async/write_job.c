/*
 * WriteJob — async ObjWrite state-machine implementation.
 *
 * Pumped from K-Core continuations posted by the AHCI IRQ. The IRQ side
 * is intentionally minimal: stash status, set the next state, post the
 * continuation. The K-Core side does everything that needs locks
 * (the Plot, the record's Ink, token release, Touch publish).
 *
 * State flow (full):
 *
 *   W_INIT → W_TOKEN_WAIT? → W_ALLOC → W_LOCATE
 *
 *   per block, by the one read-modify-write rule (PlotFillFor):
 *     W_LOCATE → (the file's bytes, partly overlaid → W_READ_OLD → W_DMA_FILL)
 *              → (a block that held nothing of the file → W_DMA_FILL)  [zeros]
 *              → (covered whole                         → W_DMA_FILL)
 *     W_DMA_FILL → W_AHCI_SUBMIT → (yield to IRQ) → W_AHCI_DONE
 *     W_AHCI_DONE → if more bytes: W_LOCATE
 *                 → else:           W_LOG_META
 *
 *   the record (once):
 *     W_LOG_META → W_INK → W_PUBLISH → W_RELEASE_TOKEN → W_DONE
 *     W_INK with no Spare to close the pool's tail onto:
 *              → W_SPARE → (yield to IRQ) → W_SPARE_DONE → W_INK
 *
 * The job is a write like the synchronous one, on the same Plot (plot.h): the
 * file's record loaded under the job's token, every block the write needs
 * reserved and the record's room checked before a byte moves, and what landed
 * inked as one record (TagFSInkPlot). A refusal on the way — a read of an
 * overlaid block, a block's write, the program's pages — stops the job there:
 * what landed before it is inked and said, and the ground reserved past it
 * goes back. A job that inks nothing gives back everything it took (CR-1), and
 * never a block twice (W-DFREE): what it took is the Plot's Stakes, given back
 * once.
 *
 * ‼ NO FLUSH OF ITS OWN. What the job inked reaches the medium with a Settle
 * cycle, as a synchronous write's does — the cycle's first flush covers the
 * data before any record names it (settle.h). A flush after every write was a
 * SYNC CACHE per fwrite and proved nothing about the record, which no cycle
 * had written.
 *
 * ‼ A SPARE IS MADE THE JOB'S OWN WAY. A continuation may not stand over the
 * medium, so an Ink with no Spare to close the tail onto answers
 * ERR_WOULD_BLOCK (INK_ASKS_SPARES) and the job stamps one through the port
 * like any block of its own (W_SPARE), hands it to the pool and inks again.
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
#include "tagfs.h"
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
#include "op_registry.h"
#include "touch.h"
#include "integrity.h"
#include "ink.h"         /* TagFSInkPlot — the one way the file changes */
#include "meta_pool.h"   /* PoolSpareBegin / Adopt / Drop — a Spare stamped through the port */
#include "scribe.h"      /* VolumeFlushNeed — what the landed blocks wait for */

/* ---- forward decls ---- */
static void wjob_pump(void *job_);
static void wjob_finalize(WriteJob *j, int rc);
static void wjob_publish_change(WriteJob *j);
static void wjob_ahci_complete(uint8_t port, uint8_t slot, error_t status, void *ctx);

static inline void wjob_to(WriteJob *j, WriteJobState s)
{
    atomic_store_u32((volatile uint32_t *)&j->state, (uint32_t)s);
}

/* =========================================================================
 *  Token handoff
 * ========================================================================= */

static bool token_try_claim(WriteJob *j)
{
    OpenFileEntry *ofe = j->ofe;
    TagFSLockTake(&ofe->async_token_lock);
    if (ofe->async_write_owner == NULL) {
        ofe->async_write_owner = j;
        TagFSLockDrop(&ofe->async_token_lock);
        return true;
    }
    j->next_pending = NULL;
    if (!ofe->async_pending_head) {
        ofe->async_pending_head = j;
    } else {
        struct WriteJob *t = ofe->async_pending_head;
        while (t->next_pending) t = t->next_pending;
        t->next_pending = j;
    }
    TagFSLockDrop(&ofe->async_token_lock);
    return false;
}

static WriteJob *token_release_handoff(WriteJob *j)
{
    OpenFileEntry *ofe = j->ofe;
    WriteJob *next = NULL;
    TagFSLockTake(&ofe->async_token_lock);
    if (ofe->async_write_owner == j) {
        next = ofe->async_pending_head;
        if (next) {
            ofe->async_pending_head = next->next_pending;
            next->next_pending = NULL;
        }
        ofe->async_write_owner = next;
    }
    TagFSLockDrop(&ofe->async_token_lock);
    return next;
}

/* The token passed on: the next writer loads the file as this one left it. */
static void token_pass(WriteJob *j)
{
    WriteJob *next = token_release_handoff(j);
    if (next) {
        wjob_to(next, W_ALLOC);
        BatonPass(&next->cq_node);   /* never-drop; routes to drain core */
    }
}

/* =========================================================================
 *  State handlers
 * ========================================================================= */

/* The write stops at a refusal. What landed before it is made the file's —
 * one record, inked the way a whole write's is, the ground reserved past it
 * given back — and the refusal is the answer, with the bytes the file now
 * holds of the write. Answers whether the pump goes on. */
static bool wjob_stop(WriteJob *j, error_t cause)
{
    if (j->bytes_done > 0 && j->loaded) {
        j->stopped = cause;
        wjob_to(j, W_LOG_META);
        return true;
    }
    wjob_finalize(j, cause);
    return false;
}

/* W_ALLOC — the file's record loaded under the token, the growth rule asked,
 * and every block the write covers reserved, before any byte moves. */
static bool w_alloc(WriteJob *j)
{
    error_t rc = PlotLoad(&j->plot, j->file_id);
    j->loaded = rc == OK;
    if (rc == OK)
        rc = PlotBeginWrite(&j->plot, j->asked_offset, j->append, j->source.bytes,
                            &j->start_offset);
    if (rc != OK) {
        wjob_finalize(j, rc);
        return false;
    }
    wjob_to(j, W_LOCATE);
    return true;
}

/* W_LOCATE — the block the cursor is in, and what the rest of it needs: a
 * read of the file's bytes, zeros, or nothing. */
static bool w_locate(WriteJob *j)
{
    uint64_t pos   = j->start_offset + j->bytes_done;
    uint64_t k     = pos / TAGFS_BLOCK_SIZE;
    uint32_t off   = (uint32_t)(pos % TAGFS_BLOCK_SIZE);
    uint64_t rest  = j->source.bytes - j->bytes_done;
    uint32_t chunk = TAGFS_BLOCK_SIZE - off;
    if ((uint64_t)chunk > rest)
        chunk = (uint32_t)rest;

    /* The run the last block came from while it lasts, a search when it ends:
     * every chunk after the first begins a block. */
    if (j->bytes_done > 0 && j->if_left > 0) {
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
    wjob_to(j, j->bytes_done >= j->source.bytes ? W_LOG_META : W_LOCATE);
    return true;
}

/* W_LOG_META — what landed made ready to be the file's: the flush need its
 * blocks hold, read after their completions were seen, and the ground
 * reserved past them cut off. */
static bool w_log_meta(WriteJob *j)
{
    j->plot.data_need = VolumeFlushNeed();
    error_t rc = PlotEndWrite(&j->plot, j->start_offset, j->bytes_done);
    if (rc != OK) {
        wjob_finalize(j, rc);
        return false;
    }
    wjob_to(j, W_INK);
    return true;
}

/* W_INK — the one record the Plot builds, inked; with no Spare for the pool's
 * tail, one is stamped first (W_SPARE). */
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
    j->committed = true;
    wjob_to(j, W_PUBLISH);
    return true;
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

/* The file's size as the job leaves it: the inked record's once the job
 * loaded one, the open's before that. */
static uint64_t wjob_final_size(const WriteJob *j)
{
    return j->loaded ? j->plot.size_before : (j->handle ? j->handle->file_size : 0);
}

/* Say what happened to the file on every tag it carries. Separate from the
 * W_PUBLISH state because the change is a fact about the file, not about how
 * the job ended: a write stopped part way, or whose cache flush was refused,
 * has still changed the file for every reader, and the listeners have to hear
 * that from the same place a successful one tells them. */
static void wjob_publish_change(WriteJob *j)
{
    if (j->bytes_done > 0 && TouchHasAnyListeners()) {
        TagFSMetadata wmeta;
        memset(&wmeta, 0, sizeof(wmeta));
        if (tagfs_get_metadata(j->file_id, &wmeta) == OK) {
            struct {
                uint32_t file_id;
                uint8_t  op;          /* 1 = WRITE */
                uint8_t  _pad[3];
                uint64_t offset;
                uint64_t bytes;
                uint64_t final_size;
            } __attribute__((packed)) ev = {
                .file_id    = j->file_id,
                .op         = 1,
                .offset     = j->start_offset,
                .bytes      = j->bytes_done,
                .final_size = wjob_final_size(j),
            };
            for (uint32_t ti = 0; ti < wmeta.tag_count; ti++) {
                TouchPublishId(wmeta.tag_ids[ti], &ev, sizeof(ev),
                               (j->target ? j->target->pid : 0),
                               TOUCH_FLAG_TAGFS);
            }
            tagfs_metadata_free(&wmeta);
        }
    }
}

/* W_PUBLISH — the file's tags hear about the write, then the token moves on.
 * Best-effort: a publish failure does not abort the write. */
static bool w_publish(WriteJob *j)
{
    wjob_publish_change(j);
    wjob_to(j, W_RELEASE_TOKEN);
    return true;
}

/* W_RELEASE_TOKEN — handoff to next pending writer (if any), then DONE. */
static bool w_release_token(WriteJob *j)
{
    token_pass(j);
    wjob_to(j, W_DONE);
    return true;
}

/* W_DONE / W_ERROR — common exit. */
static void wjob_finalize(WriteJob *j, int rc)
{
    /* What the stats and the answer say is read off the Plot before it goes. */
    uint64_t bytes_written = j->committed ? j->bytes_done : 0;
    uint64_t final_size    = wjob_final_size(j);

    /* What the job took and no inked record names goes back first — on
     * every way out, once — and only then may the next writer load the file. */
    PlotRelease(&j->plot);
    if (rc != OK && j->ofe)
        token_pass(j);

    /* Stat reporter, on the sync ObjWrite contract: the size says 16 only of
     * stats that reached the program, as crate_write sets it only on success.
     * A commit the program's pages refuse leaves the size as it staged it, and
     * the write itself stands. out_crate points into the staged Crate[] kbuf;
     * out_crate->addr is the user vaddr. j->target is pinned for the job's
     * lifetime and commit_out walks the page tables physically (no target CR3
     * switch) — same model as the crate_stage_commit_and_release below.
     *
     * The count the caller is given is the count that is IN the file: the
     * bytes an inked record names — all of them, a prefix a refusal
     * stopped, or none. Answering "0 bytes" to an appending writer whose bytes
     * are in the file is exactly what would make it write the same payload
     * into the file twice. */
    if (j->out_crate && j->out_crate->capacity >= 16 &&
        j->target && j->target->cabin) {
        uint8_t stats[16];
        memcpy(stats + 0, &bytes_written, sizeof(uint64_t));
        memcpy(stats + 8, &final_size,    sizeof(uint64_t));
        if (vmm_user_buf_commit_out(j->target->cabin->vmm,
                                    (uintptr_t)j->out_crate->addr, stats, 16) == OK)
            j->out_crate->size = 16;
    }

    if (j->dma_phys)    pmm_free(j->dma_phys, 1);
    /* A Spare still in hand here was stamped for a mounting that is gone: its
     * block number is not this volume's to give back. */
    kfree(j->spare_image);
    if (j->handle)      tagfs_close(j->handle);

    /* CrateStage release. Dispatcher handed us ownership of crates_kbuf
     * at ObjWriteAsync's PROC_WAITING return. The stats' size above went
     * into kbuf only if the stats arrived; commit_and_release
     * flushes the whole descriptor array back to user memory and frees
     * the kbuf. Cabin is target->cabin (already pinned via ref). */
    if (j->crates_kbuf) {
        crate_stage_commit_and_release(j->crates_kbuf, j->crate_count,
                                        (j->target && j->target->cabin) ? j->target->cabin->vmm : NULL,
                                        j->crates_uaddr);
    }

    Result r;
    memset(&r, 0, sizeof(r));
    /* The error the job actually ended on, not a blanket ERR_IO. Every exit
     * above already carries a distinct one — ERR_TAGFS_BEYOND_SIZE (a write
     * past the size), ERR_TAGFS_RECORD_FULL (a record out of room),
     * ERR_DEVICE_NOT_READY (the port is not in the machine), ERR_NO_MEMORY
     * (this machine, not this disk), the status the AHCI completion reported,
     * the refusal of the cache flush — and flattening them here is what left
     * the caller unable to tell one from another. */
    r.error_code  = (uint32_t)rc;
    r.data_length = (uint32_t)bytes_written;
    r.sender_pid  = 0;
    /* Ф26e: a waybilled (box::ferry) write echoes its correlation token in
     * data_addr and flies the KCTX_STORAGE flag so boxlib routes it to the
     * ferry station; a plain write keeps KCTX_GUIDE / data_addr==0. */
    if (j->waybill) { r.context = KCTX_STORAGE; r.data_addr = j->waybill; }
    else            { r.context = KCTX_PACK24(KCTX_GUIDE, j->submit_cookie); }

    if (j->target) {
        /* The answer is determined; only its delivery remains, and that is
         * the kernel's own debt. Due here, kept by the push. */
        ChitDue(j->target, j->submit_cookie);
        KResultPush(j->target, &r);
        process_ref_dec(j->target);
    }
    kfree(j);
}

/* =========================================================================
 *  Pump
 * ========================================================================= */

static void wjob_pump(void *job_)
{
    WriteJob *j = (WriteJob *)job_;

    /*
     * ‼ IS THIS STILL THE VOLUME THIS WRITE WAS AIMED AT?
     *
     * A job lives across yields, and what it carries is a Plot of the file —
     * disk block numbers. If the medium left and came back in between, the
     * volume has been read again and those numbers now name whatever it keeps
     * there today. Carrying on would not fail: it would write this file's
     * bytes into another file's blocks.
     *
     * Asked once per pump rather than once per state, because a re-mount
     * cannot happen inside the loop below: it is finished by the same guide
     * loop that is running this.
     */
    if (!tagfs_handle_is_of_this_mount(j->handle)) {
        kprintf("[Storage] a write to file %u was aimed at an earlier mounting "
                "of this volume — it is refused rather than written somewhere "
                "else\n", j->file_id);
        wjob_finalize(j, ERR_IO);
        return;
    }

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
            case W_PUBLISH:       if (!w_publish(j))       return; break;
            case W_RELEASE_TOKEN: if (!w_release_token(j)) return; break;
            case W_DONE:          wjob_finalize(j, j->stopped); return;
            case W_INIT:
            case W_TOKEN_WAIT:
            case W_ERROR:
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
 *  Public entry — ObjWriteAsync
 * ========================================================================= */

int ObjWriteAsync(uint32_t                file_id,
                  uint64_t                offset,
                  bool                    append,
                  const PlotSource       *source,
                  Crate                  *out_crate,
                  const struct OpContext *ctx,
                  Crate                  *crates_kbuf,
                  uint16_t                crate_count,
                  uint64_t                crates_uaddr,
                  uint64_t                waybill)
{
    if (!ctx || !ctx->proc || !source || source->bytes == 0) return ERR_INVALID_ARGUMENT;

    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    if (!handle) return ERR_FILE_NOT_FOUND;

    void *dma_phys = pmm_alloc(1, PHYS_TAG_DMA32);
    if (!dma_phys) {
        tagfs_close(handle);
        return ERR_NO_MEMORY;
    }
    void *dma_virt = vmm_phys_to_virt((uintptr_t)dma_phys);

    process_t *target = process_find_ref(ctx->proc->pid);
    if (!target) {
        pmm_free(dma_phys, 1);
        tagfs_close(handle);
        return ERR_INVALID_ARGUMENT;
    }

    WriteJob *j = kmalloc(sizeof(WriteJob));
    if (!j) {
        process_ref_dec(target);
        pmm_free(dma_phys, 1);
        tagfs_close(handle);
        return ERR_NO_MEMORY;
    }
    memset(j, 0, sizeof(*j));

    j->target        = target;
    j->file_id       = file_id;
    j->handle        = handle;
    j->ofe           = handle->ofe;
    j->out_crate     = out_crate;
    j->source        = *source;
    j->asked_offset  = offset;
    j->append        = append;
    j->waybill       = waybill;
    j->submit_cookie = ctx->submit_cookie;
    j->dma_phys      = dma_phys;
    j->dma_virt      = dma_virt;

    /* Never-drop completion node: the AHCI IRQ / token handoff posts this
     * job to the drain core with no allocation. Set once, reused across
     * every enqueue of this job. */
    j->cq_node.run   = wjob_pump;
    j->cq_node.ctx   = j;

    /* CrateStage ownership transfer from dispatcher. After we return
     * ERR_WOULD_BLOCK below (via process_set_state PROC_WAITING), the
     * dispatcher will not touch the staged crates kbuf. wjob_finalize
     * writes out_crate->size = bytes and calls
     * crate_stage_commit_and_release to flush + free. */
    j->crates_kbuf   = crates_kbuf;
    j->crate_count   = crate_count;
    j->crates_uaddr  = crates_uaddr;

    process_set_state(ctx->proc, PROC_WAITING);
    /* The chit, and with it the staged-crates ownership transfer to
     * wjob_finalize (the dispatcher then skips its sync commit_out + kfree —
     * race-safe vs a very fast completion). Before the token claim: a job
     * that gets the token pumps at once, and can finish on another core. */
    ChitGive(ctx, "storage.write", file_id);

    if (token_try_claim(j)) {
        wjob_to(j, W_ALLOC);
        /* Run the first pump synchronously in this syscall (thread) context,
         * not as a posted continuation: thread context can safely take every
         * lock the state machine needs and skips a queue round-trip on the
         * common path. It pumps until it parks at W_AHCI_SUBMIT (yield to the
         * AHCI IRQ, whose completion posts the job's never-drop node to the
         * drain core). Any continuation the machine posts from here — e.g. a
         * token handoff on an early-error exit — goes through
         * BatonPass, which routes to the drain core (never the
         * calling App Core), so it cannot strand. */
        wjob_pump(j);
    } else {
        wjob_to(j, W_TOKEN_WAIT);
    }

    return ERR_WOULD_BLOCK;
}
