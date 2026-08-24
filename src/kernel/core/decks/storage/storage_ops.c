/*
 * Storage Deck — Manifest-native handlers.
 *
 * Removes the 168/176-byte stack buffers that capped legacy OBJ_READ /
 * OBJ_WRITE at sub-200-byte payloads per syscall. Reads and writes now move
 * to / from a Crate of arbitrary capacity through the page-walked crate_io
 * primitives, so a payload that straddles a page boundary is copied across
 * every backing frame. A 1-MiB read fits in one syscall.
 *
 * Phase 7 scope: OBJ_READ, OBJ_WRITE, OBJ_DELETE, OBJ_RENAME, OBJ_GET_INFO.
 * OBJ_CREATE, TAG_*, CONTEXT_* deferred — they need a separate pass because
 * of variable-length tag-string parsing.
 *
 * Param layouts (little-endian, packed):
 *   OBJ_READ      params: [u32 file_id][u64 offset]
 *   OBJ_WRITE     params: [u32 file_id][u64 offset][u32 flags]
 *   OBJ_DELETE    params: [u32 file_id]
 *   OBJ_RENAME    params: [u32 file_id][u16 new_name_len][char new_name[]]
 *   OBJ_GET_INFO  params: [u32 file_id]
 *
 *   OBJ_READ.out_crate     : payload destination; size <- bytes_read
 *   OBJ_WRITE.in_crate     : payload source
 *   OBJ_WRITE.out_crate    : optional [u64 bytes_written][u64 new_file_size]
 *   OBJ_GET_INFO.out_crate : serialized record, see ObjGetInfo() below
 */

#include "klib.h"
#include "crate_stage.h"
#include "crate_io.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "storage_deck.h"
#include "tagfs.h"
#include "tag_registry.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "ahci.h"
#include "ahci_async.h"
#include "kring.h"
#include "kresult.h"
#include "write_job.h"
#include "storage_completion.h"   /* StorageCompletionPush — never-drop read completion (Ф26 M2/M4) */
#include "ata.h"
#include "touch.h"
#include "cow.h"
#include "amp.h"   /* g_amp.total_cores — async write needs a K-Core to pump */
#include "boardroom.h"

/*
 * The asynchronous disk path is AHCI's, and only AHCI's.
 *
 * It used to be gated on "is there an AHCI controller?", which was the same
 * question as "is the volume on it" only for as long as there was nowhere else
 * a volume could be. There is now: a machine that boots from a flash drive has
 * its filesystem on the USB bus and its SATA controller initialised beside it,
 * and submitting that filesystem's reads to an AHCI port would have read
 * somebody else's disk and called the bytes a file.
 */
static inline bool tagfs_volume_is_ahci(void)
{
    return BoardroomSeatKind(tagfs_get_seat()) == BOARD_AHCI;
}

#define OBJ_WRITE_APPEND_FLAG (1u << 0)

/* -------------------------------------------------------------------------
 * Crate I/O lives in crate_io.{h,c}: crate_read / crate_write move a fixed
 * payload between a user Crate and a kernel buffer, and crate_in_buf /
 * crate_out_alloc / crate_out_commit / crate_buf_free carry the variable-
 * size bounce-buffer pattern. All page-walk the user range, so a Crate
 * payload that straddles a page boundary is copied across every backing
 * frame — the legacy single-page StorageCrateMap fast path is gone.
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Param accessors — alignment-safe.
 * ------------------------------------------------------------------------- */

static uint32_t param_u32(const ManifestOp *op, uint16_t off)
{
    uint32_t v;
    memcpy(&v, op->params + off, sizeof(v));
    return v;
}

static uint64_t param_u64(const ManifestOp *op, uint16_t off)
{
    uint64_t v;
    memcpy(&v, op->params + off, sizeof(v));
    return v;
}

static uint16_t param_u16(const ManifestOp *op, uint16_t off)
{
    uint16_t v;
    memcpy(&v, op->params + off, sizeof(v));
    return v;
}

/* -------------------------------------------------------------------------
 * OBJ_READ — fill out_crate with up to capacity bytes from file at offset.
 *
 * Stage 2 async fast-path: when the request lands entirely inside one
 * disk block, we submit an ahci_submit_read_async, park the caller
 * (PROC_WAITING) and return ERR_WOULD_BLOCK. The AHCI IRQ fires the
 * obj_read_async_complete callback which memcpy's DMA → user buffer,
 * writes the actual byte count back into the Crate, KResultPush'es a
 * Result and ref_dec's the pinned process — KResultPush also flips
 * PROC_WAITING → PROC_WORKING so the caller wakes.
 *
 * Fallback (multi-block, off-block, no AHCI, allocation failure, no
 * proc context) takes the existing tagfs_read sync path.
 * ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------
 * Multi-block async read state machine.
 *
 * Each completed disk read fires the callback which memcpy's the chunk
 * out of the reusable DMA page, advances the file cursor, and either
 * submits the NEXT block read (state transition) or finishes the
 * request (KResultPush + cleanup). Sequential submission keeps the
 * model simple; future enhancement could batch consecutive
 * intra-extent blocks into one multi-sector read.
 *
 * The DMA buffer is a single 4 KiB page reused across iterations;
 * each iteration overwrites the previous read since we memcpy out
 * before submitting again.
 * ------------------------------------------------------------------------ */
typedef struct {
    process_t       *target;       /* pinned via process_find_ref */
    TagFSFileHandle *handle;       /* close on completion */
    Crate           *out_crate;    /* points into crates_kbuf; valid until release */
    uint8_t         *out_base;     /* kernel-side bounce buffer (kmalloc'd) */
    uintptr_t        out_user_addr;/* user vaddr; commit_out target for payload */
    uint64_t         total_bytes;
    uint64_t         bytes_done;
    uint64_t         start_offset;
    uint64_t         waybill;       /* Ф26e: ferry correlation token (0 = sync/no-token) */
    void            *dma_phys;
    void            *dma_virt;
    uint32_t         in_flight_off_in_blk;
    uint32_t         in_flight_chunk;
    error_t          if_status;      /* Ф26 M2: AHCI IRQ stashes status here; obj_read_pump reads it */

    /* CrateStage handoff: ownership of the staged Crate[] buffer transfers
     * from the dispatcher to this async context the moment ObjRead returns
     * ERR_WOULD_BLOCK with PROC_WAITING. Async completion writes
     * crates_kbuf[idx].size = N (via out_crate above), then calls
     * crate_stage_commit_and_release to write back + free. */
    Crate           *crates_kbuf;
    uint64_t         crates_uaddr;
    uint16_t         crate_count;

    /* Never-drop completion node (Ф26 M4): the AHCI IRQ posts this read to
     * the drain core with zero allocation, so a completion can never be
     * dropped. run = obj_read_pump, ctx = this ctx; set once at alloc. */
    StorageCompletion cq_node;
} ObjReadAsyncCtx;

static void obj_read_step(ObjReadAsyncCtx *ctx);
static void obj_read_finish(ObjReadAsyncCtx *ctx, error_t status, bool partial_ok);
static void obj_read_async_complete(uint8_t port, uint8_t slot,
                                     error_t status, void *ctx_);
static void obj_read_pump(void *ctx_);   /* Ф26 M2: K-Core half of a read completion */

