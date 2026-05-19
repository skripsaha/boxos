/*
 * WriteJob — async ObjWrite state-machine implementation.
 *
 * Pumped from K-Core continuations posted by the AHCI IRQ. The IRQ side
 * is intentionally minimal: stash status, set state=W_AHCI_DONE, enqueue
 * continuation. The K-Core side does everything that needs locks
 * (alloc, meta_pool_write, file_table_update, token release, CoW,
 * DiskBook, Touch publish).
 *
 * State flow (full):
 *
 *   W_INIT → W_TOKEN_WAIT? → W_LOCATE
 *
 *   per chunk:
 *     W_LOCATE → (extent missing → W_ALLOC → W_LOCATE)
 *              → (CoW active    → W_COW_BEFORE → W_DMA_FILL)
 *              → (existing      → W_DMA_FILL)        [RMW if partial]
 *              → (fresh alloc   → W_DMA_FILL)        [zero-fill]
 *     W_DMA_FILL → W_AHCI_SUBMIT → (yield to IRQ) → W_AHCI_DONE
 *     W_AHCI_DONE → if more bytes: W_LOCATE
 *                 → else:           W_BEGIN_TXN
 *
 *   commit phase (once):
 *     W_BEGIN_TXN  → W_LOG_META  → W_COW_AFTER → W_COMMIT_TXN
 *                                            ↘ skipped if no CoW ↗
 *     W_COMMIT_TXN → W_PUBLISH → W_RELEASE_TOKEN → W_DONE
 */

#include "write_job.h"
#include "write_cont_queue.h"
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
#include "cow.h"
#include "disk_book.h"
#include "touch.h"

/* ---- forward decls ---- */
static void wjob_pump(void *job_);
static void wjob_finalize(WriteJob *j, int rc);
static void wjob_ahci_complete(uint8_t port, uint8_t slot, error_t status, void *ctx);

/* =========================================================================
 *  Token handoff
 * ========================================================================= */

