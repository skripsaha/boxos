/*
 * Storage Deck — Manifest-native handlers.
 *
 * Removes the 168/176-byte stack buffers that capped legacy OBJ_READ /
 * OBJ_WRITE at sub-200-byte payloads per syscall. Reads and writes now move
 * to / from a Crate of arbitrary capacity through the page-walked crate_io
 * primitives, so a payload that straddles a page boundary is copied across
 * every backing frame. A 1-MiB read fits in one syscall.
 *
 * Param layouts (little-endian, packed); a file id is a u64 on the wire, as
 * it is in the kernel:
 *   OBJ_READ      params: [u64 file_id][u64 offset]
 *   OBJ_WRITE     params: [u64 file_id][u64 offset][u32 flags]
 *   OBJ_SPAN      params: [u64 file_id][u64 size][u32 flags]
 *   OBJ_DELETE    params: [u64 file_id]
 *   OBJ_RENAME    params: [u64 file_id][u16 new_name_len][char new_name[]]
 *   OBJ_GET_INFO  params: [u64 file_id]
 *
 *   OBJ_READ.out_crate     : payload destination, written a block at a time
 *                            as the read reaches it; size <- the bytes that
 *                            landed, on a refusal too
 *   OBJ_WRITE.in_crate     : payload source, taken out of the program's pages
 *                            a block at a time as the write reaches it
 *   OBJ_WRITE.out_crate    : optional [u64 bytes_written][u64 new_file_size],
 *                            written on a refusal too: the bytes the file's
 *                            inked record holds of the write, and its size
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
#include "book_mount.h"   /* a file its book holds locked */
#include "use_context.h"   /* the user's tags — folded into query and create */
#include "ink.h"           /* InkBookRefuses — a create a book refuses interns nothing */
#include "vmm.h"
#include "process.h"
#include "kring.h"
#include "kresult.h"
#include "write_job.h"
#include "read_job.h"  /* a read through the port, and the sink its bytes land in */
#include "ata.h"
#include "touch.h"
#include "cow.h"
#include "plot.h"     /* PlotFind, PlotSource — the write's bytes where they are */
#include "settle_ops.h"  /* anchor, obj.write.settled — the waits for the Settle */

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
 * completion cannot race it (no UAF). The answer passes the border every
 * answer passes (ChitAnswer). */
