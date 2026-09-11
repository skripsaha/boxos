
#include "klib.h"
#include "chit.h"
#include "crate_stage.h"
#include "crate_io.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "storage_deck.h"
#include "tagfs.h"
#include "use_context.h"
#include "tag_registry.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "ahci.h"
#include "ahci_async.h"
#include "kring.h"
#include "kresult.h"
#include "write_job.h"
#include "baton.h"
#include "ata.h"
#include "touch.h"
#include "cow.h"
#include "amp.h"
#include "boardroom.h"

static inline bool tagfs_volume_can_read_async(void)
{
    return BoardroomSeatCanReadAsync(tagfs_get_seat());
}

static inline bool tagfs_volume_is_ahci(void)
{
    return BoardroomSeatKind(tagfs_get_seat()) == BOARD_AHCI;
}

#define OBJ_WRITE_APPEND_FLAG (1u << 0)



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


typedef struct {
    process_t       *target;
    TagFSFileHandle *handle;
    Crate           *out_crate;
    uint8_t         *out_base;
    uintptr_t        out_user_addr;
    uint64_t         total_bytes;
    uint64_t         bytes_done;
    uint64_t         start_offset;
    uint64_t         waybill;
    uint32_t         submit_cookie;
    void            *dma_phys;
    void            *dma_virt;
    uint32_t         in_flight_off_in_blk;
    uint32_t         in_flight_chunk;
    error_t          if_status;

    Crate           *crates_kbuf;
    uint64_t         crates_uaddr;
    uint16_t         crate_count;

    Baton cq_node;
} ObjReadAsyncCtx;

static void obj_read_step(ObjReadAsyncCtx *ctx);
static void obj_read_finish(ObjReadAsyncCtx *ctx, error_t status, bool partial_ok);
static void obj_read_async_complete(uint8_t port, uint8_t slot,
                                     error_t status, void *ctx_);
static void obj_read_pump(void *ctx_);

static void obj_read_finish(ObjReadAsyncCtx *ctx, error_t status, bool partial_ok)
{
    Result r;
    memset(&r, 0, sizeof(r));
    uint64_t reported = partial_ok ? ctx->bytes_done : 0;
    ctx->out_crate->size = reported;
    r.error_code  = (status == OK) ? OK : ERR_IO;
    r.data_length = (uint32_t)reported;
    r.sender_pid  = 0;
    if (ctx->waybill) { r.context = KCTX_STORAGE; r.data_addr = ctx->waybill; }
    else              { r.context = KCTX_PACK24(KCTX_GUIDE, ctx->submit_cookie); }

    if (reported > 0 && ctx->out_base && ctx->target && ctx->target->cabin) {
        vmm_user_buf_commit_out(ctx->target->cabin->vmm, ctx->out_user_addr,
                                 ctx->out_base, (size_t)reported);
    }

    if (ctx->out_base)  vmm_user_buf_free(ctx->out_base);
    if (ctx->dma_phys)  pmm_free(ctx->dma_phys, 1);
    if (ctx->handle)    tagfs_close(ctx->handle);

    if (ctx->crates_kbuf) {
        crate_stage_commit_and_release(ctx->crates_kbuf, ctx->crate_count,
                                        (ctx->target && ctx->target->cabin) ? ctx->target->cabin->vmm : NULL,
                                        ctx->crates_uaddr);
    }

    ChitDue(ctx->target, ctx->submit_cookie);
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
    ctx->if_status = status;
    BatonPass(&ctx->cq_node);
}

static void obj_read_pump(void *ctx_)
{
    ObjReadAsyncCtx *ctx = (ObjReadAsyncCtx *)ctx_;

    if (ctx->if_status != OK) {
        obj_read_finish(ctx, ERR_IO, false);
        return;
    }

    memcpy(ctx->out_base + ctx->bytes_done,
           (uint8_t *)ctx->dma_virt + ctx->in_flight_off_in_blk,
           ctx->in_flight_chunk);
    ctx->bytes_done += ctx->in_flight_chunk;

    if (ctx->bytes_done >= ctx->total_bytes) {
        obj_read_finish(ctx, OK, true);
        return;
    }

    obj_read_step(ctx);
}