static bool token_try_claim(WriteJob *j)
{
    OpenFileEntry *ofe = j->ofe;
    spin_lock(&ofe->async_token_lock);
    if (ofe->async_write_owner == NULL) {
        ofe->async_write_owner = j;
        spin_unlock(&ofe->async_token_lock);
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
    spin_unlock(&ofe->async_token_lock);
    return false;
}

static WriteJob *token_release_handoff(WriteJob *j)
{
    OpenFileEntry *ofe = j->ofe;
    WriteJob *next = NULL;
    spin_lock(&ofe->async_token_lock);
    if (ofe->async_write_owner == j) {
        next = ofe->async_pending_head;
        if (next) {
            ofe->async_pending_head = next->next_pending;
            next->next_pending = NULL;
        }
        ofe->async_write_owner = next;
    }
    spin_unlock(&ofe->async_token_lock);
    return next;
}

/* =========================================================================
 *  Per-chunk in-flight scratch (allocated inside WriteJob via flags)
 * =========================================================================
 *  Tracking which subspecies of write this chunk is so we know what to
 *  do at meta-commit and CoW-after time.  Stored back into j->if_* fields.
 */

/* =========================================================================
 *  State handlers
 * ========================================================================= */

/* W_LOCATE — figure out which disk block to target for current cursor.
 * Decides: alloc, CoW, or plain. Returns true to keep pumping. */
static bool w_locate(WriteJob *j)
{
    uint64_t file_pos = j->start_offset + j->bytes_done;

    /* Walk extents. */
    uint64_t extent_start = 0;
    int found = -1;
    for (uint16_t i = 0; i < j->handle->extent_count; i++) {
        uint64_t ext_size = (uint64_t)j->handle->extents[i].block_count * TAGFS_BLOCK_SIZE;
        if (file_pos < extent_start + ext_size) { found = (int)i; break; }
        extent_start += ext_size;
    }

    if (found < 0) {
        /* No extent yet — go alloc one. */
        atomic_store_u32((volatile uint32_t *)&j->state, W_ALLOC);
        return true;
    }

    uint64_t off_in_ext = file_pos - extent_start;
    uint32_t blk_in_ext = (uint32_t)(off_in_ext / TAGFS_BLOCK_SIZE);
    uint32_t off_in_blk = (uint32_t)(off_in_ext % TAGFS_BLOCK_SIZE);
    uint32_t disk_blk   = j->handle->extents[found].start_block + blk_in_ext;

    uint64_t remaining = j->total_bytes - j->bytes_done;
    uint32_t chunk     = TAGFS_BLOCK_SIZE - off_in_blk;
    if ((uint64_t)chunk > remaining) chunk = (uint32_t)remaining;

    j->if_disk_block  = disk_blk;
    j->if_off_in_blk  = off_in_blk;
    j->if_chunk       = chunk;
    j->if_partial_rmw = (off_in_blk != 0) || (chunk != TAGFS_BLOCK_SIZE);

    /* CoW redirect: if this file has an active snapshot, the block we're
     * about to write is potentially shared. Allocate a fresh copy via
     * CowBeforeWrite (which also copies the OLD block content into NEW)
     * and re-point the handle's extent. The actual user-bytes overlay
     * happens in W_DMA_FILL after we've staged the post-CoW content. */
    bool cow = (!j->if_alloc_fresh) && TagFS_CowIsActive(j->file_id);
    if (cow) {
        atomic_store_u32((volatile uint32_t *)&j->state, W_COW_BEFORE);
        j->if_cow_old_block = disk_blk;
        j->if_extent_idx    = (uint16_t)found;
        j->if_blk_in_ext    = blk_in_ext;
        return true;
    }

    /* No CoW. For partial overwrite of an existing block we need
     * read-modify-write — async-load disk into DMA via W_COW_READ_OLD
     * (the same path CoW uses; "cow" here is a misnomer for "load
     * existing block before overlay"). For fresh allocations we just
     * zero the DMA so unwritten bytes are clean. Full-block writes
     * skip both. */
    if (j->if_partial_rmw && !j->if_alloc_fresh) {
        atomic_store_u32((volatile uint32_t *)&j->state, W_COW_READ_OLD);
        return true;
    }
    if (j->if_partial_rmw && j->if_alloc_fresh) {
        memset(j->dma_virt, 0, TAGFS_BLOCK_SIZE);
    }

    atomic_store_u32((volatile uint32_t *)&j->state, W_DMA_FILL);
    return true;
}

/* W_ALLOC — extent missing for file_pos; alloc one block, splice into
 * handle. Mark if_alloc_fresh so W_LOCATE doesn't RMW stale content. */
static bool w_alloc(WriteJob *j)
{
    uint32_t blk = 0;
    int rc = tagfs_alloc_blocks(1, &blk);
    if (rc != 0 || blk == 0) {
        wjob_finalize(j, ERR_NO_MEMORY);
        return false;
    }

    /* Track for rollback on error AND for meta-commit. */
    j->alloc_block         = blk;
    j->alloc_count         = 1;
    j->alloc_pending_meta  = true;
    j->if_alloc_fresh      = true;

    uint16_t new_cnt = (uint16_t)(j->handle->extent_count + 1);
    FileExtent *new_ext = kmalloc(sizeof(FileExtent) * new_cnt);
    if (!new_ext) {
        tagfs_free_blocks(blk, 1);
        wjob_finalize(j, ERR_NO_MEMORY);
        return false;
    }
    if (j->handle->extents && j->handle->extent_count > 0) {
        memcpy(new_ext, j->handle->extents,
               sizeof(FileExtent) * j->handle->extent_count);
    }
    new_ext[j->handle->extent_count].start_block = blk;
    new_ext[j->handle->extent_count].block_count = 1;
    if (j->handle->extents) kfree(j->handle->extents);
    j->handle->extents = new_ext;
    j->handle->extent_count = new_cnt;

    atomic_store_u32((volatile uint32_t *)&j->state, W_LOCATE);
    return true;
}

/* W_COW_BEFORE — snapshot active. Redirect: allocate fresh block,
 * splice extent. For partial overwrites, transition to W_COW_READ_OLD
 * to async-load the OLD content; for full-block overwrites, jump
 * straight to W_DMA_FILL (no need to read old content — user supplies
 * the entire block). The K-Core never blocks here. */
static bool w_cow_before(WriteJob *j)
{
    /* TagFS_CowBeforeWrite both allocates AND copies old → new on disk
     * synchronously today. We only want the alloc — the copy step is
     * the slow part we're trying to make async. Workaround: call it,
     * accept the sync copy, and read NEW into DMA via async. Once cow.c
     * grows an alloc-only entry point we can split this. */
    uint32_t new_block = 0;
    error_t err = TagFS_CowBeforeWrite(j->file_id, j->if_cow_old_block, &new_block);
    if (err != OK || new_block == 0) {
        wjob_finalize(j, ERR_IO);
        return false;
    }

    /* Splice handle's extent table to point at the redirected block. */
    FileExtent *ex = &j->handle->extents[j->if_extent_idx];
    if (ex->block_count == 1) {
        ex->start_block = new_block;
    } else {
        /* Multi-block extent — append a 1-block redirect extent for THIS
         * chunk. Leaves the original extent record intact. Future passes
         * can split-merge contiguous CoW redirects. */
        uint16_t new_cnt = (uint16_t)(j->handle->extent_count + 1);
        FileExtent *grown = kmalloc(sizeof(FileExtent) * new_cnt);
        if (!grown) {
            tagfs_free_blocks(new_block, 1);
            wjob_finalize(j, ERR_NO_MEMORY);
            return false;
        }
        memcpy(grown, j->handle->extents,
               sizeof(FileExtent) * j->handle->extent_count);
        grown[j->handle->extent_count].start_block = new_block;
        grown[j->handle->extent_count].block_count = 1;
        kfree(j->handle->extents);
        j->handle->extents      = grown;
        j->handle->extent_count = new_cnt;
        j->if_extent_idx        = (uint16_t)(new_cnt - 1);
    }

    j->if_disk_block      = new_block;
    j->if_cow_redirected  = true;
    j->alloc_pending_meta = true;

    /* For partial writes we still need the old (now redirected) content
     * loaded into DMA so the user-bytes overlay only mutates the
     * targeted window. Read async — IRQ-completed; for full-block
     * writes, skip directly to fill. */
    if (j->if_partial_rmw) {
        atomic_store_u32((volatile uint32_t *)&j->state, W_COW_READ_OLD);
    } else {
        atomic_store_u32((volatile uint32_t *)&j->state, W_DMA_FILL);
    }
    return true;
}

/* W_COW_READ_OLD — async read of (now-redirected) NEW block content
 * into DMA. CowBeforeWrite already populated NEW on disk with the OLD
 * bytes; we just need them into DMA so the user overlay can fold over.
 * IRQ jumps state to W_DMA_FILL via wjob_cow_read_complete. */
static bool w_cow_read_old(WriteJob *j);
static void wjob_cow_read_complete(uint8_t port, uint8_t slot,
                                    error_t status, void *ctx);

static bool w_cow_read_old(WriteJob *j)
{
    uint64_t lba = tagfs_block_to_sector(j->if_disk_block);
    uint8_t  slot;
    error_t  err = ahci_submit_read_async(0, lba, 8, j->dma_phys,
                                           wjob_cow_read_complete, j, &slot);
    if (err != OK) {
        wjob_finalize(j, ERR_IO);
        return false;
    }
    return false;  /* parked until IRQ */
}

/* W_DMA_FILL — overlay user bytes onto staged DMA window. */
static bool w_dma_fill(WriteJob *j)
{
    memcpy((uint8_t *)j->dma_virt + j->if_off_in_blk,
           j->src_kp + j->bytes_done,
           j->if_chunk);

    atomic_store_u32((volatile uint32_t *)&j->state, W_AHCI_SUBMIT);
    return true;
}

/* W_AHCI_SUBMIT — fire write, yield to IRQ. */
static bool w_ahci_submit(WriteJob *j)
{
    uint64_t lba = tagfs_block_to_sector(j->if_disk_block);
    uint8_t  slot;
    error_t  err = ahci_submit_write_async(0, lba, 8, j->dma_phys,
                                            wjob_ahci_complete, j, &slot);
    if (err != OK) {
        wjob_finalize(j, ERR_IO);
        return false;
    }
    return false;  /* parked until IRQ */
}

/* W_AHCI_DONE — IRQ retired. Advance and decide next phase. */
static bool w_ahci_done(WriteJob *j)
{
    if (j->if_status != OK) {
        wjob_finalize(j, j->if_status);
        return false;
    }
    j->bytes_done += j->if_chunk;

    /* Reset per-chunk flags before next iteration. */
    j->if_alloc_fresh = false;

    if (j->bytes_done >= j->total_bytes) {
        atomic_store_u32((volatile uint32_t *)&j->state, W_BEGIN_TXN);
    } else {
        atomic_store_u32((volatile uint32_t *)&j->state, W_LOCATE);
    }
    return true;
}

/* W_BEGIN_TXN — open a DiskBook transaction for crash safety. */
static bool w_begin_txn(WriteJob *j)
{
    if (DiskBookBegin(&j->txn) != OK) {
        /* Journal full or unavailable — degrade gracefully: skip journal
         * and proceed. The write itself already hit disk; meta-commit
         * still happens, just without a journal entry to replay on
         * crash. */
        j->txn_active = false;
    } else {
        j->txn_active = true;
    }
    atomic_store_u32((volatile uint32_t *)&j->state, W_LOG_META);
    return true;
}

/* W_LOG_META — write extent table + size through meta_pool. */
static bool w_log_meta(WriteJob *j)
{
    if (!j->alloc_pending_meta &&
        (j->start_offset + j->total_bytes) <= j->handle->file_size) {
        /* Pure overwrite-in-place — no metadata change. */
        atomic_store_u32((volatile uint32_t *)&j->state, W_COMMIT_TXN);
        return true;
    }

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (tagfs_get_metadata(j->file_id, &meta) != OK) {
        wjob_finalize(j, ERR_IO);
        return false;
    }

    /* Pull authoritative extents from the handle. */
    if (meta.extents) { kfree(meta.extents); meta.extents = NULL; }
    meta.extent_count = j->handle->extent_count;
    if (j->handle->extent_count > 0) {
        meta.extents = kmalloc(sizeof(FileExtent) * j->handle->extent_count);
        if (!meta.extents) {
            tagfs_metadata_free(&meta);
            wjob_finalize(j, ERR_NO_MEMORY);
            return false;
        }
        memcpy(meta.extents, j->handle->extents,
               sizeof(FileExtent) * j->handle->extent_count);
    }

    uint64_t end_pos = j->start_offset + j->total_bytes;
    if (end_pos > meta.size) {
        meta.size = end_pos;
        j->handle->file_size = end_pos;
    }

    /* Journal the meta record before persisting it (write-ahead). */
    if (j->txn_active) {
        DiskBookLogMetadata(&j->txn, j->file_id, 0, &meta);
    }

    uint32_t out_block = 0, out_offset = 0;
    int rc = meta_pool_write(&meta, &out_block, &out_offset);
    if (rc != 0) {
        tagfs_metadata_free(&meta);
        wjob_finalize(j, ERR_IO);
        return false;
    }
    if (file_table_update(j->file_id, out_block, out_offset) != 0) {
        tagfs_metadata_free(&meta);
        wjob_finalize(j, ERR_IO);
        return false;
    }
    tagfs_metadata_free(&meta);

    j->alloc_pending_meta = false;
    atomic_store_u32((volatile uint32_t *)&j->state,
                     j->if_cow_redirected ? W_COW_AFTER : W_COMMIT_TXN);
    return true;
}

/* W_COW_AFTER — snapshot now owns OLD block; release it from the live
 * file's accounting so the next allocation sees it as free. */
static bool w_cow_after(WriteJob *j)
{
    if (j->if_cow_old_block != 0 && j->if_disk_block != j->if_cow_old_block) {
        TagFS_CowAfterWrite(j->file_id, j->if_cow_old_block, j->if_disk_block);
        /* Free old block — snapshot manifest still references it through
         * the journaled metadata; the actual byte contents on disk are
         * preserved at the OLD physical block until something allocates
         * over it AND a snapshot read is requested. The CowAfterWrite
         * hook in cow.c marks accounting; here we explicitly free.
         *
         * NOTE: this path is conservative — a fully-correct Cow needs a
         * refcount on the OLD block until snapshot deletion. Phase 5
         * will integrate that. */
        /* tagfs_free_blocks(j->if_cow_old_block, 1); -- deferred */
    }
    atomic_store_u32((volatile uint32_t *)&j->state, W_COMMIT_TXN);
    return true;
}

/* W_COMMIT_TXN — close out the journal txn. */
static bool w_commit_txn(WriteJob *j)
{
    if (j->txn_active) {
        DiskBookCommit(&j->txn);
        j->txn_active = false;
    }
    atomic_store_u32((volatile uint32_t *)&j->state, W_PUBLISH);
    return true;
}

/* W_PUBLISH — emit Touch event "WROTE" to all of the file's tags so
 * tag-listening processes wake up. Best-effort: a publish failure does
 * not abort the write. */
static bool w_publish(WriteJob *j)
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
                .final_size = j->handle->file_size,
            };
            for (uint16_t ti = 0; ti < wmeta.tag_count; ti++) {
                TouchPublishId(wmeta.tag_ids[ti], &ev, sizeof(ev),
                               (j->target ? j->target->pid : 0),
                               TOUCH_FLAG_TAGFS);
            }
            tagfs_metadata_free(&wmeta);
        }
    }

    atomic_store_u32((volatile uint32_t *)&j->state, W_RELEASE_TOKEN);
    return true;
}