int storage_sync_ferry_finalize(const OpContext *ctx, Crate *crates,
                                error_t err, uint64_t bytes,
                                uint64_t waybill)
{
    err = ChitAnswer(err, ctx->proc ? ctx->proc->pid : 0, 0, "storage.ferry");
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

/* The warrant is the cabin's authority as it stands when the call is taken:
 * a tag the program puts on or takes off itself later changes its next call,
 * not this one. */
InkWarrant StorageCallWarrant(const OpContext *ctx)
{
    if (!ctx || !ctx->proc || !ctx->proc->cabin)
        return INK_UNWARRANTED;
    uint32_t auth = __atomic_load_n(&ctx->proc->cabin->auth_bits, __ATOMIC_RELAXED);
    return (auth & (AUTH_TAG_SYSTEM | AUTH_TAG_GOD)) ? INK_WARRANTED : INK_UNWARRANTED;
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

/* -------------------------------------------------------------------------
 * OBJ_READ — up to out_crate.capacity bytes of the file from offset, into the
 * program's pages a block at a time as the read reaches them: never bounced
 * whole through the kernel heap, so a read is bounded by the volume, not the
 * heap (S3c). The file is opened first, and nothing is allocated for a read
 * the file refuses.
 *
 * On a seat that takes reads nobody stands over, with a K-Core to pump them,
 * the read is a ReadJob (async/read_job.h) and the program is parked until it
 * answers; otherwise — one core, a legacy channel, a kernel caller, no job to
 * be had — it stands in the program's call. Either way the answer is the one
 * contract: the real reason, never folded into ERR_IO (S3b), with the bytes
 * that landed before it in out_crate.size.
 * ------------------------------------------------------------------------- */

static int ObjRead(const ManifestOp *op,
                   Crate            *crates,
                   uint16_t          crate_count,
                   const OpContext  *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 16)               return ERR_INVALID_ARGUMENT;

    uint64_t file_id = param_u64(op, 0);
    uint64_t offset  = param_u64(op, 8);
    /* Ф26e: optional trailing correlation waybill (box::ferry). Absent for a
     * synchronous read (param_size 16) → 0 → plain KCTX_GUIDE completion. */
    uint64_t waybill = (op->param_size >= 24) ? param_u64(op, 16) : 0;
    Crate   *out     = &crates[op->out_crate];
    if (out->capacity == 0)
        return storage_op_fail(ctx, crates, ERR_BUFFER_TOO_SMALL, waybill);

    /* A file its book holds locked is not a file that is not there: it lies
     * in a rotten range of the Tally, or against the book's own shape. */
    TagFSFileHandle *handle = tagfs_open(file_id, TAGFS_HANDLE_READ);
    if (!handle)
        return storage_op_fail(ctx, crates, TagFSFileLocked(file_id) ? ERR_TAGFS_CORRUPTED
                                                                      : ERR_FILE_NOT_FOUND, waybill);
    handle->offset = offset;

    /* Where the bytes land: the program's pages, or — for a kernel caller
     * with no cabin — the crate's own address. */
    ReadJobAsk ask;
    memset(&ask, 0, sizeof(ask));
    ask.file_id    = file_id;
    ask.out_crate  = out;
    ask.waybill    = waybill;
    ask.sink.bytes = out->capacity;
    if (ctx && ctx->proc && ctx->proc->cabin) {
        ask.sink.vmm  = ctx->proc->cabin->vmm;
        ask.sink.user = (uintptr_t)out->addr;
    } else {
        ask.sink.kernel = (uint8_t *)(uintptr_t)out->addr;
    }

    if (offset < handle->file_size && ctx && ctx->proc &&
        ReadJobSeatRunsAsync(tagfs_get_seat()) &&
        ReadJobSubmit(handle, &ask, crates, ctx) == ERR_WOULD_BLOCK)
        return ERR_WOULD_BLOCK;

    uint64_t got = 0;
    error_t  rc  = TagFSReadInto(handle, &ask.sink, &got);
    tagfs_close(handle);
    out->size = got;

    /* Ф26e: a ferry read that stood in its call must still be answered on the
     * KCTX_STORAGE channel or its awaiter hangs. */
    if (waybill != 0)
        return storage_sync_ferry_finalize(ctx, crates, rc, got, waybill);
    return (int)rc;
}

/* -------------------------------------------------------------------------
 * OBJ_WRITE — in_crate.size bytes into the file at offset, or at its size
 * with STORAGE_WRITE_APPEND. An offset past the size is refused
 * ERR_TAGFS_BEYOND_SIZE. The bytes are taken out of the program's pages a
 * block at a time as the write reaches them — a write is never copied whole
 * into the kernel heap, so its size is bounded by the volume, not the heap.
 * If out_crate is provided and capacity >= 16, the stats are written back on
 * every answer: the bytes the file now holds of the write, and its size.
 *
 * Every write is a WriteJob (write_job.h): it holds the file's Helm for its
 * whole length, and a program whose file another holds is parked until the
 * Helm is handed to its job. A file the system keeps is refused a program
 * whose cabin holds no warrant, and the kernel the loader reads by its blocks
 * every program, by the volume itself (ink.h, InkMayChange), before a byte of
 * the write moves: the deck only says what warrant the call carries
 * (StorageCallWarrant), and asks nothing on its own.
 * ------------------------------------------------------------------------- */

static int ObjWrite(const ManifestOp *op,
                    Crate            *crates,
                    uint16_t          crate_count,
                    const OpContext  *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 20)              return ERR_INVALID_ARGUMENT;

    WriteJobAsk ask;
    memset(&ask, 0, sizeof(ask));
    ask.kind    = WRITE_JOB_WRITE;
    ask.file_id = param_u64(op, 0);
    ask.offset  = param_u64(op, 8);
    ask.append  = (param_u32(op, 16) & STORAGE_WRITE_APPEND) != 0;
    ask.warrant = StorageCallWarrant(ctx);
    /* Ф26e: optional trailing correlation waybill (box::ferry). Absent for a
     * synchronous write (param_size 20) → 0 → plain KCTX_GUIDE completion. */
    ask.waybill = (op->param_size >= 28) ? param_u64(op, 20) : 0;

    Crate *src = &crates[op->in_crate];
    if (src->size == 0 || src->size > src->capacity)
        return storage_op_fail(ctx, crates, ERR_INVALID_ARGUMENT, ask.waybill);

    /* Where the bytes are: the program's pages, walked as the write reaches
     * them, or — for a kernel caller with no cabin — the crate's own address.
     * A page the program has not touched yet is brought in here, in the
     * program's own call, before the job is built: the job pulls where it
     * happens to run, which is not the program's context. A write that cannot
     * have its pages is refused with nothing of it written. */
    ask.source.bytes = src->size;
    if (ctx && ctx->proc && ctx->proc->cabin) {
        ask.source.vmm  = ctx->proc->cabin->vmm;
        ask.source.user = (uintptr_t)src->addr;
    } else {
        ask.source.kernel = (const uint8_t *)(uintptr_t)src->addr;
    }
    error_t pages = PlotSourceBringIn(&ask.source);
    if (pages != OK)
        return storage_op_fail(ctx, crates, pages, ask.waybill);

    /* Optional output stats crate [u64 bytes_written][u64 new_file_size],
     * page-walked on either road, so a stats crate that straddles a page is
     * safe. */
    if (op->out_crate != CRATE_INDEX_NONE && crates[op->out_crate].capacity >= 16)
        ask.out_crate = &crates[op->out_crate];

    return WriteJobSubmit(&ask, crates, ctx);
}

