/*
 * Storage Deck — Manifest-native handlers.
 *
 * Removes the 168/176-byte stack buffers that capped legacy OBJ_READ /
 * OBJ_WRITE at sub-200-byte payloads per syscall. Reads and writes now move
 * to / from a Crate of arbitrary capacity through the page-walked crate_io
 * primitives, so a payload that straddles a page boundary is copied across
 * every backing frame. A 1-MiB read fits in one syscall.
 *
 * Param layouts (little-endian, packed):
 *   OBJ_READ      params: [u32 file_id][u64 offset]
 *   OBJ_WRITE     params: [u32 file_id][u64 offset][u32 flags]
 *   OBJ_SPAN      params: [u32 file_id][u64 size]
 *   OBJ_DELETE    params: [u32 file_id]
 *   OBJ_RENAME    params: [u32 file_id][u16 new_name_len][char new_name[]]
 *   OBJ_GET_INFO  params: [u32 file_id]
 *
 *   OBJ_READ.out_crate     : payload destination; size <- bytes_read
 *   OBJ_WRITE.in_crate     : payload source, taken out of the program's pages
 *                            a block at a time as the write reaches it
 *   OBJ_WRITE.out_crate    : optional [u64 bytes_written][u64 new_file_size],
 *                            written on a refusal too: the bytes the file holds
 *   OBJ_GET_INFO.out_crate : serialized record, see ObjGetInfo() below
 */

#include "klib.h"
#include "chit.h"        /* ChitGive / ChitDue — an answer put off is an answer promised */
#include "crate_stage.h"
#include "crate_io.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "storage_deck.h"
#include "tagfs.h"
#include "use_context.h"   /* the user's tags — folded into query and create */
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "ahci.h"
#include "ahci_async.h"
#include "kring.h"
#include "kresult.h"
#include "write_job.h"
#include "baton.h"   /* BatonPass — never-drop read completion (Ф26 M2/M4) */
#include "ata.h"
#include "touch.h"
#include "cow.h"
#include "amp.h"   /* g_amp.total_cores — async write needs a K-Core to pump */
#include "boardroom.h"
#include "scribe.h"   /* VolumeFlushFor — an anchor's flush, unless one already covers it */
#include "plot.h"     /* PlotFind, PlotSource — the write's bytes where they are */
#include "settle.h"   /* TagFSSettle — what an anchor waits for */

/*
 * Can the medium under the volume answer a read without somebody standing over
 * it?
 *
 * This used to be "is there an AHCI controller", which was the same question as
 * "is the volume on it" only for as long as there was nowhere else a volume
 * could be. Then it became "is the volume on AHCI", which stopped reads going
 * to somebody else's disk but left the real answer unasked: a machine that
 * boots from a flash drive has its filesystem on the USB bus, answered no, and
 * spent a core standing over every block of every file it read — while the
 * same machine booted from SATA parked the caller and got on with something
 * else. The medium was never what made the difference.
 *
 * So the question is put to the seat, which is the one place that knows what is
 * under it.
 */
static inline bool tagfs_volume_can_read_async(void)
{
    return BoardroomSeatCanReadAsync(tagfs_get_seat());
}

/*
 * Writing is still AHCI's, and only AHCI's — honestly rather than by omission.
 *
 * A write here is not one command: it is a WriteJob that allocates blocks and
 * commits the file's record, and its state machine is built on AHCI slots and
 * the AHCI completion. Pointing it at another kind of
 * seat would need that machine rebuilt, not a different submit call. Until it
 * is, a volume on a flash drive takes the synchronous write path, which is
 * correct and says so.
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
 * Async fast-path: the seat under the volume takes a read nobody stands over
 * (BoardroomReadAsync), so the caller is parked (PROC_WAITING) and
 * ERR_WOULD_BLOCK returned. The seat's answer (obj_read_async_complete)
 * hands the heavy half to the drain core, which copies DMA → user buffer,
 * writes the actual byte count back into the Crate, KResultPush'es a Result
 * and ref_dec's the pinned process — KResultPush also flips PROC_WAITING →
 * PROC_WORKING so the caller wakes. A seat with every place taken does not
 * refuse the read: it waits in the context's berth for its turn.
 *
 * Fallback (a seat that only answers a caller who stays, allocation
 * failure, no proc context) takes the existing tagfs_read sync path.
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
    uint32_t         submit_cookie; /* the read submit's cloakroom token — echoed
                                     * into the plain (waybill==0) completion so
                                     * the caller's paired wait adopts its own. */
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
    Baton cq_node;

    /* Where each block's read waits when the seat has no place for it — the
     * seat's until its answer comes, then this read's again for the next
     * block. */
    RoadsteadBerth berth;
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
    error_t  answer   = (status == OK) ? OK : ERR_IO;

    /* The bytes reach the program's pages BEFORE anything says they did, and
     * a count is said only of bytes that got there. A commit that cannot
     * finish — a page the program does not own (ERR_INVALID_ADDRESS), or no
     * frame left to bring in one it does (ERR_NO_MEMORY) — is the read's
     * answer: its error and 0 bytes, the code crate_out_commit hands the
     * synchronous path for the same refusal. This used to go on and report
     * the whole count, and a read into a buffer the program had not been
     * through yet said every byte had arrived while the pages past the first
     * untouched one stayed zeros. */
    if (reported > 0) {
        error_t crc = (ctx->out_base && ctx->target && ctx->target->cabin)
                          ? vmm_user_buf_commit_out(ctx->target->cabin->vmm, ctx->out_user_addr,
                                                    ctx->out_base, (size_t)reported)
                          : ERR_INVALID_ADDRESS;
        if (crc != OK) {
            answer   = crc;
            reported = 0;
        }
    }
    ctx->out_crate->size = reported;
    r.error_code  = answer;
    r.data_length = (uint32_t)reported;
    r.sender_pid  = 0;
    /* Ф26e: a waybilled (box::ferry) read echoes its correlation token in
     * data_addr and flies the KCTX_STORAGE flag so boxlib routes it to the
     * ferry station and nothing else. A plain (waybill==0) read keeps
     * KCTX_GUIDE / data_addr==0 — byte-identical to the pre-ferry substrate. */
    if (ctx->waybill) { r.context = KCTX_STORAGE; r.data_addr = ctx->waybill; }
    else              { r.context = KCTX_PACK24(KCTX_GUIDE, ctx->submit_cookie); }

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

    /* The answer is determined; only its delivery remains, and that is the
     * kernel's own debt. Due here, kept by the push. */
    ChitDue(ctx->target, ctx->submit_cookie);
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
    BatonPass(&ctx->cq_node);   /* never-drop; heavy half on the K-Core pump */
}