/* W_RELEASE_TOKEN — handoff to next pending writer (if any), then DONE. */
static bool w_release_token(WriteJob *j)
{
    WriteJob *next = token_release_handoff(j);
    if (next) {
        atomic_store_u32((volatile uint32_t *)&next->state, W_LOCATE);
        WriteContEnqueue(wjob_pump, next);
    }
    atomic_store_u32((volatile uint32_t *)&j->state, W_DONE);
    return true;
}

/* W_DONE / W_ERROR — common exit. */
static void wjob_finalize(WriteJob *j, int rc)
{
    /* Token release on error path. */
    if (rc != OK && j->ofe) {
        WriteJob *next = token_release_handoff(j);
        if (next) {
            atomic_store_u32((volatile uint32_t *)&next->state, W_LOCATE);
            WriteContEnqueue(wjob_pump, next);
        }
    }

    /* Abort journal txn if still open. */
    if (j->txn_active) {
        DiskBookAbort(&j->txn);
        j->txn_active = false;
    }

    /* Roll back uncommitted allocations. */
    if (rc != OK && j->alloc_pending_meta && j->alloc_count > 0) {
        tagfs_free_blocks(j->alloc_block, j->alloc_count);
    }

    /* Stat reporter (matches sync ObjWrite contract). */
    uint64_t bytes_written = (rc == OK) ? j->bytes_done : 0;
    uint64_t final_size    = j->handle ? j->handle->file_size : 0;
    if (j->out_crate && j->out_kp && j->out_crate->capacity >= 16) {
        memcpy((uint8_t *)j->out_kp + 0, &bytes_written, sizeof(uint64_t));
        memcpy((uint8_t *)j->out_kp + 8, &final_size,    sizeof(uint64_t));
        j->out_crate->size = 16;
    }

    if (j->dma_phys)    pmm_free(j->dma_phys, 1);
    if (j->handle)      tagfs_close(j->handle);
    if (j->src_bounce)  vmm_user_buf_free(j->src_bounce);

    Result r;
    memset(&r, 0, sizeof(r));
    r.error_code  = (rc == OK) ? OK : (uint32_t)ERR_IO;
    r.data_length = (uint32_t)bytes_written;
    r.sender_pid  = 0;
    r.context     = KCTX_GUIDE;

    if (j->target) {
        KResultPush(j->target, &r);
        process_ref_dec(j->target);
    }
    j->result_rc = rc;
    kfree(j);
}