static void obj_read_finish(ObjReadAsyncCtx *ctx, error_t status, bool partial_ok)
{
    Result r;
    memset(&r, 0, sizeof(r));
    uint64_t reported = partial_ok ? ctx->bytes_done : 0;
    ctx->out_crate->size = reported;
    r.error_code  = (status == OK) ? OK : ERR_IO;
    r.data_length = (uint32_t)reported;
    r.sender_pid  = 0;
    /* Ф26e: a waybilled (box::ferry) read echoes its correlation token in
     * data_addr and flies the KCTX_STORAGE flag so boxlib routes it to the
     * ferry station and nothing else. A plain (waybill==0) read keeps
     * KCTX_GUIDE / data_addr==0 — byte-identical to the pre-ferry substrate. */
    if (ctx->waybill) { r.context = KCTX_STORAGE; r.data_addr = ctx->waybill; }
    else              { r.context = KCTX_GUIDE; }

    /* Commit bounce buffer back into user pages BEFORE waking caller.
     * If commit fails (e.g. user unmapped the page mid-flight) we still
     * report the byte count — the user's buffer is just left untouched. */
    if (reported > 0 && ctx->out_base && ctx->target && ctx->target->cabin) {
        vmm_user_buf_commit_out(ctx->target->cabin->vmm, ctx->out_user_addr,
                                 ctx->out_base, (size_t)reported);
    }

    if (ctx->out_base)  vmm_user_buf_free(ctx->out_base);
    if (ctx->dma_phys)  pmm_free(ctx->dma_phys, 1);
    if (ctx->handle)    tagfs_close(ctx->handle);

    /* CrateStage release: the dispatcher handed the staged Crate[] buffer
     * to this async context at ERR_WOULD_BLOCK. We wrote crates[idx].size
     * just above; now commit the whole descriptor array back to user
     * memory and free the kbuf. Always run, even on partial-fail paths —
     * the dispatcher will NOT clean up after async-park. */
    if (ctx->crates_kbuf) {
        crate_stage_commit_and_release(ctx->crates_kbuf, ctx->crate_count,
                                        (ctx->target && ctx->target->cabin) ? ctx->target->cabin->vmm : NULL,
                                        ctx->crates_uaddr);
    }

    KResultPush(ctx->target, &r);
    process_ref_dec(ctx->target);
    kfree(ctx);
}

/* Ф26 M2/M4 — IRQ callback, LEAN. Runs in raw AHCI completion IRQ context. Stashes
 * the retire status and posts the heavy half (memcpy out of DMA, page-walked
 * commit, pmm/vmm free, tagfs_close, crate-stage release, KResultPush, kfree, and
 * re-arming the next block) to the drain core's K-Core pump via the ctx's
 * embedded never-drop node. Doing that work HERE is the exact "no allocation /
 * no thread-context locks from IRQ" violation diagnosed as the root of the
 * 2026-05-17 random-hang class; the write path (wjob_ahci_complete) posts the
 * same way. The node lives inside the ctx, so the post can never be dropped. */
static void obj_read_async_complete(uint8_t port, uint8_t slot,
                                     error_t status, void *ctx_)
{
    (void)port;
    (void)slot;
    ObjReadAsyncCtx *ctx = (ObjReadAsyncCtx *)ctx_;
    ctx->if_status = status;
    StorageCompletionPush(&ctx->cq_node);   /* never-drop; heavy half on the K-Core pump */
}

/* Ф26 M2 — K-Core pump: the heavy half of a read completion, drained by the same
 * StorageCompletionPump loop that pumps write jobs (kcore_run_loop). Safe to take locks /
 * alloc / free / page-walk here (pump context, not IRQ). Applies the just-read
 * chunk, advances the cursor, and either finalizes or arms the next block. */
static void obj_read_pump(void *ctx_)
{
    ObjReadAsyncCtx *ctx = (ObjReadAsyncCtx *)ctx_;

    if (ctx->if_status != OK) {
        obj_read_finish(ctx, ERR_IO, /*partial_ok=*/false);
        return;
    }

    /* Apply this chunk out of the reusable DMA page. The next block is armed
     * only AFTER this copy (both in pump context), so the device cannot
     * overwrite the page between completion and copy. */
    memcpy(ctx->out_base + ctx->bytes_done,
           (uint8_t *)ctx->dma_virt + ctx->in_flight_off_in_blk,
           ctx->in_flight_chunk);
    ctx->bytes_done += ctx->in_flight_chunk;

    if (ctx->bytes_done >= ctx->total_bytes) {
        obj_read_finish(ctx, OK, /*partial_ok=*/true);
        return;
    }

    /* More to do — arm the next block (now in pump context, not IRQ). */
    obj_read_step(ctx);
}

static void obj_read_step(ObjReadAsyncCtx *ctx)
{
    uint64_t file_pos = ctx->start_offset + ctx->bytes_done;

    /* Locate extent containing file_pos. */
    uint64_t extent_start = 0;
    uint16_t ext_idx = 0xFFFF;
    for (uint16_t i = 0; i < ctx->handle->extent_count; i++) {
        uint64_t ext_size = (uint64_t)ctx->handle->extents[i].block_count * TAGFS_BLOCK_SIZE;
        if (file_pos < extent_start + ext_size) { ext_idx = i; break; }
        extent_start += ext_size;
    }
    if (ext_idx == 0xFFFF) {
        /* Walked past last extent — finish with what we have. */
        obj_read_finish(ctx, OK, /*partial_ok=*/true);
        return;
    }

    uint64_t off_in_ext = file_pos - extent_start;
    uint32_t blk_in_ext = (uint32_t)(off_in_ext / TAGFS_BLOCK_SIZE);
    uint32_t off_in_blk = (uint32_t)(off_in_ext % TAGFS_BLOCK_SIZE);
    uint32_t disk_block = ctx->handle->extents[ext_idx].start_block + blk_in_ext;

    uint64_t remaining = ctx->total_bytes - ctx->bytes_done;
    uint32_t chunk     = TAGFS_BLOCK_SIZE - off_in_blk;
    if ((uint64_t)chunk > remaining) chunk = (uint32_t)remaining;

    ctx->in_flight_off_in_blk = off_in_blk;
    ctx->in_flight_chunk      = chunk;

    uint64_t lba = tagfs_block_to_sector(disk_block);
    uint8_t  slot;
    error_t  err = ahci_submit_read_async(BoardroomSeatIndex(tagfs_get_seat()),
                                           lba, 8, ctx->dma_phys,
                                           obj_read_async_complete, ctx, &slot);
    if (err != OK) {
        obj_read_finish(ctx, ERR_IO, /*partial_ok=*/false);
    }
}

/* Ф26e — deliver a box::ferry completion for a storage op that ran on the
 * SYNCHRONOUS fallback (single core, EOF read, or an allocation failure that
 * declined the async state machine). Without this the guide dispatcher would
 * push the op's ordinary KCTX_GUIDE self-Result, which carries no waybill, so
 * the ferry awaiter would match nothing and hang forever.
 *
 * Mirrors the async finaliser: flush the staged Crate[] descriptors back to
 * user memory and free the kbuf (the dispatcher will NOT, because we claim the
 * async-owner flag), publish a KCTX_STORAGE Result carrying the waybill + byte
 * count, and return ERR_WOULD_BLOCK so guide_process_manifest_pocket takes its
 * async-park branch (skips the crate commit + the KCTX_GUIDE push). The caller
 * was NOT parked — it ran synchronously and stays PROC_WORKING, consuming this
 * completion from its own ResultRing via the ferry poll. Descriptor write-back
 * precedes the KResultPush so a ferry that frees its submission block on
 * completion cannot race it (no UAF). */