/* Ф26 M2 — K-Core pump: the heavy half of a read completion, drained by the same
 * BatonPump loop that pumps write jobs (kcore_run_loop). Safe to take locks /
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
    /* The same question the write pump asks, for the same reason: this read
     * carries a handle full of block numbers from the mounting it started in,
     * and a volume that left and came back has been read again since. */
    if (!tagfs_handle_is_of_this_mount(ctx->handle)) {
        obj_read_finish(ctx, ERR_IO, /*partial_ok=*/false);
        return;
    }

    uint64_t file_pos   = ctx->start_offset + ctx->bytes_done;
    uint32_t off_in_blk = (uint32_t)(file_pos % TAGFS_BLOCK_SIZE);
    uint32_t disk_block = 0;

    if (!PlotFind(ctx->handle->extents, ctx->handle->extent_count,
                  file_pos / TAGFS_BLOCK_SIZE, &disk_block, NULL)) {
        /* Walked past last extent — finish with what we have. */
        obj_read_finish(ctx, OK, /*partial_ok=*/true);
        return;
    }

    uint64_t remaining = ctx->total_bytes - ctx->bytes_done;
    uint32_t chunk     = TAGFS_BLOCK_SIZE - off_in_blk;
    if ((uint64_t)chunk > remaining) chunk = (uint32_t)remaining;

    ctx->in_flight_off_in_blk = off_in_blk;
    ctx->in_flight_chunk      = chunk;

    uint64_t lba = tagfs_block_to_sector(disk_block);
    error_t  err = BoardroomReadAsync(tagfs_get_seat(), lba, 8, ctx->dma_phys,
                                      obj_read_async_complete, ctx, &ctx->berth);
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

    /* A ferry op is submitted without a token — nobody waits on a Result to
     * it — so this raises the async flag and leaves no chit. */
    ChitGive(ctx, "storage.ferry", waybill);
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
    if (!kp) return storage_op_fail(ctx, crates, ERR_NO_MEMORY, waybill);

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
        tagfs_volume_can_read_async() && ctx && ctx->proc && g_amp.total_cores > 1) {

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
                        async_ctx->submit_cookie = ctx->submit_cookie;
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
                        /* The chit, and with it the ownership transfer of
                         * the staged crates kbuf to the async_ctx (the flag
                         * the dispatcher reads instead of process state —
                         * race-safe vs a fast completion that flips state
                         * back to PROC_WORKING). Given BEFORE the first
                         * submit: the finish that keeps it may run on
                         * another core the moment the read is armed. */
                        ChitGive(ctx, "storage.read", file_id);
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
    uint64_t got  = 0;
    error_t  read = tagfs_read(handle, kp, out->capacity, &got);
    tagfs_close(handle);

    error_t  sync_err   = OK;
    uint64_t sync_bytes = 0;
    if (read != OK) {
        sync_err  = ERR_IO;
        out->size = 0;
    } else if (got > 0) {
        int crc = crate_out_commit(out, ctx, kp, got);
        if (crc != OK) {
            sync_err  = (error_t)crc;
            out->size = 0;
        } else {
            sync_bytes = got;
            out->size  = got;
        }
    } else {
        out->size = 0;   /* at or past the size: 0 bytes, success */
    }
    crate_buf_free(kp);

    /* Ф26e: a ferry read that fell through to sync must still be answered on
     * the KCTX_STORAGE channel or its awaiter hangs (single-core / EOF-read). */
    if (waybill != 0)
        return storage_sync_ferry_finalize(ctx, crates, sync_err, sync_bytes, waybill);
    return (sync_err == OK) ? OK : (int)sync_err;
}