/* =========================================================================
 *  Pump
 * ========================================================================= */

static void wjob_pump(void *job_)
{
    WriteJob *j = (WriteJob *)job_;

    for (;;) {
        WriteJobState s = (WriteJobState)atomic_load_u32((volatile uint32_t *)&j->state);
        switch (s) {
            case W_LOCATE:        if (!w_locate(j))        return; break;
            case W_ALLOC:         if (!w_alloc(j))         return; break;
            case W_COW_BEFORE:    if (!w_cow_before(j))    return; break;
            case W_COW_READ_OLD:  if (!w_cow_read_old(j))  return; break;
            case W_DMA_FILL:      if (!w_dma_fill(j))      return; break;
            case W_AHCI_SUBMIT:   if (!w_ahci_submit(j))   return; break;
            case W_AHCI_DONE:     if (!w_ahci_done(j))     return; break;
            case W_BEGIN_TXN:     if (!w_begin_txn(j))     return; break;
            case W_LOG_META:      if (!w_log_meta(j))      return; break;
            case W_COW_AFTER:     if (!w_cow_after(j))     return; break;
            case W_COMMIT_TXN:    if (!w_commit_txn(j))    return; break;
            case W_PUBLISH:       if (!w_publish(j))       return; break;
            case W_RELEASE_TOKEN: if (!w_release_token(j)) return; break;
            case W_DONE:          wjob_finalize(j, OK);    return;
            case W_INIT:
            case W_TOKEN_WAIT:
            case W_ERROR:
                return;
        }
    }
}