static int storage_sync_ferry_finalize(const OpContext *ctx, Crate *crates,
                                       error_t err, uint64_t bytes,
                                       uint64_t waybill)
{
    if (crates && ctx->crate_count > 0 && ctx->proc && ctx->proc->cabin) {
        crate_stage_commit_and_release(crates, ctx->crate_count,
                                       ctx->proc->cabin->vmm, ctx->crates_uaddr);
    }

    Result r;
    memset(&r, 0, sizeof(r));
    r.error_code  = (uint32_t)err;
    r.data_length = (uint32_t)bytes;
    r.data_addr   = waybill;
    r.sender_pid  = 0;
    r.context     = KCTX_STORAGE;
    KResultPush(ctx->proc, &r);

    if (ctx->async_owns_crates) *ctx->async_owns_crates = true;
    return ERR_WOULD_BLOCK;
}

/* Ф26e — fail a storage op on the channel its submitter listens on. A waybilled
 * (box::ferry) op MUST answer on KCTX_STORAGE even from an early-error exit, or
 * its awaiter — which only ever collects KCTX_STORAGE records — waits for a
 * completion that never comes and hangs (the dispatcher would deliver the raw
 * code as a KCTX_GUIDE reply, which the ferry isolation routes away). A plain
 * (waybill==0) op returns the code for the ordinary KCTX_GUIDE push. */
static int storage_op_fail(const OpContext *ctx, Crate *crates,
                           error_t err, uint64_t waybill)
{
    if (waybill != 0)
        return storage_sync_ferry_finalize(ctx, crates, err, 0, waybill);
    return (int)err;
}

static int ObjRead(const ManifestOp *op,
                   Crate            *crates,
                   uint16_t          crate_count,
                   const OpContext  *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 12)               return ERR_INVALID_ARGUMENT;

    uint32_t file_id = param_u32(op, 0);
    uint64_t offset  = param_u64(op, 4);
    /* Ф26e: optional trailing correlation waybill (box::ferry). Absent for a
     * synchronous read (param_size 12) → 0 → plain KCTX_GUIDE completion. */
    uint64_t waybill = (op->param_size >= 20) ? param_u64(op, 12) : 0;
    Crate   *out     = &crates[op->out_crate];
    if (out->capacity == 0)
        return storage_op_fail(ctx, crates, ERR_BUFFER_TOO_SMALL, waybill);

    /* Bounce buffer for multi-page user payloads. tagfs_read fills kp,
     * we copy back to user pages at the end. The kernel address is one
     * contiguous allocation regardless of how the user pages map. */
    void *kp = crate_out_alloc(out, out->capacity);
    if (!kp) return storage_op_fail(ctx, crates, ERR_INVALID_ADDRESS, waybill);

    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    if (!handle) {
        crate_buf_free(kp);
        return storage_op_fail(ctx, crates, ERR_FILE_NOT_FOUND, waybill);
    }

    handle->offset = offset;

    /* Async path — multi-block state machine. Covers any in-bounds read
     * regardless of size or extent layout. The state machine submits one
     * disk read at a time; each completion's callback memcpy's the chunk
     * out of the reusable DMA page, advances the cursor and either fires
     * the next read or finalizes. Sequential per-block submission keeps
     * the model simple — future work can batch consecutive blocks within
     * one extent into a single multi-sector read. */
    /* Multi-core only: the async read completion (obj_read_finish) runs in
     * AHCI IRQ context and touches the heap / tagfs / result ring. On a
     * single core that IRQ preempts the very userspace thread that owns
     * those structures; the sync read path below is correct there and on
     * real HW (which is multi-core) the async path still applies. */
    if (offset < handle->file_size && handle->extent_count > 0 &&
        tagfs_volume_is_ahci() && ctx && ctx->proc && g_amp.total_cores > 1) {

        uint64_t remaining = handle->file_size - offset;
        uint64_t to_read   = (out->capacity > remaining) ? remaining : out->capacity;

        if (to_read > 0) {
            void *dma_phys = pmm_alloc(1, PHYS_TAG_DMA32);
            if (dma_phys) {
                void *dma_virt = vmm_phys_to_virt((uintptr_t)dma_phys);
                ObjReadAsyncCtx *async_ctx = kmalloc(sizeof(*async_ctx));
                if (async_ctx) {
                    process_t *target = process_find_ref(ctx->proc->pid);
                    if (target) {
                        async_ctx->target        = target;
                        async_ctx->handle        = handle;
                        async_ctx->out_crate     = out;
                        async_ctx->out_base      = (uint8_t *)kp;
                        async_ctx->out_user_addr = (uintptr_t)out->addr;
                        async_ctx->total_bytes   = to_read;
                        async_ctx->bytes_done    = 0;
                        async_ctx->start_offset  = offset;
                        async_ctx->waybill       = waybill;
                        async_ctx->dma_phys      = dma_phys;
                        async_ctx->dma_virt      = dma_virt;
                        /* CrateStage ownership transfer: the dispatcher
                         * staged the Crate[] kbuf and points `crates`
                         * (handler arg) at it. We take that pointer +
                         * the user-side write-back coordinates from
                         * OpContext. After we return ERR_WOULD_BLOCK
                         * + PROC_WAITING, the dispatcher will not touch
                         * the staged crates; obj_read_finish must call
                         * crate_stage_commit_and_release. */
                        async_ctx->crates_kbuf   = crates;
                        async_ctx->crates_uaddr  = ctx->crates_uaddr;
                        async_ctx->crate_count   = ctx->crate_count;

                        /* Never-drop completion node: set before the first
                         * submit so the AHCI IRQ can post it with no alloc. */
                        async_ctx->cq_node.run   = obj_read_pump;
                        async_ctx->cq_node.ctx   = async_ctx;

                        /* Park BEFORE the first submit — if the IRQ
                         * fires before we set WAITING, KResultPush's
                         * "if state==WAITING flip to WORKING" is a
                         * no-op and the wake is lost. Setting WAITING
                         * first makes that transition reliable. */
                        process_set_state(ctx->proc, PROC_WAITING);
                        /* Signal ownership transfer of the staged crates
                         * kbuf to the async_ctx. Dispatcher reads this
                         * flag (instead of process state) to decide
                         * whether to skip sync cleanup — race-safe vs
                         * a fast AHCI completion that would have flipped
                         * state back to PROC_WORKING. */
                        if (ctx->async_owns_crates) *ctx->async_owns_crates = true;
                        obj_read_step(async_ctx);
                        return ERR_WOULD_BLOCK;
                    }
                    kfree(async_ctx);
                }
                pmm_free(dma_phys, 1);
            }
            /* Allocation failure — fall through to sync. */
        }
    }

    /* Sync fallback (no AHCI, no proc context, alloc failure, or empty
     * read). Same bounce path — we already allocated kp above. */
    int got = tagfs_read(handle, kp, out->capacity);
    tagfs_close(handle);

    error_t  sync_err   = OK;
    uint64_t sync_bytes = 0;
    if (got < 0) {
        sync_err  = ERR_IO;
        out->size = 0;
    } else if (got > 0) {
        int crc = crate_out_commit(out, ctx, kp, (uint64_t)got);
        if (crc != OK) {
            sync_err  = (error_t)crc;
            out->size = 0;
        } else {
            sync_bytes = (uint64_t)got;
            out->size  = (uint64_t)got;
        }
    } else {
        out->size = 0;   /* EOF read: 0 bytes, success */
    }
    crate_buf_free(kp);

    /* Ф26e: a ferry read that fell through to sync must still be answered on
     * the KCTX_STORAGE channel or its awaiter hangs (single-core / EOF-read). */
    if (waybill != 0)
        return storage_sync_ferry_finalize(ctx, crates, sync_err, sync_bytes, waybill);
    return (sync_err == OK) ? OK : (int)sync_err;
}