/* -------------------------------------------------------------------------
 * OBJ_DELETE — the file given up (a Quitclaim). A file the system keeps —
 * its record puts it in a role (system, boot or kernel) — is refused ERR_PERMISSION_DENIED to a program
 * whose cabin holds no warrant, the kernel the loader reads by its blocks to
 * every program, by the volume's own guard (InkMayChange); and so is every
 * delete once the medium left: the answer is the volume's. A delete whose
 * Quitclaim finds no Spare to close the tail onto, while a cycle would make
 * one, waits for that cycle's end and is asked again (SettleOpForRoom).
 * ------------------------------------------------------------------------- */

static int ObjDelete(const ManifestOp *op,
                     Crate            *crates,
                     uint16_t          crate_count,
                     const OpContext  *ctx)
{
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    int rc = tagfs_delete_file(param_u64(op, 0), StorageCallWarrant(ctx));
    if (-rc == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, param_u64(op, 0));
    return rc == 0 ? OK : -rc;
}

/* -------------------------------------------------------------------------
 * OBJ_KEEP — params = [u64 file_id][u64 term seconds]: the file deleted for a
 * term, or given up at once for a term of 0 (tagfs_keep_file). Guarded as
 * OBJ_DELETE is, by the volume's own guard and the caller's warrant; a
 * give-up that finds no Spare waits for the cycle that makes one and is asked
 * again, as OBJ_DELETE's does. OBJ_RESTORE — params = [u64 file_id]: the file
 * brought back until its term ends (tagfs_restore_file).
 * ------------------------------------------------------------------------- */

static int ObjKeep(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    if (op->param_size < 16) return ERR_INVALID_ARGUMENT;

    int rc = tagfs_keep_file(param_u64(op, 0), param_u64(op, 8), StorageCallWarrant(ctx));
    if (-rc == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, param_u64(op, 0));
    return rc == 0 ? OK : -rc;
}

static int ObjRestore(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crates;
    (void)crate_count;
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    int rc = tagfs_restore_file(param_u64(op, 0), StorageCallWarrant(ctx));
    return rc == 0 ? OK : -rc;
}

/* -------------------------------------------------------------------------
 * OBJ_SPAN — params = [u64 file_id][u64 size][u32 flags]: the file made
 * exactly `size` bytes long (HelmSpanWindow). Shorter cuts the tail; longer
 * reads as zeros from the old size on, grown a window at a time. With
 * STORAGE_SPAN_GROW the file is grown to `size` only if it is shorter, decided
 * under its Helm — a writer that spans to where its write begins never cuts
 * back what another writer added meanwhile (Z16-1). flags may be left off: 0.
 * A flag this deck does not know is refused ERR_INVALID_ARGUMENT. A WriteJob,
 * as a write is: it holds the file's Helm, and waits for it parked.
 *
 * Guarded like OBJ_DELETE, by the same one guard and the same warrant:
 * cutting a file the system keeps down to nothing destroys it just as
 * thoroughly as deleting it, and the span's load of the file refuses it
 * (PlotLoad).
 *
 * The change is published on every tag of the file (op 3, write_job.c): a
 * content change that published nothing would be a hole in the Touch spine —
 * an observer would have to poll to notice, which is the thing Touch exists
 * to remove.
 * ------------------------------------------------------------------------- */

static int ObjSpan(const ManifestOp *op,
                   Crate            *crates,
                   uint16_t          crate_count,
                   const OpContext  *ctx)
{
    (void)crate_count;
    if (op->param_size < 16) return ERR_INVALID_ARGUMENT;
    uint32_t flags = op->param_size >= 20 ? param_u32(op, 16) : 0;
    if (flags & ~STORAGE_SPAN_GROW) return ERR_INVALID_ARGUMENT;

    WriteJobAsk ask;
    memset(&ask, 0, sizeof(ask));
    ask.kind      = WRITE_JOB_SPAN;
    ask.file_id   = param_u64(op, 0);
    ask.size      = param_u64(op, 8);
    ask.grow_only = (flags & STORAGE_SPAN_GROW) != 0;
    ask.warrant   = StorageCallWarrant(ctx);
    return WriteJobSubmit(&ask, crates, ctx);
}

