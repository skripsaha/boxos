/*
 * Storage Deck — Manifest-native handlers.
 *
 * Removes the 168/176-byte stack buffers that capped legacy OBJ_READ /
 * OBJ_WRITE at sub-200-byte payloads per syscall. Reads and writes now go
 * straight from / to a Crate of arbitrary capacity, translated once via
 * vmm_translate_user_addr. A 1-MiB read fits in one syscall.
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
#include "ata.h"
#include "touch.h"
#include "cow.h"
#include "amp.h"   /* g_amp.total_cores — async write needs a K-Core to pump */

#define OBJ_WRITE_APPEND_FLAG (1u << 0)

/* -------------------------------------------------------------------------
 * Crate translation
 *
 * StorageCrateMap is the legacy single-page fast path. Works only when
 * the requested bytes fit inside one phys page — used for parameter-
 * sized payloads (rename name, query result header, etc).
 *
 * For multi-page user payloads (read/write data crates) we bounce
 * through a kmalloc'd kernel buffer:
 *   crate_in_buf       — copy user → fresh kbuf (for input crates)
 *   crate_out_alloc    — allocate empty kbuf same size as crate
 *   crate_out_commit   — copy filled kbuf → user pages
 *   crate_buf_free     — release kbuf
 * Each helper handles arbitrary cross-page user ranges by walking the
 * user PT page-by-page.
 * ------------------------------------------------------------------------- */

static void *StorageCrateMap(const Crate *c, const OpContext *ctx, uint64_t bytes)
{
    if (!c || bytes == 0)            return NULL;
    if (bytes > c->capacity)         return NULL;
    if (ctx && ctx->proc && ctx->proc->cabin) {
        return vmm_translate_user_addr(ctx->proc->cabin, (uintptr_t)c->addr, (size_t)bytes);
    }
    return (void *)(uintptr_t)c->addr;
}

static void *crate_in_buf(const Crate *src, const OpContext *ctx)
{
    if (!src || src->size == 0) return NULL;
    if (src->size > src->capacity) return NULL;
    if (ctx && ctx->proc && ctx->proc->cabin) {
        return vmm_user_buf_in(ctx->proc->cabin, (uintptr_t)src->addr, (size_t)src->size);
    }
    /* No cabin (kernel-internal caller) — just snapshot the bytes so
     * cleanup is uniform. */
    void *kbuf = kmalloc((size_t)src->size);
    if (!kbuf) return NULL;
    memcpy(kbuf, (const void *)(uintptr_t)src->addr, (size_t)src->size);
    return kbuf;
}

static void *crate_out_alloc(const Crate *out, uint64_t bytes)
{
    if (!out || bytes == 0) return NULL;
    if (bytes > out->capacity) return NULL;
    return vmm_user_buf_alloc_out((size_t)bytes);
}

static int crate_out_commit(const Crate *out, const OpContext *ctx,
                             const void *kbuf, uint64_t bytes)
{
    if (!out || !kbuf || bytes == 0) return 0;
    if (ctx && ctx->proc && ctx->proc->cabin) {
        return vmm_user_buf_commit_out(ctx->proc->cabin, (uintptr_t)out->addr,
                                        kbuf, (size_t)bytes);
    }
    memcpy((void *)(uintptr_t)out->addr, kbuf, (size_t)bytes);
    return 0;
}

static void crate_buf_free(void *kbuf)
{
    if (kbuf) vmm_user_buf_free(kbuf);
}

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
    Crate           *out_crate;    /* user-mapped, write back size on completion */
    uint8_t         *out_base;     /* kernel-side bounce buffer (kmalloc'd) */
    uintptr_t        out_user_addr;/* user vaddr; commit_out target */
    uint64_t         total_bytes;
    uint64_t         bytes_done;
    uint64_t         start_offset;
    void            *dma_phys;
    void            *dma_virt;
    uint32_t         in_flight_off_in_blk;
    uint32_t         in_flight_chunk;
} ObjReadAsyncCtx;

static void obj_read_step(ObjReadAsyncCtx *ctx);
static void obj_read_finish(ObjReadAsyncCtx *ctx, error_t status, bool partial_ok);
static void obj_read_async_complete(uint8_t port, uint8_t slot,
                                     error_t status, void *ctx_);

static void obj_read_finish(ObjReadAsyncCtx *ctx, error_t status, bool partial_ok)
{
    Result r;
    memset(&r, 0, sizeof(r));
    uint64_t reported = partial_ok ? ctx->bytes_done : 0;
    ctx->out_crate->size = reported;
    r.error_code  = (status == OK) ? OK : ERR_IO;
    r.data_length = (uint32_t)reported;
    r.sender_pid  = 0;
    r.context     = KCTX_GUIDE;

    /* Commit bounce buffer back into user pages BEFORE waking caller.
     * If commit fails (e.g. user unmapped the page mid-flight) we still
     * report the byte count — the user's buffer is just left untouched. */
    if (reported > 0 && ctx->out_base && ctx->target && ctx->target->cabin) {
        vmm_user_buf_commit_out(ctx->target->cabin, ctx->out_user_addr,
                                 ctx->out_base, (size_t)reported);
    }

    if (ctx->out_base)  vmm_user_buf_free(ctx->out_base);
    if (ctx->dma_phys)  pmm_free(ctx->dma_phys, 1);
    if (ctx->handle)    tagfs_close(ctx->handle);

    KResultPush(ctx->target, &r);
    process_ref_dec(ctx->target);
    kfree(ctx);
}