/* -------------------------------------------------------------------------
 * OBJ_WRITE — write in_crate.size bytes to file at offset.
 * If out_crate is provided and capacity >= 16, write back stats.
 * ------------------------------------------------------------------------- */

static int ObjWrite(const ManifestOp *op,
                    Crate            *crates,
                    uint16_t          crate_count,
                    const OpContext  *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 16)              return ERR_INVALID_ARGUMENT;

    uint32_t file_id = param_u32(op, 0);
    uint64_t offset  = param_u64(op, 4);
    uint32_t flags   = param_u32(op, 12);
    /* Ф26e: optional trailing correlation waybill (box::ferry). Absent for a
     * synchronous write (param_size 16) → 0 → plain KCTX_GUIDE completion. */
    uint64_t waybill = (op->param_size >= 24) ? param_u64(op, 16) : 0;

    Crate *src = &crates[op->in_crate];
    if (src->size == 0)
        return storage_op_fail(ctx, crates, ERR_INVALID_ARGUMENT, waybill);

    /* Bounce-buffer the input crate. Multi-page user buffers are common
     * (any write over 4 KiB) and vmm_translate_user_addr cannot span
     * pages, so we copy through a kmalloc'd kernel buffer. The async
     * path takes ownership of src_bounce on a successful submit and
     * frees it from W_DONE; otherwise we free here. */
    void *src_bounce = crate_in_buf(src, ctx);
    if (!src_bounce) return storage_op_fail(ctx, crates, ERR_INVALID_ADDRESS, waybill);

    /* Optional output stats crate [u64 bytes_written][u64 new_file_size].
     * Written via crate_write (sync) / vmm_user_buf_commit_out (async) —
     * both page-walk, so a stats crate that straddles a page is safe. */
    Crate *out_crate = NULL;
    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *o = &crates[op->out_crate];
        if (o->capacity >= 16) out_crate = o;
    }

    /* Async write requires a K-Core to pump its IRQ-deferred completion
     * continuations (write completion does heavy work — alloc / DiskBook /
     * CoW — that cannot run in IRQ context, so it is posted to the never-drop
     * storage completion queue and drained by kcore_run_loop). That loop only exists in multi-core
     * mode; on a single core the BSP runs userspace and never pumps, so the
     * job would strand. Use the sync path there — it is correct and has no
     * benefit to lose (one core does everything regardless). */
    if (tagfs_volume_is_ahci() && ctx && ctx->proc && g_amp.total_cores > 1) {
        int rc = ObjWriteAsync(file_id, offset, flags,
                               src_bounce, (uint32_t)src->size,
                               out_crate, ctx,
                               crates, ctx->crate_count, ctx->crates_uaddr,
                               waybill);
        if (rc == ERR_WOULD_BLOCK) {
            /* Async owns src_bounce AND the staged Crate[] kbuf now —
             * wjob_finalize frees both at W_DONE. */
            return ERR_WOULD_BLOCK;
        }
        /* Hard failure before submission — free + fall through. */
    }

    /* Sync fallback. */
    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    if (!handle) {
        crate_buf_free(src_bounce);
        return storage_op_fail(ctx, crates, ERR_FILE_NOT_FOUND, waybill);
    }

    if (flags & OBJ_WRITE_APPEND_FLAG) {
        handle->offset = handle->file_size;
    } else {
        handle->offset = offset;
    }

    int wrote = tagfs_write(handle, src_bounce, src->size);
    uint64_t final_size = handle->file_size;
    uint64_t start_offset = (flags & OBJ_WRITE_APPEND_FLAG)
                                ? (final_size - (uint64_t)(wrote > 0 ? wrote : 0))
                                : offset;
    tagfs_close(handle);
    crate_buf_free(src_bounce);

    if (wrote < 0) {
        if (waybill != 0)
            return storage_sync_ferry_finalize(ctx, crates, ERR_IO, 0, waybill);
        return ERR_IO;
    }

    /* WROTE fan-out — same 32-byte payload as write_job.c::w_publish so
     * a tag listener (write_observer, etc.) gets identical layout on
     * sync (BIOS / single-core) and async (UEFI + AHCI + multi-core)
     * paths. Drift between the two surfaced as
     *   "[WO] FAIL: payload too small (8)"
     * on every sync-path config. */
    if (wrote > 0 && TouchHasAnyListeners()) {
        TagFSMetadata wmeta;
        memset(&wmeta, 0, sizeof(wmeta));
        if (tagfs_get_metadata(file_id, &wmeta) == OK) {
            struct {
                uint32_t file_id;
                uint8_t  op;          /* 1 = WRITE */
                uint8_t  _pad[3];
                uint64_t offset;
                uint64_t bytes;
                uint64_t final_size;
            } __attribute__((packed)) ev = {
                .file_id    = file_id,
                .op         = 1,
                .offset     = start_offset,
                .bytes      = (uint64_t)wrote,
                .final_size = final_size,
            };
            uint32_t pid = (ctx && ctx->proc) ? ctx->proc->pid : 0;
            for (uint16_t ti = 0; ti < wmeta.tag_count; ti++) {
                TouchPublishId(wmeta.tag_ids[ti], &ev, sizeof(ev),
                               pid, TOUCH_FLAG_TAGFS);
            }
            tagfs_metadata_free(&wmeta);
        }
    }

    if (out_crate) {
        uint8_t stats[16];
        uint64_t bytes_written = (uint64_t)wrote;
        memcpy(stats + 0, &bytes_written, sizeof(uint64_t));
        memcpy(stats + 8, &final_size,    sizeof(uint64_t));
        /* Best-effort: a commit failure (user unmapped the page mid-op)
         * leaves the stats crate empty but does not fail the completed
         * write — crate_write sets out_crate->size only on success. */
        (void)crate_write(out_crate, ctx, stats, 16);
    }

    /* Ф26e: answer a ferry write on the KCTX_STORAGE channel — the async
     * path was declined (single core / alloc-fail) and fell through here. */
    if (waybill != 0)
        return storage_sync_ferry_finalize(ctx, crates, OK, (uint64_t)wrote, waybill);
    return OK;
}

/* -------------------------------------------------------------------------
 * OBJ_DELETE — refuse to delete files with system/boot tags.
 * ------------------------------------------------------------------------- */