/* A change to a file's bytes, said on every tag it wears — one 32-byte shape
 * for every road that changes them, so one listener reads them all: op 1 a
 * write (the bytes written at offset), op 3 a span (the bytes cut or added
 * from offset, the old or the new size, whichever is less). */
static void storage_publish_change(uint32_t file_id, uint8_t op, uint64_t offset,
                                   uint64_t bytes, uint64_t final_size, const OpContext *ctx)
{
    if (!TouchHasAnyListeners())
        return;
    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (tagfs_get_metadata(file_id, &meta) != OK)
        return;
    struct {
        uint32_t file_id;
        uint8_t  op;
        uint8_t  _pad[3];
        uint64_t offset;
        uint64_t bytes;
        uint64_t final_size;
    } __attribute__((packed)) ev = {
        .file_id    = file_id,
        .op         = op,
        .offset     = offset,
        .bytes      = bytes,
        .final_size = final_size,
    };
    uint32_t pid = (ctx && ctx->proc) ? ctx->proc->pid : 0;
    for (uint32_t ti = 0; ti < meta.tag_count; ti++)
        TouchPublishId(meta.tag_ids[ti], &ev, sizeof(ev), pid, TOUCH_FLAG_TAGFS);
    tagfs_metadata_free(&meta);
}

/* -------------------------------------------------------------------------
 * OBJ_WRITE — in_crate.size bytes into the file at offset, or at its size
 * with OBJ_WRITE_APPEND_FLAG. An offset past the size is refused
 * ERR_TAGFS_BEYOND_SIZE. The bytes are taken out of the program's pages a
 * block at a time as the write reaches them — a write is never copied whole
 * into the kernel heap, so its size is bounded by the volume, not the heap.
 * If out_crate is provided and capacity >= 16, the stats are written back on
 * every answer: the bytes the file now holds of the write, and its size.
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
    bool     append  = (flags & OBJ_WRITE_APPEND_FLAG) != 0;

    Crate *src = &crates[op->in_crate];
    if (src->size == 0 || src->size > src->capacity)
        return storage_op_fail(ctx, crates, ERR_INVALID_ARGUMENT, waybill);

    /* Where the bytes are: the program's pages, walked as the write reaches
     * them, or — for a kernel caller with no cabin — the crate's own address.
     * A page the program has not touched yet is brought in here, before any
     * lock of the volume is taken; a write that cannot have its pages is
     * refused with nothing of it written. */
    PlotSource source;
    memset(&source, 0, sizeof(source));
    source.bytes = src->size;
    if (ctx && ctx->proc && ctx->proc->cabin) {
        source.vmm  = ctx->proc->cabin->vmm;
        source.user = (uintptr_t)src->addr;
    } else {
        source.kernel = (const uint8_t *)(uintptr_t)src->addr;
    }
    error_t pages = PlotSourceBringIn(&source);
    if (pages != OK)
        return storage_op_fail(ctx, crates, pages, waybill);

    /* Optional output stats crate [u64 bytes_written][u64 new_file_size].
     * Written via crate_write (sync) / vmm_user_buf_commit_out (async) —
     * both page-walk, so a stats crate that straddles a page is safe. */
    Crate *out_crate = NULL;
    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *o = &crates[op->out_crate];
        if (o->capacity >= 16) out_crate = o;
    }

    /* Async write requires a K-Core to pump its IRQ-deferred completion
     * continuations (write completion does heavy work — the record's
     * commit — that cannot run in IRQ context, so it is posted to the never-drop
     * storage completion queue and drained by kcore_run_loop). That loop only exists in multi-core
     * mode; on a single core the BSP runs userspace and never pumps, so the
     * job would strand. Use the sync path there — it is correct and has no
     * benefit to lose (one core does everything regardless). */
    if (tagfs_volume_is_ahci() && ctx && ctx->proc && g_amp.total_cores > 1) {
        int rc = ObjWriteAsync(file_id, offset, append, &source, out_crate, ctx,
                               crates, ctx->crate_count, ctx->crates_uaddr, waybill);
        if (rc == ERR_WOULD_BLOCK) {
            /* The job owns the write AND the staged Crate[] kbuf now —
             * wjob_finalize answers and frees. */
            return ERR_WOULD_BLOCK;
        }
        /* Refused before the job took it — nothing of it moved; fall through. */
    }

    /* Sync fallback. */
    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE |
                                                  (append ? TAGFS_HANDLE_APPEND : 0));
    if (!handle)
        return storage_op_fail(ctx, crates, ERR_FILE_NOT_FOUND, waybill);
    handle->offset = offset;

    uint64_t wrote      = 0;
    error_t  rc         = tagfs_write(handle, &source, &wrote);
    uint64_t final_size = handle->file_size;
    uint64_t began_at   = handle->offset - wrote;
    tagfs_close(handle);

    /* A refusal part way leaves the bytes before it in the file: the listeners
     * hear of them and the stats count them, the same as on the async road. */
    if (wrote > 0)
        storage_publish_change(file_id, 1, began_at, wrote, final_size, ctx);

    if (out_crate) {
        uint8_t stats[16];
        memcpy(stats + 0, &wrote,      sizeof(uint64_t));
        memcpy(stats + 8, &final_size, sizeof(uint64_t));
        /* Best-effort: a commit failure (user unmapped the page mid-op)
         * leaves the stats crate empty but does not fail the write —
         * crate_write sets out_crate->size only on success. */
        (void)crate_write(out_crate, ctx, stats, 16);
    }

    /* Ф26e: answer a ferry write on the KCTX_STORAGE channel — the async
     * path was declined (single core / alloc-fail) and fell through here. */
    if (waybill != 0)
        return storage_sync_ferry_finalize(ctx, crates, rc, wrote, waybill);
    return (int)rc;
}