static void obj_read_step(ObjReadAsyncCtx *ctx)
{
    if (!tagfs_handle_is_of_this_mount(ctx->handle)) {
        obj_read_finish(ctx, ERR_IO, false);
        return;
    }

    uint64_t file_pos = ctx->start_offset + ctx->bytes_done;

    uint64_t extent_start = 0;
    uint16_t ext_idx = 0xFFFF;
    for (uint16_t i = 0; i < ctx->handle->extent_count; i++) {
        uint64_t ext_size = (uint64_t)ctx->handle->extents[i].block_count * TAGFS_BLOCK_SIZE;
        if (file_pos < extent_start + ext_size) { ext_idx = i; break; }
        extent_start += ext_size;
    }
    if (ext_idx == 0xFFFF) {
        obj_read_finish(ctx, OK, true);
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
    error_t  err = BoardroomReadAsync(tagfs_get_seat(), lba, 8, ctx->dma_phys,
                                      obj_read_async_complete, ctx);
    if (err != OK) {
        obj_read_finish(ctx, ERR_IO, false);
    }
}

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

    ChitGive(ctx, "storage.ferry", waybill);
    return ERR_WOULD_BLOCK;
}

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
    uint64_t waybill = (op->param_size >= 20) ? param_u64(op, 12) : 0;
    Crate   *out     = &crates[op->out_crate];
    if (out->capacity == 0)
        return storage_op_fail(ctx, crates, ERR_BUFFER_TOO_SMALL, waybill);

    void *kp = crate_out_alloc(out, out->capacity);
    if (!kp) return storage_op_fail(ctx, crates, ERR_INVALID_ADDRESS, waybill);

    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    if (!handle) {
        crate_buf_free(kp);
        return storage_op_fail(ctx, crates, ERR_FILE_NOT_FOUND, waybill);
    }

    handle->offset = offset;

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
                        async_ctx->crates_kbuf   = crates;
                        async_ctx->crates_uaddr  = ctx->crates_uaddr;
                        async_ctx->crate_count   = ctx->crate_count;

                        async_ctx->cq_node.run   = obj_read_pump;
                        async_ctx->cq_node.ctx   = async_ctx;

                        process_set_state(ctx->proc, PROC_WAITING);
                        ChitGive(ctx, "storage.read", file_id);
                        obj_read_step(async_ctx);
                        return ERR_WOULD_BLOCK;
                    }
                    kfree(async_ctx);
                }
                pmm_free(dma_phys, 1);
            }
        }
    }

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
        out->size = 0;
    }
    crate_buf_free(kp);

    if (waybill != 0)
        return storage_sync_ferry_finalize(ctx, crates, sync_err, sync_bytes, waybill);
    return (sync_err == OK) ? OK : (int)sync_err;
}


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
    uint64_t waybill = (op->param_size >= 24) ? param_u64(op, 16) : 0;

    Crate *src = &crates[op->in_crate];
    if (src->size == 0)
        return storage_op_fail(ctx, crates, ERR_INVALID_ARGUMENT, waybill);

    void *src_bounce = crate_in_buf(src, ctx);
    if (!src_bounce) return storage_op_fail(ctx, crates, ERR_INVALID_ADDRESS, waybill);

    Crate *out_crate = NULL;
    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *o = &crates[op->out_crate];
        if (o->capacity >= 16) out_crate = o;
    }

    if (tagfs_volume_is_ahci() && ctx && ctx->proc && g_amp.total_cores > 1) {
        int rc = ObjWriteAsync(file_id, offset, flags,
                               src_bounce, (uint32_t)src->size,
                               out_crate, ctx,
                               crates, ctx->crate_count, ctx->crates_uaddr,
                               waybill);
        if (rc == ERR_WOULD_BLOCK) {
            return ERR_WOULD_BLOCK;
        }
    }

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

    if (wrote > 0 && TouchHasAnyListeners()) {
        TagFSMetadata wmeta;
        memset(&wmeta, 0, sizeof(wmeta));
        if (tagfs_get_metadata(file_id, &wmeta) == OK) {
            struct {
                uint32_t file_id;
                uint8_t  op;
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
        (void)crate_write(out_crate, ctx, stats, 16);
    }

    if (waybill != 0)
        return storage_sync_ferry_finalize(ctx, crates, OK, (uint64_t)wrote, waybill);
    return OK;
}


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
        for (uint16_t i = 0; i < md.tag_count; i++) {
            char key[128];
            if (!tagfs_tag_key(md.tag_ids[i], key, sizeof(key))) continue;
            if (strcmp(key, "system") == 0 || strcmp(key, "boot") == 0) {
                tagfs_metadata_free(&md);
                return ERR_PERMISSION_DENIED;
            }
        }
        tagfs_metadata_free(&md);
    }

    int rc = tagfs_delete_file(file_id);
    return (rc == 0) ? OK : ERR_FILE_NOT_FOUND;
}


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
    uint64_t      old_size = 0;
    bool          have_md  = false;
    if (tagfs_get_metadata(file_id, &md) == 0) {
        for (uint16_t i = 0; i < md.tag_count; i++) {
            char key[128];
            if (!tagfs_tag_key(md.tag_ids[i], key, sizeof(key))) continue;
            if (strcmp(key, "system") == 0 || strcmp(key, "boot") == 0) {
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
                uint8_t  op;
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

    uint64_t need = 4u + 4u + 8u + 2u + 2u + filename_len;
    for (uint16_t i = 0; i < md.tag_count; i++) {
        char k[128], v[128];
        (void)tagfs_tag_parts(md.tag_ids[i], k, sizeof(k), v, sizeof(v), NULL);
        need += 2u + 2u + 1u + strlen(k) + strlen(v);
    }

    if (need > out->capacity) {
        tagfs_metadata_free(&md);
        return ERR_BUFFER_TOO_SMALL;
    }

    uint8_t *kp = crate_out_alloc(out, need);
    if (!kp) { tagfs_metadata_free(&md); return ERR_INVALID_ADDRESS; }

    uint32_t flags = md.flags;
    {
        const WellKnownTags *wk = tagfs_get_well_known_tags();
        uint16_t trashed_id = (wk && wk->trashed)
                              ? (uint16_t)__builtin_ctzll(wk->trashed) : TAGFS_INVALID_TAG_ID;
        uint16_t hidden_id  = (wk && wk->hidden)
                              ? (uint16_t)__builtin_ctzll(wk->hidden)  : TAGFS_INVALID_TAG_ID;
        for (uint16_t i = 0; i < md.tag_count; i++) {
            if (md.tag_ids[i] == trashed_id) flags |= TAGFS_FILE_TRASHED;
            if (md.tag_ids[i] == hidden_id)  flags |= TAGFS_FILE_HIDDEN;
        }
    }

    uint64_t pos = 0;
    memcpy(kp + pos, &md.file_id, 4); pos += 4;
    memcpy(kp + pos, &flags,      4); pos += 4;
    memcpy(kp + pos, &md.size,    8); pos += 8;
    memcpy(kp + pos, &md.tag_count, 2); pos += 2;
    memcpy(kp + pos, &filename_len, 2); pos += 2;
    if (filename_len) { memcpy(kp + pos, md.filename, filename_len); pos += filename_len; }

    uint16_t written = 0;
    for (uint16_t i = 0; i < md.tag_count; i++) {
        char k[128], v[128];
        bool is_system = false;
        (void)tagfs_tag_parts(md.tag_ids[i], k, sizeof(k), v, sizeof(v), &is_system);
        uint16_t kl = (uint16_t)strlen(k);
        uint16_t vl = (uint16_t)strlen(v);
        if (pos + 5u + kl + vl > need) break;
        memcpy(kp + pos, &kl, 2); pos += 2;
        memcpy(kp + pos, &vl, 2); pos += 2;
        kp[pos++] = is_system ? 1 : 0;
        if (kl) { memcpy(kp + pos, k, kl); pos += kl; }
        if (vl) { memcpy(kp + pos, v, vl); pos += vl; }
        written++;
    }
    memcpy(kp + 16, &written, 2);

    int crc = crate_out_commit(out, ctx, kp, pos);
    crate_buf_free(kp);
    tagfs_metadata_free(&md);
    if (crc != OK) return crc;
    out->size = pos;
    return OK;
}


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

static error_t storage_scope_of(const ManifestOp *op, size_t at, uint8_t *out_scope)
{
    uint8_t scope = STORAGE_SCOPE_USE;
    if (op->param_size > at) scope = op->params[at];
    if (scope != STORAGE_SCOPE_USE && scope != STORAGE_SCOPE_EVERYWHERE)
        return ERR_INVALID_ARGUMENT;
    *out_scope = scope;
    return OK;
}

static error_t storage_tags_with_use(uint8_t scope,
                                     const char **own, uint32_t own_count,
                                     const char ***out_all, char **out_block,
                                     uint32_t *out_total)
{
    *out_all   = NULL;
    *out_block = NULL;
    *out_total = own_count;
    if (scope != STORAGE_SCOPE_USE) return OK;

    char        *block = NULL;
    const char **names = NULL;
    uint32_t     count = 0;
    error_t rc = UseContextTagArray(&block, &names, &count);
    if (rc != OK) return rc;
    if (count == 0) return OK;

    const char **all = kmalloc(sizeof(char *) * (own_count + count));
    if (!all) {
        kfree(block);
        kfree(names);
        return ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < own_count; i++) all[i] = own[i];
    for (uint32_t i = 0; i < count; i++)     all[own_count + i] = names[i];
    kfree(names);

    *out_all   = all;
    *out_block = block;
    *out_total = own_count + count;
    return OK;
}

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

    const char *own_tags[16];
    char        own_storage[16][32];
    int own = parse_tag_list(qbuf, qlen, own_tags, own_storage, 16);
    if (own < 0) own = 0;

    const char **all_tags  = NULL;
    char        *use_block = NULL;
    uint32_t     total     = 0;
    error_t mrc = storage_tags_with_use(scope, own_tags, (uint32_t)own,
                                        &all_tags, &use_block, &total);
    if (mrc != OK) return mrc;
    const char **asked = all_tags ? all_tags : own_tags;

    uint32_t  room    = tagfs_file_ceiling();
    uint32_t *all_ids = NULL;
    if (room > 0) {
        all_ids = kmalloc(room * sizeof(uint32_t));
        if (!all_ids) {
            kfree(all_tags);
            kfree(use_block);
            return ERR_NO_MEMORY;
        }
    }
    int count = 0;
    if (all_ids) {
        if (total > 0) count = tagfs_query_files(asked, total, all_ids, room);
        else           count = tagfs_list_all_files(all_ids, room);
        if (count < 0) count = 0;
    }
    kfree(all_tags);
    kfree(use_block);

    uint32_t max_results = (uint32_t)((out->capacity - 8) / sizeof(uint32_t));
    uint32_t hdr[2];
    hdr[0] = (uint32_t)count < max_results ? (uint32_t)count : max_results;
    hdr[1] = (uint32_t)count;
    uint64_t out_bytes = 8 + (uint64_t)hdr[0] * sizeof(uint32_t);

    uint8_t *kp = crate_out_alloc(out, out_bytes);
    if (!kp) {
        kfree(all_ids);
        return ERR_INVALID_ADDRESS;
    }
    memcpy(kp, hdr, sizeof(hdr));
    if (hdr[0] > 0) memcpy(kp + 8, all_ids, hdr[0] * sizeof(uint32_t));
    kfree(all_ids);

    int crc = crate_out_commit(out, ctx, kp, out_bytes);
    crate_buf_free(kp);
    if (crc != OK) return crc;
    out->size = out_bytes;
    return OK;
}

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

static int ObjCreate(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;

    uint8_t scope;
    error_t src_rc = storage_scope_of(op, 32, &scope);
    if (src_rc != OK) return src_rc;

    char filename[33];
    size_t fn_copy = op->param_size < 32 ? op->param_size : 32;
    memcpy(filename, op->params, fn_copy);
    filename[fn_copy] = '\0';
    for (size_t i = 0; i < fn_copy; i++) {
        if (filename[i] == '\0') { filename[i] = '\0'; break; }
    }
    if (filename[0] == '\0') return ERR_INVALID_ARGUMENT;

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

    const char *own_tags[16];
    char        own_storage[16][32];
    int own = parse_tag_list(tag_buf, tlen, own_tags, own_storage, 16);
    if (own < 0) own = 0;

    const char **all_tags  = NULL;
    char        *use_block = NULL;
    uint32_t     total     = 0;
    error_t mrc = storage_tags_with_use(scope, own_tags, (uint32_t)own,
                                        &all_tags, &use_block, &total);
    if (mrc != OK) return mrc;
    const char **stamped = all_tags ? all_tags : own_tags;

    uint16_t *tag_ids      = NULL;
    uint16_t  intern_count = 0;
    if (total > 0) {
        tag_ids = kmalloc(sizeof(uint16_t) * total);
        if (!tag_ids) {
            kfree(all_tags);
            kfree(use_block);
            return ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < total; i++) {
            char k[64], v[64];
            if (tagfs_parse_tag(stamped[i], k, sizeof(k), v, sizeof(v)) != 0) continue;
            uint16_t tid = tagfs_tag_intern(stamped[i]);
            if (tid != TAGFS_INVALID_TAG_ID) tag_ids[intern_count++] = tid;
        }
    }
    kfree(all_tags);
    kfree(use_block);

    uint32_t file_id = 0;
    int rc = tagfs_create_file(filename,
                               intern_count > 0 ? tag_ids : NULL,
                               intern_count, &file_id);
    kfree(tag_ids);
    if (rc != 0) return ERR_DISK_FULL;

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(uint32_t)) {
            (void)crate_write(out, ctx, &file_id, sizeof(uint32_t));
        }
    }
    return OK;
}



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
    uint8_t blob[4 + 64 * 4];
    memcpy(blob, &count, 4);
    if (count > 0) memcpy(blob + 4, ids, count * 4);
    if (crate_write(out, ctx, blob, bytes) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int ObjSnapInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 4)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint32_t snapshot_id = param_u32(op, 0);

    CowSnapshot cs;
    error_t err = TagFS_SnapshotInfo(snapshot_id, &cs);
    if (err != OK) return (int)err;

    uint32_t need = 4u + 4u + 8u + 4u + 8u + 1u + 32u;
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