static int ObjDelete(const ManifestOp *op,
                     Crate            *crates,
                     uint16_t          crate_count,
                     const OpContext  *ctx)
{
    (void)crates;
    (void)crate_count;
    (void)ctx;
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;

    uint32_t file_id = param_u32(op, 0);

    TagFSMetadata md;
    TagFSState   *state = tagfs_get_state();
    if (tagfs_get_metadata(file_id, &md) == 0 && (md.flags & TAGFS_FILE_ACTIVE)) {
        for (uint16_t i = 0; i < md.tag_count; i++) {
            const char *key = state ? tag_registry_key(state->registry, md.tag_ids[i]) : NULL;
            if (key && (strcmp(key, "system") == 0 || strcmp(key, "boot") == 0)) {
                tagfs_metadata_free(&md);
                return ERR_PERMISSION_DENIED;
            }
        }
        tagfs_metadata_free(&md);
    }

    int rc = tagfs_delete_file(file_id);
    return (rc == 0) ? OK : ERR_FILE_NOT_FOUND;
}

/* -------------------------------------------------------------------------
 * OBJ_TRUNCATE — params = [u32 file_id][u64 new_size]. Shrink only.
 *
 * Guarded like OBJ_DELETE: cutting a "system" or "boot" file down to nothing
 * destroys it just as thoroughly as deleting it, so the same tags refuse the
 * same way.
 *
 * Publishes the TRUNCATED fan-out on every tag of the file, in the same
 * 32-byte payload shape ObjWrite uses so one observer can read both:
 *   op = 3, offset = the cut point (= the new length),
 *   bytes = how many bytes were discarded, final_size = the new length.
 * A content change that published nothing would be a hole in the Touch spine
 * — an observer would have to poll to notice, which is the thing Touch exists
 * to remove.
 * ------------------------------------------------------------------------- */

static int ObjTruncate(const ManifestOp *op,
                       Crate            *crates,
                       uint16_t          crate_count,
                       const OpContext  *ctx)
{
    (void)crates;
    (void)crate_count;
    if (op->param_size < 12) return ERR_INVALID_ARGUMENT;

    uint32_t file_id  = param_u32(op, 0);
    uint64_t new_size = param_u64(op, 4);

    TagFSMetadata md;
    TagFSState   *state    = tagfs_get_state();
    uint64_t      old_size = 0;
    bool          have_md  = false;
    if (tagfs_get_metadata(file_id, &md) == 0) {
        for (uint16_t i = 0; i < md.tag_count; i++) {
            const char *key = state ? tag_registry_key(state->registry, md.tag_ids[i]) : NULL;
            if (key && (strcmp(key, "system") == 0 || strcmp(key, "boot") == 0)) {
                tagfs_metadata_free(&md);
                return ERR_PERMISSION_DENIED;
            }
        }
        old_size = md.size;
        have_md  = true;
    }

    int rc = tagfs_truncate_file(file_id, new_size);
    if (rc != 0) {
        if (have_md) tagfs_metadata_free(&md);
        return (error_t)(-rc);
    }

    if (have_md) {
        if (old_size > new_size && TouchHasAnyListeners()) {
            struct {
                uint32_t file_id;
                uint8_t  op;          /* 3 = TRUNCATE */
                uint8_t  _pad[3];
                uint64_t offset;
                uint64_t bytes;
                uint64_t final_size;
            } __attribute__((packed)) ev = {
                .file_id    = file_id,
                .op         = 3,
                .offset     = new_size,
                .bytes      = old_size - new_size,
                .final_size = new_size,
            };
            uint32_t pid = (ctx && ctx->proc) ? ctx->proc->pid : 0;
            for (uint16_t ti = 0; ti < md.tag_count; ti++) {
                TouchPublishId(md.tag_ids[ti], &ev, sizeof(ev), pid, TOUCH_FLAG_TAGFS);
            }
        }
        tagfs_metadata_free(&md);
    }
    return OK;
}

/* -------------------------------------------------------------------------
 * OBJ_RENAME — params carry the new name inline.
 * ------------------------------------------------------------------------- */

static int ObjRename(const ManifestOp *op,
                     Crate            *crates,
                     uint16_t          crate_count,
                     const OpContext  *ctx)
{
    (void)crates;
    (void)crate_count;
    (void)ctx;
    if (op->param_size < 6) return ERR_INVALID_ARGUMENT;

    uint32_t file_id      = param_u32(op, 0);
    uint16_t new_name_len = param_u16(op, 4);
    if (new_name_len == 0)                          return ERR_INVALID_ARGUMENT;
    if (op->param_size < 6u + (uint32_t)new_name_len) return ERR_INVALID_ARGUMENT;

    char name[64];
    if (new_name_len >= sizeof(name)) return ERR_BUFFER_TOO_SMALL;
    memcpy(name, op->params + 6, new_name_len);
    name[new_name_len] = '\0';

    int rc = tagfs_rename_file(file_id, name);
    return (rc == 0) ? OK : ERR_INVALID_ARGUMENT;
}

/* -------------------------------------------------------------------------
 * OBJ_GET_INFO — serialize file metadata into out_crate.
 *
 * Layout written into out_crate (little-endian):
 *   [u32 file_id]
 *   [u32 flags]
 *   [u64 size]
 *   [u16 tag_count]
 *   [u16 filename_len]
 *   [char filename[filename_len]]
 *   for each tag:
 *     [u16 key_len][u16 val_len][u8 type][char key[key_len]][char val[val_len]]
 *       type: 1 = system (reserved-vocabulary tag), 0 = user.
 *
 * out_crate.size is set to the total bytes written. No truncation: if the
 * crate is too small, we return ERR_BUFFER_TOO_SMALL with size unchanged.
 * ------------------------------------------------------------------------- */

static int ObjGetInfo(const ManifestOp *op,
                      Crate            *crates,
                      uint16_t          crate_count,
                      const OpContext  *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 4)                return ERR_INVALID_ARGUMENT;

    uint32_t file_id = param_u32(op, 0);
    Crate   *out     = &crates[op->out_crate];

    TagFSMetadata md;
    if (tagfs_get_metadata(file_id, &md) != 0 || !(md.flags & TAGFS_FILE_ACTIVE)) {
        return ERR_FILE_NOT_FOUND;
    }

    uint16_t filename_len = 0;
    if (md.filename) {
        size_t l = strlen(md.filename);
        if (l > 0xFFFFu) l = 0xFFFFu;
        filename_len = (uint16_t)l;
    }

    /* Compute required size up front so we can fail fast on too-small crate. */
    TagFSState *state = tagfs_get_state();
    uint64_t need = 4u + 4u + 8u + 2u + 2u + filename_len;
    for (uint16_t i = 0; i < md.tag_count; i++) {
        const char *k = state ? tag_registry_key  (state->registry, md.tag_ids[i]) : NULL;
        const char *v = state ? tag_registry_value(state->registry, md.tag_ids[i]) : NULL;
        uint16_t kl = k ? (uint16_t)strlen(k) : 0;
        uint16_t vl = v ? (uint16_t)strlen(v) : 0;
        need += 2u + 2u + 1u + kl + vl;   /* +1: per-tag type byte */
    }

    if (need > out->capacity) {
        tagfs_metadata_free(&md);
        return ERR_BUFFER_TOO_SMALL;
    }

    uint8_t *kp = crate_out_alloc(out, need);
    if (!kp) { tagfs_metadata_free(&md); return ERR_INVALID_ADDRESS; }

    uint64_t pos = 0;
    memcpy(kp + pos, &md.file_id, 4); pos += 4;
    memcpy(kp + pos, &md.flags,   4); pos += 4;
    memcpy(kp + pos, &md.size,    8); pos += 8;
    memcpy(kp + pos, &md.tag_count, 2); pos += 2;
    memcpy(kp + pos, &filename_len, 2); pos += 2;
    if (filename_len) { memcpy(kp + pos, md.filename, filename_len); pos += filename_len; }

    for (uint16_t i = 0; i < md.tag_count; i++) {
        const char *k = state ? tag_registry_key  (state->registry, md.tag_ids[i]) : NULL;
        const char *v = state ? tag_registry_value(state->registry, md.tag_ids[i]) : NULL;
        uint16_t kl = k ? (uint16_t)strlen(k) : 0;
        uint16_t vl = v ? (uint16_t)strlen(v) : 0;
        memcpy(kp + pos, &kl, 2); pos += 2;
        memcpy(kp + pos, &vl, 2); pos += 2;
        kp[pos++] = (state && tag_registry_is_system(state->registry, md.tag_ids[i])) ? 1 : 0;
        if (kl) { memcpy(kp + pos, k, kl); pos += kl; }
        if (vl) { memcpy(kp + pos, v, vl); pos += vl; }
    }

    int crc = crate_out_commit(out, ctx, kp, pos);
    crate_buf_free(kp);
    tagfs_metadata_free(&md);
    if (crc != OK) return crc;   /* fail closed: leave out->size unset */
    out->size = pos;
    return OK;
}