/* Whether the file wears a tag whose key is system or boot: a file the
 * machine starts from, which no program cuts, grows or deletes. */
static bool storage_file_is_guarded(const TagFSMetadata *md)
{
    for (uint32_t i = 0; i < md->tag_count; i++) {
        char key[128];
        if (!tagfs_tag_key(md->tag_ids[i], key, sizeof(key))) continue;
        if (strcmp(key, "system") == 0 || strcmp(key, "boot") == 0)
            return true;
    }
    return false;
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
    if (tagfs_get_metadata(file_id, &md) == 0 && (md.flags & TAGFS_FILE_ACTIVE)) {
        bool guarded = storage_file_is_guarded(&md);
        tagfs_metadata_free(&md);
        if (guarded)
            return ERR_PERMISSION_DENIED;
    }

    int rc = tagfs_delete_file(file_id);
    return (rc == 0) ? OK : ERR_FILE_NOT_FOUND;
}

/* -------------------------------------------------------------------------
 * OBJ_SPAN — params = [u32 file_id][u64 size]: the file made exactly `size`
 * bytes long (tagfs_span_file). Shorter cuts the tail; longer reads as zeros
 * from the old size on.
 *
 * Guarded like OBJ_DELETE: cutting a "system" or "boot" file down to nothing
 * destroys it just as thoroughly as deleting it, so the same tags refuse the
 * same way.
 *
 * Publishes the change on every tag of the file (storage_publish_change,
 * op 3): a content change that published nothing would be a hole in the Touch
 * spine — an observer would have to poll to notice, which is the thing Touch
 * exists to remove.
 * ------------------------------------------------------------------------- */

static int ObjSpan(const ManifestOp *op,
                   Crate            *crates,
                   uint16_t          crate_count,
                   const OpContext  *ctx)
{
    (void)crates;
    (void)crate_count;
    if (op->param_size < 12) return ERR_INVALID_ARGUMENT;

    uint32_t file_id = param_u32(op, 0);
    uint64_t size    = param_u64(op, 4);

    TagFSMetadata md;
    memset(&md, 0, sizeof(md));
    if (tagfs_get_metadata(file_id, &md) != 0 || !(md.flags & TAGFS_FILE_ACTIVE)) {
        tagfs_metadata_free(&md);
        return ERR_FILE_NOT_FOUND;
    }
    bool     guarded = storage_file_is_guarded(&md);
    uint64_t was     = md.size;
    tagfs_metadata_free(&md);
    if (guarded)
        return ERR_PERMISSION_DENIED;

    error_t rc = tagfs_span_file(file_id, size);
    if (rc != OK)
        return (int)rc;
    if (size != was)
        storage_publish_change(file_id, 3, size < was ? size : was,
                               size < was ? was - size : size - was, size, ctx);
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
 * Every tag goes out whole, however long: its key and value are copied
 * straight out of the volume's registry into the answer.
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

/* One tag's record: [u16 key_len][u16 val_len][u8 type][key][val]. */
static uint64_t info_tag_record(uint8_t *kp, uint64_t pos, uint16_t tag_id)
{
    TagSpelling tag       = { 0 };
    bool        is_system = false;
    /* A tag the volume no longer knows still takes its place in the list:
     * the count in the header says how many records follow, so skipping one
     * would leave the reader parsing the next tag's bytes as this one's. */
    (void)TagFSTagSpelling(tag_id, &tag, &is_system);
    uint16_t kl = (uint16_t)tag.key_len;
    uint16_t vl = (uint16_t)tag.value_len;
    if (kp) {
        memcpy(kp + pos, &kl, 2);
        memcpy(kp + pos + 2, &vl, 2);
        kp[pos + 4] = is_system ? 1 : 0;
        if (kl) memcpy(kp + pos + 5, tag.key, kl);
        if (vl) memcpy(kp + pos + 5 + kl, tag.value, vl);
    }
    return pos + 5u + kl + vl;
}

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

    /* Both passes read the tags as spans into the volume's registry, so both
     * are inside the door: nothing is copied into a buffer of this deck's
     * size, and the registry the second pass writes from is the one the first
     * measured — a tag's text never changes while it is mounted. */
    if (!tagfs_enter()) {
        tagfs_metadata_free(&md);
        return ERR_FILE_NOT_FOUND;
    }

    /* The answer's tag count is u16 on the wire; a record holds no more. */
    uint16_t tag_count = (uint16_t)md.tag_count;
    uint64_t need = 4u + 4u + 8u + 2u + 2u + filename_len;
    for (uint32_t i = 0; i < tag_count; i++)
        need = info_tag_record(NULL, need, md.tag_ids[i]);

    uint8_t *kp = NULL;
    int      rc = OK;
    if (need > out->capacity)
        rc = ERR_BUFFER_TOO_SMALL;
    else if (!(kp = crate_out_alloc(out, need)))
        rc = ERR_INVALID_ADDRESS;

    uint64_t pos = 0;
    if (rc == OK) {
        /* trashed and hidden are tags, not bits the kernel ever set in the
         * record — so the flags a program read said "not trashed" of every
         * file, and `erase <name>` refused every file as not trashed yet. The
         * record carries the bits; the tags decide them. */
        const WellKnownTags *wk    = tagfs_get_well_known_tags();
        uint32_t             flags = md.flags;
        for (uint32_t i = 0; i < tag_count; i++) {
            if (md.tag_ids[i] == wk->trashed) flags |= TAGFS_FILE_TRASHED;
            if (md.tag_ids[i] == wk->hidden)  flags |= TAGFS_FILE_HIDDEN;
        }

        memcpy(kp + pos, &md.file_id, 4); pos += 4;
        memcpy(kp + pos, &flags,      4); pos += 4;
        memcpy(kp + pos, &md.size,    8); pos += 8;
        memcpy(kp + pos, &tag_count,  2); pos += 2;
        memcpy(kp + pos, &filename_len, 2); pos += 2;
        if (filename_len) { memcpy(kp + pos, md.filename, filename_len); pos += filename_len; }
        for (uint32_t i = 0; i < tag_count; i++)
            pos = info_tag_record(kp, pos, md.tag_ids[i]);
    }
    tagfs_leave();
    tagfs_metadata_free(&md);
    if (rc != OK) return rc;

    int crc = crate_out_commit(out, ctx, kp, pos);
    crate_buf_free(kp);
    if (crc != OK) return crc;   /* fail closed: leave out->size unset */
    out->size = pos;
    return OK;
}