/* -------------------------------------------------------------------------
 * OBJ_RENAME — params carry the new name inline. The old name's tag comes
 * off with the old name, and the new name's goes on: tags like any other,
 * asking no warrant.
 * ------------------------------------------------------------------------- */

static int ObjRename(const ManifestOp *op,
                     Crate            *crates,
                     uint16_t          crate_count,
                     const OpContext  *ctx)
{
    if (op->param_size < 10) return ERR_INVALID_ARGUMENT;

    uint64_t file_id      = param_u64(op, 0);
    uint16_t new_name_len = param_u16(op, 8);
    if (new_name_len == 0)                           return ERR_INVALID_ARGUMENT;
    if (op->param_size < 10u + (uint32_t)new_name_len) return ERR_INVALID_ARGUMENT;

    char name[64];
    if (new_name_len >= sizeof(name)) return ERR_BUFFER_TOO_SMALL;
    memcpy(name, op->params + 10, new_name_len);
    name[new_name_len] = '\0';

    /* The volume's own answer: ERR_TAGFS_RECORD_FULL for a name that would
     * take the room a write in flight claimed (RT-19) among them. Room on its
     * way — the name's tag for the registry, a Spare for the tail — is waited
     * for (SettleOpForRoom). */
    int rc = tagfs_rename_file(file_id, name);
    if (-rc == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, file_id);
    return rc == 0 ? OK : -rc;
}