/* =========================================================================
 *  Tag / Query / Context / Create — Phase 12 additions
 *
 *  All tag-string parsing is done with a small static helper that tolerates
 *  empty input. The auto-context tags are merged in for query and create
 *  exactly as the legacy handler did, keyed on ctx->proc->pid.
 * ========================================================================= */

static int parse_tag_list(const char *src, size_t src_len,
                          const char *tags[], char buffer[][32],
                          uint32_t max_tags)
{
    if (!src || src_len == 0) return 0;

    uint32_t count = 0;
    size_t pos = 0;
    while (pos < src_len && count < max_tags) {
        size_t len = 0;
        while (pos + len < src_len && src[pos + len] != ',' && src[pos + len] != '\0' && len < 31) {
            len++;
        }
        if (len > 0) {
            memcpy(buffer[count], src + pos, len);
            buffer[count][len] = '\0';
            tags[count] = buffer[count];
            count++;
        }
        pos += len;
        while (pos < src_len && src[pos] != ',' && src[pos] != '\0') pos++;
        if (pos < src_len && src[pos] == ',') pos++;
        if (pos < src_len && src[pos] == '\0') break;
    }
    return (int)count;
}

/* STORAGE_TAG_QUERY  in_crate (optional): comma-separated tag string
 *                    out_crate: [u32 count][u32 file_ids[count]]
 *                    Auto-context tags from ctx->proc->pid are merged. */
static int ObjQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                    const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 4) return ERR_BUFFER_TOO_SMALL;

    TagFSState *state = tagfs_get_state();
    if (!state || !state->initialized) return ERR_NOT_INITIALIZED;

    /* Query string from in_crate, if any. Read it BEFORE allocating the
     * output bounce buffer so an early return here cannot leak the kbuf. */
    char    qbuf[256];
    size_t  qlen = 0;
    if (op->in_crate != CRATE_INDEX_NONE) {
        Crate *src = &crates[op->in_crate];
        if (src->size > 0 && src->size < sizeof(qbuf)) {
            if (crate_read(src, ctx, qbuf, src->size) != OK) return ERR_INVALID_ADDRESS;
            qlen = src->size;
            qbuf[qlen] = '\0';
        }
    }

    uint8_t *kp = crate_out_alloc(out, out->capacity);
    if (!kp) return ERR_INVALID_ADDRESS;

    uint32_t max_results = (uint32_t)((out->capacity - 4) / sizeof(uint32_t));
    uint32_t *file_ids = (uint32_t *)(kp + 4);

    const char *all_tags[32];
    char        tag_storage[32][32];
    int total = parse_tag_list(qbuf, qlen, all_tags, tag_storage, 16);

    /* Merge auto-context tags. */
    if (ctx && ctx->proc) {
        const char *ctx_tags[16];
        int ccount = tagfs_context_get_tags(ctx->proc->pid, ctx_tags, 16);
        for (int i = 0; i < ccount && total < 32; i++) {
            size_t l = strlen(ctx_tags[i]);
            if (l > 31) l = 31;
            memcpy(tag_storage[total], ctx_tags[i], l);
            tag_storage[total][l] = '\0';
            all_tags[total] = tag_storage[total];
            total++;
        }
    }

    int count;
    if (total > 0) {
        count = tagfs_query_files(all_tags, (uint32_t)total, file_ids, max_results);
        if (count < 0) count = 0;
    } else {
        count = tagfs_list_all_files(file_ids, max_results);
        if (count < 0) count = 0;
    }

    uint32_t cnt32 = (uint32_t)count;
    memcpy(kp, &cnt32, sizeof(uint32_t));
    uint64_t out_bytes = 4 + (uint64_t)cnt32 * sizeof(uint32_t);
    int crc = crate_out_commit(out, ctx, kp, out_bytes);
    crate_buf_free(kp);
    if (crc != OK) return crc;   /* fail closed: leave out->size unset */
    out->size = out_bytes;
    return OK;
}

/* STORAGE_TAG_SET  params:[u32 file_id]  in_crate: "key" or "key:value" */
static int ObjTagSet(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 4)               return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint32_t file_id = param_u32(op, 0);
    Crate   *src     = &crates[op->in_crate];
    if (src->size == 0 || src->size > 127) return ERR_INVALID_ARGUMENT;

    char tag[128];
    if (crate_read(src, ctx, tag, src->size) != OK) return ERR_INVALID_ADDRESS;
    tag[src->size] = '\0';

    char key[64], value[64];
    if (tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value)) != 0) {
        return ERR_INVALID_ARGUMENT;
    }
    if (tagfs_add_tag_string(file_id, key, value[0] ? value : NULL) != 0) {
        return ERR_INVALID_ARGUMENT;
    }
    return OK;
}

/* STORAGE_TAG_UNSET  params:[u32 file_id]  in_crate: tag key */
static int ObjTagUnset(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 4)               return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint32_t file_id = param_u32(op, 0);
    Crate   *src     = &crates[op->in_crate];
    if (src->size == 0 || src->size > 63) return ERR_INVALID_ARGUMENT;

    char key[64];
    if (crate_read(src, ctx, key, src->size) != OK) return ERR_INVALID_ADDRESS;
    key[src->size] = '\0';

    if (tagfs_remove_tag_string(file_id, key) != 0) {
        return ERR_INVALID_ARGUMENT;
    }
    return OK;
}

/* STORAGE_OBJ_CREATE  params:[char filename[32]] (NUL-padded)
 *                     in_crate (optional): tag string
 *                     out_crate (optional): u32 file_id */