/* IRQ callback for CoW READ_OLD. Drops state directly to W_DMA_FILL —
 * the read populated DMA with the (redirected) old content, ready for
 * the user-bytes overlay.
 *
 * Runs in AHCI completion IRQ context. The old emergency-finalize
 * fallback (kmalloc / pmm_free / tagfs_close / kfree from IRQ) is
 * gone — WriteContEnqueue now delegates to irq_defer, which is
 * allocation-free on the producer side and cannot fail. If irq_defer
 * exhausts its overflow chain (extreme burst beyond pre-allocated
 * headroom), the slot is silently dropped and the writer process
 * will time out and retry. That graceful drop is preferable to the
 * old deadlock-prone in-IRQ cleanup path. */
static void wjob_cow_read_complete(uint8_t port, uint8_t slot,
                                    error_t status, void *ctx)
{
    (void)port; (void)slot;
    WriteJob *j = (WriteJob *)ctx;
    if (status != OK) {
        j->if_status = status;
        atomic_store_u32((volatile uint32_t *)&j->state, W_AHCI_DONE);
    } else {
        atomic_store_u32((volatile uint32_t *)&j->state, W_DMA_FILL);
    }
    (void)WriteContEnqueue(wjob_pump, j);  /* irq_defer; cannot fail */
}