/* =========================================================================
 *  Tag / Query / Context / Create
 *
 *  A tag's text is read by the one spelling (tag_spelling.h), in place in the
 *  crate's copy, and a tag that does not spell refuses the call. The Use
 *  Context's tags are folded in for query and create when the scope is USE.
 * ========================================================================= */

/*
 * The scope byte of storage.query / storage.create (boxos_decks.h): inside the
 * Use Context unless the caller said everywhere. Absent means inside.
 */
static error_t storage_scope_of(const ManifestOp *op, size_t at, uint8_t *out_scope)
{
    uint8_t scope = STORAGE_SCOPE_USE;
    if (op->param_size > at) scope = op->params[at];
    if (scope != STORAGE_SCOPE_USE && scope != STORAGE_SCOPE_EVERYWHERE)
        return ERR_INVALID_ARGUMENT;
    *out_scope = scope;
    return OK;
}

/*
 * The tags a query asks for or a create stamps: the caller's, spelled in place
 * out of the `len` bytes at `text`, followed — when the scope is USE — by the
 * Use Context's. The caller's list is judged whole before anything is
 * allocated for it, and a tag that does not spell refuses the call: a query
 * never drops a tag it cannot read and answers for the rest, and a create
 * never makes a file without a tag it was asked for. So is a text that names
 * no tag at all — asking for every file is asking with no text.
 *
 * *out_tags is the caller's to kfree, and so is *out_block, the Use Context's
 * names the last spellings point into. Both NULL, and a count of 0, when there
 * is nothing to ask.
 */
static error_t storage_tags_spelled(uint8_t scope, const char *text, size_t len,
                                    TagSpelling **out_tags, char **out_block,
                                    uint32_t *out_count)
{
    *out_tags  = NULL;
    *out_block = NULL;
    *out_count = 0;

    uint32_t own = 0;
    error_t  rc  = TagListCount(text, len, &own);
    if (rc != OK)
        return rc;
    if (len > 0 && own == 0)
        return ERR_TAGFS_INVALID_TAG;

    char        *block = NULL;
    const char **names = NULL;
    uint32_t     use   = 0;
    if (scope == STORAGE_SCOPE_USE) {
        rc = UseContextTagArray(&block, &names, &use);
        if (rc != OK)
            return rc;
    }
    if (own + use == 0)
        return OK;

    TagSpelling *tags = kmalloc(sizeof(TagSpelling) * (own + use));
    if (!tags) {
        kfree(block);
        kfree(names);
        return ERR_NO_MEMORY;
    }
    size_t at = 0;
    for (uint32_t i = 0; i < own; i++)
        (void)TagListNext(text, len, &at, &tags[i]);   /* judged whole above */
    for (uint32_t i = 0; i < use && rc == OK; i++)
        rc = TagSpell(names[i], strlen(names[i]), &tags[own + i]);
    kfree(names);
    if (rc != OK) {
        kfree(tags);
        kfree(block);
        return rc;
    }

    *out_tags  = tags;
    *out_block = block;
    *out_count = own + use;
    return OK;
}