static int ObjCreate(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;

    char filename[33];
    size_t fn_copy = op->param_size < 32 ? op->param_size : 32;
    memcpy(filename, op->params, fn_copy);
    filename[fn_copy] = '\0';
    /* trim at first NUL within the 32-byte field */
    for (size_t i = 0; i < fn_copy; i++) {
        if (filename[i] == '\0') { filename[i] = '\0'; break; }
    }
    if (filename[0] == '\0') return ERR_INVALID_ARGUMENT;

    /* Parse tag list (in_crate) + auto-context tags. */
    char    tag_buf[256];
    size_t  tlen = 0;
    if (op->in_crate != CRATE_INDEX_NONE) {
        Crate *src = &crates[op->in_crate];
        if (src->size > 0 && src->size < sizeof(tag_buf)) {
            if (crate_read(src, ctx, tag_buf, src->size) != OK) return ERR_INVALID_ADDRESS;
            tlen = src->size;
            tag_buf[tlen] = '\0';
        }
    }

    const char *tag_ptrs[16];
    char        tag_storage[16][32];
    int tag_count = parse_tag_list(tag_buf, tlen, tag_ptrs, tag_storage, 16);
    if (tag_count < 0) tag_count = 0;

    /* Merge auto-context tags. */
    if (ctx && ctx->proc) {
        const char *ctx_tags[16];
        int ccount = tagfs_context_get_tags(ctx->proc->pid, ctx_tags, 16);
        for (int i = 0; i < ccount && tag_count < 16; i++) {
            size_t l = strlen(ctx_tags[i]);
            if (l > 31) l = 31;
            memcpy(tag_storage[tag_count], ctx_tags[i], l);
            tag_storage[tag_count][l] = '\0';
            tag_ptrs[tag_count] = tag_storage[tag_count];
            tag_count++;
        }
    }

    /* Intern tags into the registry. */
    TagFSState *tfs_state = tagfs_get_state();
    uint16_t tag_id_buf[16];
    uint16_t intern_count = 0;
    if (tfs_state && tfs_state->registry) {
        for (int i = 0; i < tag_count && intern_count < 16; i++) {
            char k[64], v[64];
            if (tagfs_parse_tag(tag_ptrs[i], k, sizeof(k), v, sizeof(v)) != 0) continue;
            uint16_t tid = tag_registry_intern(tfs_state->registry, k,
                                               v[0] ? v : NULL);
            if (tid != TAGFS_INVALID_TAG_ID) {
                tag_id_buf[intern_count++] = tid;
            }
        }
    }

    uint32_t file_id = 0;
    int rc = tagfs_create_file(filename,
                               intern_count > 0 ? tag_id_buf : NULL,
                               intern_count, &file_id);
    if (rc != 0) return ERR_DISK_FULL;

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(uint32_t)) {
            (void)crate_write(out, ctx, &file_id, sizeof(uint32_t));
        }
    }
    return OK;
}

/* STORAGE_CONTEXT_SET  in_crate: tag string ("key" or "key:value") */
static int ObjContextSet(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)               return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *src = &crates[op->in_crate];
    if (src->size == 0 || src->size > 127) return ERR_INVALID_ARGUMENT;

    char tag[128];
    if (crate_read(src, ctx, tag, src->size) != OK) return ERR_INVALID_ADDRESS;
    tag[src->size] = '\0';

    char key[64], value[64];
    const char *colon = strchr(tag, ':');
    if (colon) {
        size_t klen = (size_t)(colon - tag);
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        memcpy(key, tag, klen);
        key[klen] = '\0';
        strncpy(value, colon + 1, sizeof(value) - 1);
        value[sizeof(value) - 1] = '\0';
    } else {
        strncpy(key, tag, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
        value[0] = '\0';
    }

    if (tagfs_context_add_tag_string(ctx->proc->pid, key, value[0] ? value : NULL) != 0) {
        return ERR_INVALID_ARGUMENT;
    }
    return OK;
}

/* STORAGE_CONTEXT_CLEAR  no params, no crates */
static int ObjContextClear(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    tagfs_context_clear(ctx->proc->pid);
    return OK;
}

/* STORAGE_CONTEXT_GET  no params
 *                      out_crate: [u32 count][ (u16 len)(char tag[len]) ]*
 *
 * The companion to CONTEXT_SET / CONTEXT_CLEAR — reports the calling
 * process's current context tags ("key" or "key:value"), so a caller can
 * snapshot the context, install its own, and restore the original on exit
 * (correct nesting). tagfs_context_get_tags already formats each tag; we
 * just length-prefix them into the crate. Tags that would overflow the
 * caller's buffer are dropped (count reflects what was actually written). */
static int ObjContextGet(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)op; (void)crate_count;
    if (!ctx || !ctx->proc)               return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 4) return ERR_BUFFER_TOO_SMALL;

    uint8_t *kp = crate_out_alloc(out, out->capacity);
    if (!kp) return ERR_INVALID_ADDRESS;

    /* tagfs_context_get_tags returns pointers into a static per-slot buffer
     * held only for the duration of the call — copy each out immediately. */
    const char *ctx_tags[64];
    int ccount = tagfs_context_get_tags(ctx->proc->pid, ctx_tags, 64);
    if (ccount < 0) ccount = 0;

    uint64_t pos     = 4;  /* reserve the leading count */
    uint32_t written = 0;
    for (int i = 0; i < ccount; i++) {
        size_t l = strlen(ctx_tags[i]);
        if (l > 0xFFFFu) l = 0xFFFFu;
        if (pos + 2u + l > out->capacity) break;  /* no room — stop, report partial */
        uint16_t l16 = (uint16_t)l;
        memcpy(kp + pos, &l16, 2);          pos += 2;
        memcpy(kp + pos, ctx_tags[i], l);   pos += l;
        written++;
    }
    memcpy(kp, &written, 4);
    int crc = crate_out_commit(out, ctx, kp, pos);
    crate_buf_free(kp);
    if (crc != OK) return crc;   /* fail closed: leave out->size unset */
    out->size = pos;
    return OK;
}

/* -------------------------------------------------------------------------
 * Snapshot ops — userspace surface for CoW snapshots.
 *
 *   STORAGE_SNAP_CREATE  params: [u32 file_id][u8 name_len][char name[]]
 *                        out_crate: [u32 snapshot_id]
 *   STORAGE_SNAP_DELETE  params: [u32 snapshot_id]
 *   STORAGE_SNAP_LIST    out_crate: [u32 count][u32 ids[count]]
 * ------------------------------------------------------------------------- */

static int ObjSnapCreate(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count; (void)ctx;
    if (op->param_size < 5) return ERR_INVALID_ARGUMENT;

    uint32_t file_id = param_u32(op, 0);
    uint8_t name_len;
    memcpy(&name_len, op->params + 4, 1);
    if (name_len == 0 || name_len > 31) return ERR_INVALID_ARGUMENT;
    if (op->param_size < (uint16_t)(5 + name_len)) return ERR_INVALID_ARGUMENT;

    char name[32];
    memcpy(name, op->params + 5, name_len);
    name[name_len] = '\0';

    uint32_t snapshot_id = 0;
    error_t err = TagFS_SnapshotCreate(name, file_id, &snapshot_id);
    if (err != OK) return err;

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= 4) {
            (void)crate_write(out, ctx, &snapshot_id, sizeof(uint32_t));
        }
    }
    return OK;
}

static int ObjSnapDelete(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;
    uint32_t snapshot_id = param_u32(op, 0);
    return (int)TagFS_SnapshotDelete(snapshot_id);
}