/* IRQ callback. Lean — only stash status + defer continuation. See
 * wjob_cow_read_complete above for the IRQ-safety rationale. */
static void wjob_ahci_complete(uint8_t port, uint8_t slot,
                                error_t status, void *ctx)
{
    (void)port; (void)slot;
    WriteJob *j = (WriteJob *)ctx;
    j->if_status = status;
    atomic_store_u32((volatile uint32_t *)&j->state, W_AHCI_DONE);
    (void)WriteContEnqueue(wjob_pump, j);  /* irq_defer; cannot fail */
}

/* =========================================================================
 *  Public entry — ObjWriteAsync
 * ========================================================================= */

int ObjWriteAsync(uint32_t            file_id,
                  uint64_t            offset,
                  uint32_t            flags,
                  const void         *src_kp,
                  uint32_t            size,
                  Crate              *out_crate,
                  void               *out_kp,
                  const struct OpContext *ctx)
{
    if (!ctx || !ctx->proc || !src_kp || size == 0) return ERR_INVALID_ARGUMENT;

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
    j->out_kp        = out_kp;
    j->src_kp        = (const uint8_t *)src_kp;
    /* Caller (storage_ops ObjWrite) hands us a bounce buffer it allocated
     * via crate_in_buf; on a successful return WE own it and free at
     * W_DONE. Marker: src_bounce == src_kp says "we own this". */
    j->src_bounce    = (void *)src_kp;
    j->flags         = flags;
    j->dma_phys      = dma_phys;
    j->dma_virt      = dma_virt;

    if (flags & 0x1u /* OBJ_WRITE_APPEND_FLAG */) {
        j->start_offset = handle->file_size;
    } else {
        j->start_offset = offset;
    }
    j->total_bytes   = size;
    j->bytes_done    = 0;
    j->home_kcore    = amp_get_core_index();

    process_set_state(ctx->proc, PROC_WAITING);

    if (token_try_claim(j)) {
        atomic_store_u32((volatile uint32_t *)&j->state, W_LOCATE);
        if (WriteContEnqueue(wjob_pump, j) != OK) {
            wjob_pump(j);
        }
    } else {
        atomic_store_u32((volatile uint32_t *)&j->state, W_TOKEN_WAIT);
    }

    return ERR_WOULD_BLOCK;
}