/* The in_crate's bytes copied in, or NULL with *out_len 0 when there is no
 * in_crate or it is empty. ERR_INVALID_ADDRESS when the copy failed. */
static error_t storage_crate_text(const ManifestOp *op, Crate *crates, const OpContext *ctx,
                                  char **out_text, size_t *out_len)
{
    *out_text = NULL;
    *out_len  = 0;
    if (op->in_crate == CRATE_INDEX_NONE || crates[op->in_crate].size == 0)
        return OK;
    *out_text = crate_in_buf(&crates[op->in_crate], ctx);
    if (!*out_text)
        return ERR_INVALID_ADDRESS;
    *out_len = crates[op->in_crate].size;
    return OK;
}

/* The ids a query answer moves from the index to the caller's crate at a time. */
#define STORAGE_QUERY_PIECE 512u

/*
 * The files `tags` match (every file, for none), written into the out crate
 * after its 8-byte header a piece at a time, straight from the index's pages:
 * the kernel holds one piece and never the whole answer, and the crate is
 * given as many ids as it has room for. hdr[0] is how many went in, hdr[1]
 * how many match — counted by the first page, and never less than what went
 * in, since a file made while the pages are taken is delivered too.
 */
static error_t storage_query_into(const TagSpelling *tags, uint32_t count, const Crate *out,
                                  const OpContext *ctx, uint32_t hdr[2])
{
    uint64_t room_ids = (out->capacity - 8) / sizeof(uint32_t);
    uint32_t most     = room_ids > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)room_ids;
    uint32_t *piece   = kmalloc(STORAGE_QUERY_PIECE * sizeof(uint32_t));
    if (!piece) return ERR_NO_MEMORY;

    error_t  rc    = OK;
    uint32_t from  = 1;
    bool     first = true;
    for (;;) {
        uint32_t  room = most - hdr[0] < STORAGE_QUERY_PIECE ? most - hdr[0] : STORAGE_QUERY_PIECE;
        TagFSPage page;
        rc = TagFSQueryPage(tags, count, from, piece, room, first, &page);
        if (rc != OK) break;
        if (first) hdr[1] = page.total;
        first = false;
        if (page.delivered > 0) {
            int wrc = crate_out_commit_at(out, ctx, 8 + (uint64_t)hdr[0] * sizeof(uint32_t),
                                          piece, (uint64_t)page.delivered * sizeof(uint32_t));
            if (wrc != OK) {
                rc = (error_t)wrc;
                break;
            }
            hdr[0] += page.delivered;
        }
        if (page.next == 0 || hdr[0] == most) break;
        from = page.next;
    }
    kfree(piece);
    if (hdr[1] < hdr[0]) hdr[1] = hdr[0];
    return rc;
}

/* STORAGE_TAG_QUERY  params (optional): [u8 scope] — inside the Use Context
 *                    (absent, STORAGE_SCOPE_USE) or STORAGE_SCOPE_EVERYWHERE
 *                    in_crate (optional): comma-separated tag string
 *                    out_crate: [u32 delivered][u32 total][u32 file_ids[delivered]]
 *                    `total` is how many files match; `delivered` is how many
 *                    of them the crate had room for. A caller whose crate was
 *                    too small is told the true count and can ask again with
 *                    room for it. It used to be told the smaller number as if
 *                    that were the whole answer, and nothing said otherwise.
 *                    Inside the context the user's tags are ANDed with the
 *                    caller's: `use code cpp` then "project" asks for files
 *                    that are code AND cpp AND project. */
static int ObjQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                    const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 8) return ERR_BUFFER_TOO_SMALL;

    uint8_t scope;
    error_t src_rc = storage_scope_of(op, 0, &scope);
    if (src_rc != OK) return src_rc;

    TagFSState *state = tagfs_get_state();
    if (!state || !state->initialized) return ERR_NOT_INITIALIZED;

    /* The query's tags, read BEFORE the output bounce buffer is allocated so
     * an early return here cannot leak the kbuf. */
    char  *text = NULL;
    size_t tlen = 0;
    error_t rc = storage_crate_text(op, crates, ctx, &text, &tlen);
    if (rc != OK) return rc;

    TagSpelling *tags      = NULL;
    char        *use_block = NULL;
    uint32_t     total     = 0;
    rc = storage_tags_spelled(scope, text, tlen, &tags, &use_block, &total);
    if (rc != OK) {
        crate_buf_free(text);
        return rc;
    }

    uint32_t hdr[2] = { 0, 0 };   /* delivered, total */
    rc = storage_query_into(tags, total, out, ctx, hdr);
    kfree(tags);
    kfree(use_block);
    crate_buf_free(text);
    if (rc != OK) return rc;

    /* The header last: a crate whose ids did not all go in says nothing. */
    int crc = crate_out_commit_at(out, ctx, 0, hdr, sizeof(hdr));
    if (crc != OK) return crc;   /* fail closed: leave out->size unset */
    out->size = sizeof(hdr) + (uint64_t)hdr[0] * sizeof(uint32_t);
    return OK;
}

/* One tag's text out of the in_crate, spelled. The width is judged on the
 * crate's size before anything is copied: a text longer than any tag can be
 * is refused as the too-long tag it is. *out_text is the caller's to free. */