static void obj_read_async_complete(uint8_t port, uint8_t slot,
                                     error_t status, void *ctx_)
{
    (void)port;
    (void)slot;
    ObjReadAsyncCtx *ctx = (ObjReadAsyncCtx *)ctx_;

    if (status != OK) {
        obj_read_finish(ctx, ERR_IO, /*partial_ok=*/false);
        return;
    }

    /* Apply this chunk. */
    memcpy(ctx->out_base + ctx->bytes_done,
           (uint8_t *)ctx->dma_virt + ctx->in_flight_off_in_blk,
           ctx->in_flight_chunk);
    ctx->bytes_done += ctx->in_flight_chunk;

    if (ctx->bytes_done >= ctx->total_bytes) {
        obj_read_finish(ctx, OK, /*partial_ok=*/true);
        return;
    }

    /* More to do — fire the next block. */
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
    error_t  err = ahci_submit_read_async(tagfs_get_ahci_port(), lba, 8, ctx->dma_phys,
                                           obj_read_async_complete, ctx, &slot);
    if (err != OK) {
        obj_read_finish(ctx, ERR_IO, /*partial_ok=*/false);
    }
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
    Crate   *out     = &crates[op->out_crate];
    if (out->capacity == 0) return ERR_BUFFER_TOO_SMALL;

    /* Bounce buffer for multi-page user payloads. tagfs_read fills kp,
     * we copy back to user pages at the end. The kernel address is one
     * contiguous allocation regardless of how the user pages map. */
    void *kp = crate_out_alloc(out, out->capacity);
    if (!kp) return ERR_INVALID_ADDRESS;

    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    if (!handle) {
        crate_buf_free(kp);
        return ERR_FILE_NOT_FOUND;
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
        ahci_is_initialized() && ctx && ctx->proc && g_amp.total_cores > 1) {

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
                        async_ctx->dma_phys      = dma_phys;
                        async_ctx->dma_virt      = dma_virt;

                        /* Park BEFORE the first submit — if the IRQ
                         * fires before we set WAITING, KResultPush's
                         * "if state==WAITING flip to WORKING" is a
                         * no-op and the wake is lost. Setting WAITING
                         * first makes that transition reliable. */
                        process_set_state(ctx->proc, PROC_WAITING);
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

    if (got < 0) {
        crate_buf_free(kp);
        out->size = 0;
        return ERR_IO;
    }
    if (got > 0) {
        crate_out_commit(out, ctx, kp, (uint64_t)got);
    }
    crate_buf_free(kp);
    out->size = (uint64_t)got;
    return OK;
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

    Crate *src = &crates[op->in_crate];
    if (src->size == 0) return ERR_INVALID_ARGUMENT;

    /* Bounce-buffer the input crate. Multi-page user buffers are common
     * (any write over 4 KiB) and vmm_translate_user_addr cannot span
     * pages, so we copy through a kmalloc'd kernel buffer. The async
     * path takes ownership of src_bounce on a successful submit and
     * frees it from W_DONE; otherwise we free here. */
    void *src_bounce = crate_in_buf(src, ctx);
    if (!src_bounce) return ERR_INVALID_ADDRESS;

    /* Output stats crate (16 bytes) fits in a single page — direct map. */
    Crate *out_crate = NULL;
    void  *out_kp    = NULL;
    if (op->out_crate != CRATE_INDEX_NONE) {
        out_crate = &crates[op->out_crate];
        if (out_crate->capacity >= 16) {
            out_kp = StorageCrateMap(out_crate, ctx, 16);
        }
    }

    /* Async write requires a K-Core to pump its IRQ-deferred completion
     * continuations (write completion does heavy work — alloc / DiskBook /
     * CoW — that cannot run in IRQ context, so it is deferred via irq_defer
     * and drained by kcore_run_loop). That loop only exists in multi-core
     * mode; on a single core the BSP runs userspace and never pumps, so the
     * job would strand. Use the sync path there — it is correct and has no
     * benefit to lose (one core does everything regardless). */
    if (ahci_is_initialized() && ctx && ctx->proc && g_amp.total_cores > 1) {
        int rc = ObjWriteAsync(file_id, offset, flags,
                               src_bounce, (uint32_t)src->size,
                               out_crate, out_kp, ctx);
        if (rc == ERR_WOULD_BLOCK) {
            /* Async owns src_bounce now — it frees on W_DONE. */
            return ERR_WOULD_BLOCK;
        }
        /* Hard failure before submission — free + fall through. */
    }

    /* Sync fallback. */
    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_WRITE);
    if (!handle) {
        crate_buf_free(src_bounce);
        return ERR_FILE_NOT_FOUND;
    }

    if (flags & OBJ_WRITE_APPEND_FLAG) {
        handle->offset = handle->file_size;
    } else {
        handle->offset = offset;
    }

    int wrote = tagfs_write(handle, src_bounce, src->size);
    uint64_t final_size = handle->file_size;
    tagfs_close(handle);
    crate_buf_free(src_bounce);

    if (wrote < 0) return ERR_IO;

    if (out_kp && out_crate && out_crate->capacity >= 16) {
        uint64_t bytes_written = (uint64_t)wrote;
        memcpy((uint8_t *)out_kp + 0, &bytes_written, sizeof(uint64_t));
        memcpy((uint8_t *)out_kp + 8, &final_size,    sizeof(uint64_t));
        out_crate->size = 16;
    }
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
 *     [u16 key_len][u16 val_len][char key[key_len]][char val[val_len]]
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
        need += 2u + 2u + kl + vl;
    }

    if (need > out->capacity) {
        tagfs_metadata_free(&md);
        return ERR_BUFFER_TOO_SMALL;
    }

    uint8_t *kp = StorageCrateMap(out, ctx, need);
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
        if (kl) { memcpy(kp + pos, k, kl); pos += kl; }
        if (vl) { memcpy(kp + pos, v, vl); pos += vl; }
    }

    out->size = pos;
    tagfs_metadata_free(&md);
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

    uint8_t *kp = StorageCrateMap(out, ctx, out->capacity);
    if (!kp) return ERR_INVALID_ADDRESS;

    uint32_t max_results = (uint32_t)((out->capacity - 4) / sizeof(uint32_t));
    uint32_t *file_ids = (uint32_t *)(kp + 4);

    /* Query string from in_crate, if any. */
    char    qbuf[256];
    size_t  qlen = 0;
    if (op->in_crate != CRATE_INDEX_NONE) {
        Crate *src = &crates[op->in_crate];
        if (src->size > 0 && src->size < sizeof(qbuf)) {
            const void *src_kp = StorageCrateMap(src, ctx, src->size);
            if (!src_kp) return ERR_INVALID_ADDRESS;
            memcpy(qbuf, src_kp, src->size);
            qlen = src->size;
            qbuf[qlen] = '\0';
        }
    }

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
    out->size = 4 + cnt32 * sizeof(uint32_t);
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

    const void *src_kp = StorageCrateMap(src, ctx, src->size);
    if (!src_kp) return ERR_INVALID_ADDRESS;

    char tag[128];
    memcpy(tag, src_kp, src->size);
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

    const void *src_kp = StorageCrateMap(src, ctx, src->size);
    if (!src_kp) return ERR_INVALID_ADDRESS;

    char key[64];
    memcpy(key, src_kp, src->size);
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
            const void *src_kp = StorageCrateMap(src, ctx, src->size);
            if (!src_kp) return ERR_INVALID_ADDRESS;
            memcpy(tag_buf, src_kp, src->size);
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
            void *kp = StorageCrateMap(out, ctx, sizeof(uint32_t));
            if (kp) {
                memcpy(kp, &file_id, sizeof(uint32_t));
                out->size = sizeof(uint32_t);
            }
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

    const void *src_kp = StorageCrateMap(src, ctx, src->size);
    if (!src_kp) return ERR_INVALID_ADDRESS;

    char tag[128];
    memcpy(tag, src_kp, src->size);
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
            void *kp = StorageCrateMap(out, ctx, 4);
            if (kp) {
                memcpy(kp, &snapshot_id, sizeof(uint32_t));
                out->size = 4;
            }
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
    void *kp = StorageCrateMap(out, ctx, bytes);
    if (!kp) return ERR_INVALID_ADDRESS;
    memcpy(kp, &count, 4);
    if (count > 0) memcpy((uint8_t *)kp + 4, ids, count * 4);
    out->size = bytes;
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
    ata_flush_cache(1);

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
        { STORAGE_OBJ_RENAME,   ObjRename,       OP_AUTH_APP, "storage.rename"   },
        /* Per-process context: app+. */
        { STORAGE_CONTEXT_SET,  ObjContextSet,   OP_AUTH_APP, "storage.ctx.set"  },
        { STORAGE_CONTEXT_CLEAR,ObjContextClear, OP_AUTH_APP, "storage.ctx.clear"},
        /* Snapshot management: app+. */
        { STORAGE_SNAP_CREATE,  ObjSnapCreate,   OP_AUTH_APP, "storage.snap.create"},
        { STORAGE_SNAP_DELETE,  ObjSnapDelete,   OP_AUTH_APP, "storage.snap.delete"},
        { STORAGE_SNAP_LIST,    ObjSnapList,     OP_AUTH_APP, "storage.snap.list"  },
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