static int ObjSnapList(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 4) return ERR_BUFFER_TOO_SMALL;

    uint32_t max_ids = (uint32_t)((out->capacity - 4) / 4);
    if (max_ids > 64) max_ids = 64;

    uint32_t ids[64];
    uint32_t count = 0;
    error_t err = TagFS_SnapshotList(ids, max_ids, &count);
    if (err != OK) return err;

    uint32_t bytes = 4 + count * 4;
    uint8_t blob[4 + 64 * 4];   /* count + up to max_ids (<= 64) ids */
    memcpy(blob, &count, 4);
    if (count > 0) memcpy(blob + 4, ids, count * 4);
    if (crate_write(out, ctx, blob, bytes) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* STORAGE_SNAP_INFO  params: [u32 snapshot_id]
 *                    out_crate: [u32 id][u32 parent_file_id][u64 created_time]
 *                               [u32 file_count][u64 total_size][u8 flags]
 *                               [char name[32]]  (61 bytes)
 *
 * The structured counterpart to SNAP_LIST (ids only) — carries the snapshot's
 * name so userspace can resolve a snapshot by name for deterministic cleanup.
 * The record is a fixed 61 bytes, so it ships through crate_write of a stack
 * blob rather than a capacity-sized bounce buffer. */
static int ObjSnapInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 4)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint32_t snapshot_id = param_u32(op, 0);

    CowSnapshot cs;
    error_t err = TagFS_SnapshotInfo(snapshot_id, &cs);
    if (err != OK) return (int)err;   /* ERR_SNAPSHOT_NOT_FOUND for an unknown id */

    uint32_t need = 4u + 4u + 8u + 4u + 8u + 1u + 32u;   /* 61 bytes */
    Crate   *out  = &crates[op->out_crate];
    if (out->capacity < need) return ERR_BUFFER_TOO_SMALL;

    uint8_t  blob[61];
    uint32_t pos = 0;
    memcpy(blob + pos, &cs.snapshot_id,    4); pos += 4;
    memcpy(blob + pos, &cs.parent_file_id, 4); pos += 4;
    memcpy(blob + pos, &cs.created_time,   8); pos += 8;
    memcpy(blob + pos, &cs.file_count,     4); pos += 4;
    memcpy(blob + pos, &cs.total_size,     8); pos += 8;
    blob[pos++] = cs.flags;
    memcpy(blob + pos, cs.name, 32); pos += 32;

    if (crate_write(out, ctx, blob, pos) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* -------------------------------------------------------------------------
 * STORAGE_OBJ_ANCHOR — durability primitive.
 *
 *   params: [u32 file_id]   (0 = anchor everything; non-zero = the file
 *                             whose tags get the Touch payload's fid)
 *
 * Blocking semantics for the caller — returns when meta_pool current
 * block, file_table dirty blocks, block-bitmap, and superblock are all
 * persisted to disk. Plus the AHCI cache flush is forced.
 *
 * Then publishes a Touch "anchor" event on every tag of file_id so any
 * REST/REACT subscriber observing that tag wakes up: *this file is now
 * durable*. No POSIX equivalent — fsync() returns silently; anchor()
 * fans out a tag-targeted notification, so OBSERVERS get to react too.
 * Combined with Touch's REST mode, an app can fire-and-forget many
 * fwrites and have a single observer await N "anchor" events when its
 * full transaction is durable.
 *
 *   Form A (blocking):  anchor(fid)             — POSIX-shaped sync.
 *   Form B (Touch obs): touch_claim("anchor"…); — async durability fan-out.
 * ------------------------------------------------------------------------- */
static int ObjAnchor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    uint32_t file_id = (op->param_size >= 4) ? param_u32(op, 0) : 0;

    /* Sync flush of all in-memory state — same path bye uses, but
     * inline so caller's fwrite-then-anchor is durable without a
     * shutdown. */
    tagfs_sync();
    tagfs_flush_cache();

    /* Touch fan-out. For file_id == 0 we publish on a special "anchor"
     * tag (registered well-known); for a specific fid, publish on every
     * tag attached to that file so any observer keyed by file's tag
     * wakes up. */
    struct __attribute__((packed)) {
        uint32_t file_id;
        uint8_t  op;          /* 2 = ANCHOR */
        uint8_t  _pad[3];
        uint64_t now_us;
    } ev = { file_id, 2, {0,0,0}, 0 };

    uint32_t pid = (ctx && ctx->proc) ? ctx->proc->pid : 0;

    if (file_id != 0) {
        TagFSMetadata wmeta;
        memset(&wmeta, 0, sizeof(wmeta));
        if (tagfs_get_metadata(file_id, &wmeta) == OK) {
            for (uint16_t ti = 0; ti < wmeta.tag_count; ti++) {
                TouchPublishId(wmeta.tag_ids[ti], &ev, sizeof(ev),
                               pid, TOUCH_FLAG_TAGFS);
            }
            tagfs_metadata_free(&wmeta);
        }
    }
    /* Always also publish on the bare "anchor" tag — generic listeners. */
    TouchPublish("anchor", &ev, sizeof(ev));
    return OK;
}

/* -------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------- */

error_t StorageDeckRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        /* Read-only / cooperative: app+ (any process can list and read). */
        { STORAGE_TAG_QUERY,    ObjQuery,        OP_AUTH_APP, "storage.query"    },
        { STORAGE_OBJ_READ,     ObjRead,         OP_AUTH_APP, "storage.read"     },
        { STORAGE_OBJ_GET_INFO, ObjGetInfo,      OP_AUTH_APP, "storage.getinfo"  },
        /* Mutating: app+ — TagFS context further restricts what's visible. */
        { STORAGE_TAG_SET,      ObjTagSet,       OP_AUTH_APP, "storage.tag.set"  },
        { STORAGE_TAG_UNSET,    ObjTagUnset,     OP_AUTH_APP, "storage.tag.unset"},
        { STORAGE_OBJ_WRITE,    ObjWrite,        OP_AUTH_APP, "storage.write"    },
        { STORAGE_OBJ_CREATE,   ObjCreate,       OP_AUTH_APP, "storage.create"   },
        { STORAGE_OBJ_DELETE,   ObjDelete,       OP_AUTH_APP, "storage.delete"   },
        { STORAGE_OBJ_TRUNCATE, ObjTruncate,     OP_AUTH_APP, "storage.truncate" },
        { STORAGE_OBJ_RENAME,   ObjRename,       OP_AUTH_APP, "storage.rename"   },
        /* Per-process context: app+. */
        { STORAGE_CONTEXT_SET,  ObjContextSet,   OP_AUTH_APP, "storage.ctx.set"  },
        { STORAGE_CONTEXT_CLEAR,ObjContextClear, OP_AUTH_APP, "storage.ctx.clear"},
        { STORAGE_CONTEXT_GET,  ObjContextGet,   OP_AUTH_APP, "storage.ctx.get"  },
        /* Snapshot management: app+. */
        { STORAGE_SNAP_CREATE,  ObjSnapCreate,   OP_AUTH_APP, "storage.snap.create"},
        { STORAGE_SNAP_DELETE,  ObjSnapDelete,   OP_AUTH_APP, "storage.snap.delete"},
        { STORAGE_SNAP_LIST,    ObjSnapList,     OP_AUTH_APP, "storage.snap.list"  },
        { STORAGE_SNAP_INFO,    ObjSnapInfo,     OP_AUTH_APP, "storage.snap.info"  },
        { STORAGE_OBJ_ANCHOR,   ObjAnchor,       OP_AUTH_APP, "storage.anchor"     },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_STORAGE, table[i].opcode),
                                        table[i].handler,
                                        table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[StorageDeck] register %s failed: %s\n",
                    table[i].name, ErrorString(rc));
            return rc;
        }
    }
    debug_printf("[StorageDeck] registered %zu ops (read/write unbounded)\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}