static error_t storage_one_tag(const ManifestOp *op, Crate *crates, const OpContext *ctx,
                               char **out_text, TagSpelling *out)
{
    *out_text = NULL;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    const Crate *src = &crates[op->in_crate];
    if (src->size == 0)              return ERR_TAGFS_INVALID_TAG;
    if (src->size > TAG_TEXT_WIDTH)  return ERR_TAGFS_TAG_TOO_LONG;

    char  *text = NULL;
    size_t len  = 0;
    error_t rc = storage_crate_text(op, crates, ctx, &text, &len);
    if (rc != OK) return rc;
    rc = TagSpell(text, len, out);
    if (rc != OK) {
        crate_buf_free(text);
        return rc;
    }
    *out_text = text;
    return OK;
}

/* STORAGE_TAG_SET  params:[u32 file_id]  in_crate: "key" or "key:value" —
 * the file wears exactly that tag, or the call is refused. */
static int ObjTagSet(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;

    char       *text = NULL;
    TagSpelling tag;
    error_t     rc = storage_one_tag(op, crates, ctx, &text, &tag);
    if (rc != OK) return rc;
    rc = tagfs_add_tag_string(param_u32(op, 0), &tag);
    crate_buf_free(text);
    return rc;
}

/* STORAGE_TAG_UNSET  params:[u32 file_id]  in_crate: "key" — every tag of the
 * key — or "key:value" — that tag alone. */
static int ObjTagUnset(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;

    char       *text = NULL;
    TagSpelling tag;
    error_t     rc = storage_one_tag(op, crates, ctx, &text, &tag);
    if (rc != OK) return rc;
    int removed = tagfs_remove_tag_string(param_u32(op, 0), &tag);
    crate_buf_free(text);
    return removed == 0 ? OK : ERR_INVALID_ARGUMENT;
}

/* The ids of the tags a file is created with, each interned; the first
 * refusal ends it. *out_ids is the caller's to kfree. */
static error_t storage_intern_all(const TagSpelling *tags, uint32_t count, uint16_t **out_ids)
{
    *out_ids = NULL;
    if (count == 0)
        return OK;
    uint16_t *ids = kmalloc(sizeof(uint16_t) * count);
    if (!ids)
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < count; i++) {
        error_t rc = TagFSInternSpelling(&tags[i], &ids[i]);
        if (rc != OK) {
            kfree(ids);
            return rc;
        }
    }
    *out_ids = ids;
    return OK;
}

/* STORAGE_OBJ_CREATE  params:[char filename[32]] (NUL-padded)
 *                            [u8 scope] (optional) — inside the Use Context
 *                            (absent, STORAGE_SCOPE_USE) or EVERYWHERE
 *                     in_crate (optional): tag string
 *                     out_crate (optional): u32 file_id
 *                     Inside the context the new file is stamped with the
 *                     user's tags as well as its own. Every tag is spelled
 *                     before any is interned, and a tag that does not spell
 *                     or intern refuses the create: no file is made without a
 *                     tag it was asked to wear. */
static int ObjCreate(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;

    uint8_t scope;
    error_t rc = storage_scope_of(op, 32, &scope);
    if (rc != OK) return rc;

    /* The name runs to its NUL inside the 32-byte field, or fills it. */
    size_t field    = op->param_size < 32 ? op->param_size : 32;
    size_t name_len = strnlen((const char *)op->params, field);
    if (name_len == 0) return ERR_INVALID_ARGUMENT;

    char  *text = NULL;
    size_t tlen = 0;
    rc = storage_crate_text(op, crates, ctx, &text, &tlen);
    if (rc != OK) return rc;

    TagSpelling *tags      = NULL;
    char        *use_block = NULL;
    uint32_t     total     = 0;
    uint16_t    *tag_ids   = NULL;
    char        *filename  = NULL;
    rc = storage_tags_spelled(scope, text, tlen, &tags, &use_block, &total);
    if (rc == OK)
        rc = storage_intern_all(tags, total, &tag_ids);
    if (rc == OK && !(filename = kmalloc(name_len + 1)))
        rc = ERR_NO_MEMORY;
    kfree(tags);
    kfree(use_block);
    crate_buf_free(text);
    if (rc != OK) {
        kfree(tag_ids);
        return rc;
    }
    memcpy(filename, op->params, name_len);
    filename[name_len] = '\0';

    uint32_t file_id = 0;
    int made = tagfs_create_file(filename, tag_ids, total, &file_id);
    kfree(filename);
    kfree(tag_ids);
    if (made != 0) return (int)(error_t)(-made);

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(uint32_t)) {
            (void)crate_write(out, ctx, &file_id, sizeof(uint32_t));
        }
    }
    return OK;
}


/* -------------------------------------------------------------------------
 * Snapshot ops — STORAGE_SNAP_CREATE, _DELETE, _LIST and _INFO.
 *
 * All four are answered ERR_UNSUPPORTED on a format-v1 volume, whatever their
 * params: a snapshot there could be taken and never read, and snapshots arrive
 * with format v2 (TagFS_SnapshotRefused, cow.h). The four stay on the wire so a
 * program that asks is told why, not met with an operation nobody knows.
 * ------------------------------------------------------------------------- */