static int ObjAnchor(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    uint32_t file_id = (op->param_size >= 4) ? param_u32(op, 0) : 0;

    tagfs_sync();
    tagfs_flush_cache();

    struct __attribute__((packed)) {
        uint32_t file_id;
        uint8_t  op;
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
    TouchTag anchor_full, anchor_bare;
    TouchTagResolve("anchor", &anchor_full, &anchor_bare);
    TouchPublishPair(anchor_full, anchor_bare, &ev, sizeof(ev),
                     pid, TOUCH_FLAG_TAGFS);
    return OK;
}


error_t StorageDeckRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        { STORAGE_TAG_QUERY,    ObjQuery,        OP_AUTH_APP, "storage.query"    },
        { STORAGE_OBJ_READ,     ObjRead,         OP_AUTH_APP, "storage.read"     },
        { STORAGE_OBJ_GET_INFO, ObjGetInfo,      OP_AUTH_APP, "storage.getinfo"  },
        { STORAGE_TAG_SET,      ObjTagSet,       OP_AUTH_APP, "storage.tag.set"  },
        { STORAGE_TAG_UNSET,    ObjTagUnset,     OP_AUTH_APP, "storage.tag.unset"},
        { STORAGE_OBJ_WRITE,    ObjWrite,        OP_AUTH_APP, "storage.write"    },
        { STORAGE_OBJ_CREATE,   ObjCreate,       OP_AUTH_APP, "storage.create"   },
        { STORAGE_OBJ_DELETE,   ObjDelete,       OP_AUTH_APP, "storage.delete"   },
        { STORAGE_OBJ_TRUNCATE, ObjTruncate,     OP_AUTH_APP, "storage.truncate" },
        { STORAGE_OBJ_RENAME,   ObjRename,       OP_AUTH_APP, "storage.rename"   },
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