/* -------------------------------------------------------------------------
 * OBJ_GET_INFO — serialize file metadata into out_crate.
 *
 * Every tag goes out whole, however long: its key and value are copied
 * straight out of the volume's registry into the answer. What a file is —
 * a note, anything — is the tags it wears; the answer has no flags.
 *
 * Layout written into out_crate (little-endian):
 *   [u64 file_id]
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
static uint64_t info_tag_record(uint8_t *kp, uint64_t pos, uint32_t tag_id)
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
    if (op->param_size < 8)                return ERR_INVALID_ARGUMENT;

    uint64_t file_id = param_u64(op, 0);
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
    uint64_t need = 8u + 8u + 2u + 2u + filename_len + 8u;
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
        /* What a file is — a note, anything else — is the tags it
         * wears, and only they say it: the answer carries no bits beside them
         * but one the volume's own word, the time a deleted file is kept until. */
        memcpy(kp + pos, &file_id,    8); pos += 8;   /* the id asked: the record's own */
        memcpy(kp + pos, &md.size,    8); pos += 8;
        memcpy(kp + pos, &tag_count,  2); pos += 2;
        memcpy(kp + pos, &filename_len, 2); pos += 2;
        if (filename_len) { memcpy(kp + pos, md.filename, filename_len); pos += filename_len; }
        for (uint32_t i = 0; i < tag_count; i++)
            pos = info_tag_record(kp, pos, md.tag_ids[i]);
        /* The last eight bytes: when a deleted file is let go, 0 for one that is not. */
        const uint64_t kept_until = (md.flags & TAGFS_FILE_DELETED) ? md.keep_until : 0;
        memcpy(kp + pos, &kept_until, 8); pos += 8;
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
static error_t storage_scope_of(const ManifestOp *op, size_t at, bool deleted_too, uint8_t *out_scope)
{
    uint8_t scope = STORAGE_SCOPE_USE;
    if (op->param_size > at) scope = op->params[at];
    if (scope != STORAGE_SCOPE_USE && scope != STORAGE_SCOPE_EVERYWHERE &&
        !(deleted_too && scope == STORAGE_SCOPE_DELETED))
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
 * The Use Context's spellings point into the context as it stood at the one
 * look this takes — held in *out_use, which the caller gives back with
 * UseListDrop once the spellings are done with; NULL outside the USE scope or
 * with no context. *out_tags is the caller's to kfree; the caller's own tags
 * are the first *out_own of *out_count. All NULL and 0 when there is nothing
 * to ask.
 */
static error_t storage_tags_spelled(uint8_t scope, const char *text, size_t len,
                                    TagSpelling **out_tags, UseList **out_use,
                                    uint32_t *out_own, uint32_t *out_count)
{
    *out_tags  = NULL;
    *out_use   = NULL;
    *out_own   = 0;
    *out_count = 0;

    uint32_t own = 0;
    error_t  rc  = TagListCount(text, len, &own);
    if (rc != OK)
        return rc;
    if (len > 0 && own == 0)
        return ERR_TAGFS_INVALID_TAG;

    UseList *use   = scope == STORAGE_SCOPE_USE ? UseContextHold() : NULL;
    uint32_t n_use = UseListCount(use);
    if (own + n_use == 0) {
        UseListDrop(use);
        return OK;
    }

    TagSpelling *tags = kmalloc(sizeof(TagSpelling) * ((size_t)own + n_use));
    if (!tags) {
        UseListDrop(use);
        return ERR_NO_MEMORY;
    }
    size_t at = 0;
    for (uint32_t i = 0; i < own; i++)
        (void)TagListNext(text, len, &at, &tags[i]);   /* judged whole above */
    for (uint32_t i = 0; i < n_use; i++)
        tags[own + i] = UseListTag(use, i);

    *out_tags  = tags;
    *out_use   = use;
    *out_own   = own;
    *out_count = own + n_use;
    return OK;
}

/*
 * The tags a question sets aside: the comma list in the op's params from byte
 * `at` to their end, spelled in place (the params are the kernel's copy of the
 * manifest). *out is the caller's to kfree; NULL and 0 when the question sets
 * none aside. A tag that does not spell refuses the question, as one of its
 * own tags would.
 */
static error_t storage_without_spelled(const ManifestOp *op, size_t at, TagSpelling **out,
                                       uint32_t *out_count)
{
    *out       = NULL;
    *out_count = 0;
    if (op->param_size <= at)
        return OK;
    const char  *text = (const char *)op->params + at;
    const size_t len  = op->param_size - at;
    uint32_t     n    = 0;
    error_t      rc   = TagListCount(text, len, &n);
    if (rc != OK || n == 0)
        return rc;
    TagSpelling *without = kmalloc(sizeof(TagSpelling) * n);
    if (!without)
        return ERR_NO_MEMORY;
    size_t from = 0;
    for (uint32_t i = 0; i < n; i++)
        (void)TagListNext(text, len, &from, &without[i]);   /* judged whole above */
    *out       = without;
    *out_count = n;
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
 * given as many ids as it has room for, each the u64 the index holds. hdr[0]
 * is how many went in, hdr[1] how many match — counted by the first page, and
 * never less than what went in, since a file made while the pages are taken
 * is delivered too.
 */
static error_t storage_query_into(const TagSpelling *tags, uint32_t count, const TagSpelling *without,
                                  uint32_t without_count, bool kept, const Crate *out, const OpContext *ctx,
                                  uint32_t hdr[2])
{
    uint64_t room_ids = (out->capacity - 8) / sizeof(uint64_t);
    uint32_t most     = room_ids > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)room_ids;
    uint64_t *piece   = kmalloc(STORAGE_QUERY_PIECE * sizeof(uint64_t));
    if (!piece) return ERR_NO_MEMORY;

    error_t  rc    = OK;
    uint64_t from  = 1;
    bool     first = true;
    for (;;) {
        uint32_t  room = most - hdr[0] < STORAGE_QUERY_PIECE ? most - hdr[0] : STORAGE_QUERY_PIECE;
        TagFSPage page;
        rc = kept ? TagFSQueryKeptPage(tags, count, without, without_count, from, piece, room, first, &page)
                  : TagFSQueryPage(tags, count, without, without_count, from, piece, room, first, &page);
        if (rc != OK) break;
        if (first) hdr[1] = page.total;
        first = false;
        if (page.delivered > 0) {
            int wrc = crate_out_commit_at(out, ctx, 8 + (uint64_t)hdr[0] * sizeof(uint64_t),
                                          piece, (uint64_t)page.delivered * sizeof(uint64_t));
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

/* STORAGE_TAG_QUERY  params (optional): [u8 scope][tags set aside] — inside
 *                    the Use Context (absent, STORAGE_SCOPE_USE) or
 *                    STORAGE_SCOPE_EVERYWHERE; after the scope, a comma list
 *                    of tags no file of the answer wears ("key:..." any tag of
 *                    the key). The volume sets aside nothing of its own
 *                    accord but the files it keeps deleted (obj.keep), which
 *                    no scope but STORAGE_SCOPE_DELETED meets — that scope
 *                    asks for them alone, over the whole volume; every tag is
 *                    left out when the asker names it, and only then.
 *                    in_crate (optional): comma-separated tag string
 *                    out_crate: [u32 delivered][u32 total][u64 file_ids[delivered]]
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
    error_t src_rc = storage_scope_of(op, 0, true, &scope);
    if (src_rc != OK) return src_rc;

    TagFSState *state = tagfs_get_state();
    if (!state || !state->initialized) return ERR_NOT_INITIALIZED;

    /* The query's tags, read BEFORE the output bounce buffer is allocated so
     * an early return here cannot leak the kbuf. */
    char  *text = NULL;
    size_t tlen = 0;
    error_t rc = storage_crate_text(op, crates, ctx, &text, &tlen);
    if (rc != OK) return rc;

    TagSpelling *without       = NULL;
    uint32_t     without_count = 0;
    rc = storage_without_spelled(op, 1, &without, &without_count);
    if (rc != OK) {
        crate_buf_free(text);
        return rc;
    }

    TagSpelling *tags  = NULL;
    UseList     *use   = NULL;
    uint32_t     own   = 0;
    uint32_t     total = 0;
    rc = storage_tags_spelled(scope, text, tlen, &tags, &use, &own, &total);
    if (rc != OK) {
        kfree(without);
        crate_buf_free(text);
        return rc;
    }

    uint32_t hdr[2] = { 0, 0 };   /* delivered, total */
    rc = storage_query_into(tags, total, without, without_count, scope == STORAGE_SCOPE_DELETED, out, ctx, hdr);
    kfree(tags);
    kfree(without);
    UseListDrop(use);
    crate_buf_free(text);
    if (rc != OK) return rc;

    /* The header last: a crate whose ids did not all go in says nothing. */
    int crc = crate_out_commit_at(out, ctx, 0, hdr, sizeof(hdr));
    if (crc != OK) return crc;   /* fail closed: leave out->size unset */
    out->size = sizeof(hdr) + (uint64_t)hdr[0] * sizeof(uint64_t);
    return OK;
}

/* The bytes of records the kernel holds at a time on the way to the caller's
 * crate: any one record fits, a tag's text being narrower (TAG_TEXT_WIDTH). */
#define STORAGE_TAG_LIST_PIECE 4096u

/* STORAGE_TAG_LIST  params: [u32 from][u8 scope][tags set aside] (scope and
 *                   the comma list optional, as a query's) — out_crate as
 *                   boxos_decks.h says: the tags the files wear, inside the
 *                   Use Context counted among its files, the files set aside
 *                   not counted.
 *                   The records go into the crate a piece at a time, straight
 *                   from the volume's pages, until the crate is full or the
 *                   tags run out; `next` says where the rest begin. */
static int ObjTagList(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 4 || op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < STORAGE_TAG_LIST_HEAD) return ERR_BUFFER_TOO_SMALL;

    uint8_t scope;
    error_t rc = storage_scope_of(op, 4, false, &scope);
    if (rc != OK) return rc;

    TagFSState *state = tagfs_get_state();
    if (!state || !state->initialized) return ERR_NOT_INITIALIZED;

    TagSpelling *without       = NULL;
    uint32_t     without_count = 0;
    rc = storage_without_spelled(op, 5, &without, &without_count);
    if (rc != OK) return rc;

    TagSpelling *within = NULL;
    UseList     *use    = NULL;
    uint32_t     own    = 0;
    uint32_t     count  = 0;
    rc = storage_tags_spelled(scope, NULL, 0, &within, &use, &own, &count);
    if (rc != OK) {
        kfree(without);
        return rc;
    }

    uint8_t *piece = kmalloc(STORAGE_TAG_LIST_PIECE);
    if (!piece) {
        kfree(within);
        kfree(without);
        UseListDrop(use);
        return ERR_NO_MEMORY;
    }

    uint32_t hdr[2]  = { 0, 0 };   /* delivered, next */
    uint64_t written = STORAGE_TAG_LIST_HEAD;
    uint32_t from    = param_u32(op, 0);
    for (;;) {
        const uint64_t left = out->capacity - written;
        const uint32_t room = left < STORAGE_TAG_LIST_PIECE ? (uint32_t)left : STORAGE_TAG_LIST_PIECE;
        uint32_t used = 0, delivered = 0, next = 0;
        rc = TagFSTagPage(within, count, without, without_count, from, piece, room, &used, &delivered, &next);
        if (rc == ERR_BUFFER_TOO_SMALL && hdr[0] > 0) {
            hdr[1] = next;              /* the crate is full: the rest from here */
            rc     = OK;
            break;
        }
        if (rc != OK) break;
        if (used > 0) {
            int wrc = crate_out_commit_at(out, ctx, written, piece, used);
            if (wrc != OK) {
                rc = (error_t)wrc;
                break;
            }
            written += used;
            hdr[0]  += delivered;
        }
        if (next == 0 || room == left) {
            hdr[1] = next;
            break;
        }
        from = next;
    }
    kfree(piece);
    kfree(within);
    kfree(without);
    UseListDrop(use);
    if (rc != OK) return rc;

    /* The header last: a crate whose records did not all go in says nothing. */
    int crc = crate_out_commit_at(out, ctx, 0, hdr, sizeof(hdr));
    if (crc != OK) return crc;   /* fail closed: leave out->size unset */
    out->size = written;
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

/* STORAGE_TAG_SET  params:[u64 file_id]  in_crate: "key" or "key:value" —
 * the file wears exactly that tag, or the call is refused. Any tag goes on
 * any file, whatever its key: a file's tags are its user's, and none makes a
 * file one the system keeps. Room on its way — a new tag's block of the
 * registry, a Spare for the tail — is waited for (SettleOpForRoom). */
static int ObjTagSet(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    char       *text = NULL;
    TagSpelling tag;
    error_t     rc = storage_one_tag(op, crates, ctx, &text, &tag);
    if (rc != OK) return rc;
    rc = tagfs_add_tag_string(param_u64(op, 0), &tag);
    crate_buf_free(text);
    if (rc == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, param_u64(op, 0));
    return rc;
}

/* STORAGE_TAG_UNSET  params:[u64 file_id]  in_crate: "key" — every tag of the
 * key — or "key:value" — that tag alone. The answer is the volume's own: a
 * tag the file does not wear (ERR_TAGFS_TAG_NOT_FOUND), a file that is not
 * there, a medium that left — never folded into one code a
 * program cannot tell its own mistake by. */
static int ObjTagUnset(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    char       *text = NULL;
    TagSpelling tag;
    error_t     rc = storage_one_tag(op, crates, ctx, &text, &tag);
    if (rc != OK) return rc;
    int removed = tagfs_remove_tag_string(param_u64(op, 0), &tag);
    crate_buf_free(text);
    if (-removed == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, param_u64(op, 0));
    return removed == 0 ? OK : -removed;
}

/* The ids of the tags a file is created with, each interned; the first
 * refusal ends it. *out_ids is the caller's to kfree. */
static error_t storage_intern_all(const TagSpelling *tags, uint32_t count, uint32_t **out_ids)
{
    *out_ids = NULL;
    if (count == 0)
        return OK;
    uint32_t *ids = kmalloc(sizeof(uint32_t) * count);
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

/* A create a book refuses interns none of its tags (InkBookRefuses): the
 * next stitch would lay them down for a file that never was. The room their
 * Coineds take is held while they are interned, in one stay behind the door —
 * the mount the hold was taken on is the one it is let go on. Room on the
 * ribbon on its way is waited for like any room (SettleOpForRoom). */
static error_t StorageCreateInterned(const TagSpelling *tags, uint32_t count, uint32_t **out_ids)
{
    uint64_t held = 0;
    *out_ids = NULL;
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    error_t rc = InkBookRefuses(tags, count, &held);
    if (rc == OK)
        rc = storage_intern_all(tags, count, out_ids);
    InkBookHeldLetGo(held);
    tagfs_leave();
    return rc;
}

/* The record a file named `filename` wearing `tags` would have, judged before
 * any tag is interned: a create refused for its width spends none of the
 * volume's numbers. When the caller's own tags fit and the Use Context's do
 * not, the refusal says it is the context. */
static error_t storage_create_fits(const char *filename, const TagSpelling *tags,
                                   uint32_t own, uint32_t total)
{
    uint32_t worn = 0;
    error_t  rc   = TagFSCreateFits(filename, tags, total, &worn);
    if (rc == ERR_TAGFS_RECORD_FULL && total > own &&
        TagFSCreateFits(filename, tags, own, NULL) == OK)
        kprintf("[USE] '%s' is not made: the context carries more tags than a file on "
                "this volume can hold — %u tags with the file's own and its name's\n",
                filename, worn);
    return rc;
}

/* STORAGE_OBJ_CREATE  params:[char filename[32]] (NUL-padded)
 *                            [u8 scope] (optional) — inside the Use Context
 *                            (absent, STORAGE_SCOPE_USE) or EVERYWHERE
 *                     in_crate (optional): tag string
 *                     out_crate (optional): u64 file_id
 *                     Inside the context the new file is stamped with the
 *                     user's tags as well as its own. Every tag is spelled,
 *                     and the record they make judged, before any is
 *                     interned; a tag that does not spell or intern, or a
 *                     record wider than a file may have (ERR_TAGFS_RECORD_FULL),
 *                     refuses the create: no file is made without a tag it
 *                     was asked to wear, least of all its context's. Room on
 *                     its way — the name's tag for the registry, a Spare for
 *                     the tail — is waited for (SettleOpForRoom). */
static int ObjCreate(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;

    uint8_t scope;
    error_t rc = storage_scope_of(op, 32, false, &scope);
    if (rc != OK) return rc;

    /* The name runs to its NUL inside the 32-byte field, or fills it. */
    size_t field    = op->param_size < 32 ? op->param_size : 32;
    size_t name_len = strnlen((const char *)op->params, field);
    if (name_len == 0) return ERR_INVALID_ARGUMENT;

    char *filename = kmalloc(name_len + 1);
    if (!filename) return ERR_NO_MEMORY;
    memcpy(filename, op->params, name_len);
    filename[name_len] = '\0';

    char  *text = NULL;
    size_t tlen = 0;
    rc = storage_crate_text(op, crates, ctx, &text, &tlen);
    if (rc != OK) {
        kfree(filename);
        return rc;
    }

    TagSpelling *tags    = NULL;
    UseList     *use     = NULL;
    uint32_t     own     = 0;
    uint32_t     total   = 0;
    uint32_t    *tag_ids = NULL;
    rc = storage_tags_spelled(scope, text, tlen, &tags, &use, &own, &total);
    if (rc == OK)
        rc = storage_create_fits(filename, tags, own, total);
    if (rc == OK)
        rc = StorageCreateInterned(tags, total, &tag_ids);
    kfree(tags);
    UseListDrop(use);
    crate_buf_free(text);
    if (rc != OK) {
        kfree(tag_ids);
        kfree(filename);
        if (rc == ERR_WOULD_BLOCK)
            return SettleOpForRoom(op, crates, crate_count, ctx, 0);
        return rc;
    }

    uint64_t made_id = 0;
    int made = tagfs_create_file(filename, tag_ids, total, &made_id);
    kfree(filename);
    kfree(tag_ids);
    if (-made == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, 0);
    if (made != 0) return (int)(error_t)(-made);

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(made_id))
            (void)crate_write(out, ctx, &made_id, sizeof(made_id));
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
        { STORAGE_TAG_LIST,     ObjTagList,      OP_AUTH_APP, "storage.tag.list" },
        { STORAGE_OBJ_READ,     ObjRead,         OP_AUTH_APP, "storage.read"     },
        { STORAGE_OBJ_GET_INFO, ObjGetInfo,      OP_AUTH_APP, "storage.getinfo"  },
        /* Mutating: app+ — TagFS context further restricts what's visible. */
        { STORAGE_TAG_SET,      ObjTagSet,       OP_AUTH_APP, "storage.tag.set"  },
        { STORAGE_TAG_UNSET,    ObjTagUnset,     OP_AUTH_APP, "storage.tag.unset"},
        { STORAGE_OBJ_WRITE,    ObjWrite,        OP_AUTH_APP, "storage.write"    },
        { STORAGE_OBJ_CREATE,   ObjCreate,       OP_AUTH_APP, "storage.create"   },
        { STORAGE_OBJ_DELETE,   ObjDelete,       OP_AUTH_APP, "storage.delete"   },
        { STORAGE_OBJ_KEEP,     ObjKeep,         OP_AUTH_APP, "storage.keep"     },
        { STORAGE_OBJ_RESTORE,  ObjRestore,      OP_AUTH_APP, "storage.restore"  },
        { STORAGE_OBJ_SPAN,     ObjSpan,         OP_AUTH_APP, "storage.span"     },
        { STORAGE_OBJ_RENAME,   ObjRename,       OP_AUTH_APP, "storage.rename"   },
        /* Snapshots: app+, refused on format v1. */
        { STORAGE_SNAP_CREATE,  ObjSnapRefused,  OP_AUTH_APP, "storage.snap.create"},
        { STORAGE_SNAP_DELETE,  ObjSnapRefused,  OP_AUTH_APP, "storage.snap.delete"},
        { STORAGE_SNAP_LIST,    ObjSnapRefused,  OP_AUTH_APP, "storage.snap.list"  },
        { STORAGE_SNAP_INFO,    ObjSnapRefused,  OP_AUTH_APP, "storage.snap.info"  },
        { STORAGE_OBJ_ANCHOR,   ObjAnchor,       OP_AUTH_APP, "storage.anchor"     },
        { STORAGE_OBJ_WRITE_SETTLED, ObjWriteSettled, OP_AUTH_APP, "storage.write.settled" },
        /* Escheat lets a rotten range's files and blocks go for good: system. */
        { STORAGE_OBJ_ESCHEAT,  ObjEscheat,      OP_AUTH_SYSTEM, "storage.escheat" },
        /* Conjugate says whether one file holds another's bytes: system. */
        { STORAGE_OBJ_CONJUGATE, ObjConjugate,   OP_AUTH_SYSTEM, "storage.conjugate" },
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