static int ObjSnapRefused(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    switch (OP_KIND_OPCODE(op->op_kind)) {
    case STORAGE_SNAP_CREATE: return TagFS_SnapshotRefused("storage.snap.create");
    case STORAGE_SNAP_DELETE: return TagFS_SnapshotRefused("storage.snap.delete");
    case STORAGE_SNAP_LIST:   return TagFS_SnapshotRefused("storage.snap.list");
    default:                  return TagFS_SnapshotRefused("storage.snap.info");
    }
}

/* -------------------------------------------------------------------------
 * STORAGE_OBJ_ANCHOR — durability primitive.
 *
 *   params: [u32 file_id]   (0 = anchor everything; non-zero = the file
 *                             whose tags get the Touch payload's fid)
 *
 * Blocking semantics for the caller — returns when a Settle cycle that
 * began after the call has put every change inked before it on the medium
 * and a flush after it was kept (settle.h).
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

    /* A Settle cycle, asked for and waited on, so the caller's
     * fwrite-then-anchor is durable without a shutdown. Its answer is
     * checked for the same reason the flush below is: a cycle refused
     * settles nothing, and an anchor published over nothing settled would
     * be exactly the lie the rest of this function exists to stop. */
    error_t settled = TagFSSettle();
    if (settled != OK) {
        kprintf("[Storage] anchor of file %u: the volume's changes were not "
                "settled (error %u) — no anchor event is published\n",
                file_id, (unsigned)settled);
        return (int)settled;
    }

    /*
     * ‼ THIS IS THE OPERATION AN APPLICATION USES TO SAY "MAKE MY WRITE
     * DURABLE", AND IT USED TO THROW THE ANSWER AWAY.
     *
     * The flush was called, its result dropped on the floor, the "anchor"
     * event published on every tag of the file, and OK returned — so a
     * program that wrote, anchored, and was told yes had the kernel's word
     * that its bytes would survive the power going away, while they sat in a
     * cache the drive can lose. Every observer woken by the Touch event was
     * told the same thing. There is no worse lie this kernel can tell.
     *
     * A flush that did not happen buys no right to the word "anchored": on
     * any refusal the event is NOT published — an observer that never hears
     * is an observer that has not been misled — and the refusal goes back to
     * the caller as the medium's own word.
     *
     * Two flushes of the same disk meeting is not a refusal: the second waits
     * for the port in its roadstead (core/boardroom/roadstead.h) and is issued
     * when the first has given the port back, so no answer the application
     * cannot act on reaches it. The need is read after the cycle came back,
     * so a flush the cycle issued already covers it and costs nothing more.
     *
     * anchor() therefore keeps exactly the two answers it always had —
     * durable, or a named refusal of the medium — and neither of them is a
     * lie any more.
     */
    error_t flushed = VolumeFlushFor(VolumeFlushNeed());
    if (flushed != OK) {
        kprintf("[Storage] anchor of file %u: the medium did not make it "
                "durable (error %u) — no anchor event is published and the "
                "caller is told, because a file that was not made durable "
                "must not be announced as one\n",
                file_id, (unsigned)flushed);
        return (int)flushed;
    }

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
            for (uint32_t ti = 0; ti < wmeta.tag_count; ti++) {
                TouchPublishId(wmeta.tag_ids[ti], &ev, sizeof(ev),
                               pid, TOUCH_FLAG_TAGFS);
            }
            tagfs_metadata_free(&wmeta);
        }
    }
    /* Always also publish on the bare "anchor" tag — generic listeners.
     *
     * The one kernel publisher that deliberately does NOT name into the
     * Logbook. "anchor" is a durability event in the SHARED vocabulary,
     * keyed the same way as the per-file tag ids published just above, and
     * a process subscribes to it by name (box::tagfs::on_anchor) — usually
     * before this code has ever run. Naming it in the kernel's own book
     * would put publisher and subscriber on two different ids depending on
     * who spoke first. The reader door gives both sides the same id. */
    TouchTag anchor_full, anchor_bare;
    (void)TouchTagResolve("anchor", sizeof("anchor") - 1, &anchor_full, &anchor_bare);
    TouchPublishPair(anchor_full, anchor_bare, &ev, sizeof(ev),
                     pid, TOUCH_FLAG_TAGFS);
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
        { STORAGE_OBJ_SPAN,     ObjSpan,         OP_AUTH_APP, "storage.span"     },
        { STORAGE_OBJ_RENAME,   ObjRename,       OP_AUTH_APP, "storage.rename"   },
        /* Snapshots: app+, refused on format v1. */
        { STORAGE_SNAP_CREATE,  ObjSnapRefused,  OP_AUTH_APP, "storage.snap.create"},
        { STORAGE_SNAP_DELETE,  ObjSnapRefused,  OP_AUTH_APP, "storage.snap.delete"},
        { STORAGE_SNAP_LIST,    ObjSnapRefused,  OP_AUTH_APP, "storage.snap.list"  },
        { STORAGE_SNAP_INFO,    ObjSnapRefused,  OP_AUTH_APP, "storage.snap.info"  },
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
