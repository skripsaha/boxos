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
 * it is in the kernel — the calls that speak tags are storage_tags.c's:
 *   OBJ_READ      params: [u64 file_id][u64 offset]
 *   OBJ_WRITE     params: [u64 file_id][u64 offset][u32 flags]
 *   OBJ_SPAN      params: [u64 file_id][u64 size][u32 flags]
 *   OBJ_DELETE    params: [u64 file_id]
 *   OBJ_RENAME    params: [u64 file_id][u16 new_name_len][char new_name[]]
 *
 *   OBJ_READ.out_crate     : payload destination, written a block at a time
 *                            as the read reaches it; size <- the bytes that
 *                            landed, on a refusal too
 *   OBJ_WRITE.in_crate     : payload source, taken out of the program's pages
 *                            a block at a time as the write reaches it
 *   OBJ_WRITE.out_crate    : optional [u64 bytes_written][u64 new_file_size],
 *                            written on a refusal too: the bytes the file's
 *                            inked record holds of the write, and its size
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
#include "storage_tags.h"     /* the calls that speak tags */
#include "tagfs.h"
#include "book_mount.h"   /* a file its book holds locked */
#include "vmm.h"
#include "process.h"
#include "kring.h"
#include "kresult.h"
#include "write_job.h"
#include "read_job.h"  /* a read through the port, and the sink its bytes land in */
#include "ata.h"
#include "touch.h"
#include "plot.h"     /* PlotFind, PlotSource — the write's bytes where they are */
#include "settle_ops.h"  /* anchor, obj.write.settled — the waits for the Settle */

STATIC_ASSERT(STORAGE_HEAD_MOST == TAGFS_HEAD_MOST, "head.query asks as many first bytes as a record keeps");
STATIC_ASSERT(TAGFS_FILE_ROLE_SYSTEM >> 2 == 1 && TAGFS_FILE_ROLE_BOOT >> 2 == 2 && TAGFS_FILE_ROLE_KERNEL >> 2 == 3,
              "obj.getinfo's role is the record's role bits, shifted down");

/* -------------------------------------------------------------------------
 * Crate I/O lives in crate_io.{h,c}: crate_read / crate_write move a fixed
 * payload between a user Crate and a kernel buffer, and crate_in_buf /
 * crate_out_alloc / crate_out_commit / crate_buf_free carry the variable-
 * size bounce-buffer pattern. All page-walk the user range, so a Crate
 * payload that straddles a page boundary is copied across every backing
 * frame — the legacy single-page StorageCrateMap fast path is gone.
 * ------------------------------------------------------------------------- */

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
    TagFSFileHandle *handle = tagfs_open(TagFSHome(), file_id, TAGFS_HANDLE_READ);
    if (!handle)
        return storage_op_fail(ctx, crates, TagFSFileLocked(TagFSHome(), file_id) ? ERR_TAGFS_CORRUPTED
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
        ReadJobSeatRunsAsync(tagfs_get_seat(TagFSHome())) &&
        ReadJobSubmit(handle, &ask, crates, ctx) == ERR_WOULD_BLOCK)
        return ERR_WOULD_BLOCK;

    uint64_t got = 0;
    error_t  rc  = TagFSReadInto(TagFSHome(), handle, &ask.sink, &got);
    tagfs_close(TagFSHome(), handle);
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
 * delete once the medium left: the answer is the volume's. A delete short of
 * room on the ribbon for its stitch, while a cycle would give some back, waits
 * for that cycle's end and is asked again (SettleOpForRoom).
 * ------------------------------------------------------------------------- */

static int ObjDelete(const ManifestOp *op,
                     Crate            *crates,
                     uint16_t          crate_count,
                     const OpContext  *ctx)
{
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    int rc = tagfs_delete_file(TagFSHome(), param_u64(op, 0), StorageCallWarrant(ctx));
    if (-rc == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, param_u64(op, 0));
    return rc == 0 ? OK : -rc;
}

/* -------------------------------------------------------------------------
 * OBJ_KEEP — params = [u64 file_id][u64 term seconds]: the file deleted for a
 * term, or given up at once for a term of 0 (tagfs_keep_file). Guarded as
 * OBJ_DELETE is, by the volume's own guard and the caller's warrant; a
 * give-up short of room waits for the cycle that gives it back and is asked
 * again, as OBJ_DELETE's does. OBJ_RESTORE — params = [u64 file_id]: the file
 * brought back until its term ends (tagfs_restore_file).
 * ------------------------------------------------------------------------- */

static int ObjKeep(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    if (op->param_size < 16) return ERR_INVALID_ARGUMENT;

    int rc = tagfs_keep_file(TagFSHome(), param_u64(op, 0), param_u64(op, 8), StorageCallWarrant(ctx));
    if (-rc == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, param_u64(op, 0));
    return rc == 0 ? OK : -rc;
}

static int ObjRestore(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crates;
    (void)crate_count;
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    int rc = tagfs_restore_file(TagFSHome(), param_u64(op, 0), StorageCallWarrant(ctx));
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
 * OBJ_RENAME — params carry the new name inline. The name is a field of the
 * file's card and no tag: the file wears the tags it wore, and the rename
 * asks no warrant.
 * ------------------------------------------------------------------------- */

/* The ids a head question moves to the caller's crate at a time. */
#define STORAGE_HEAD_PIECE 64u

/* A head question, as one page of it is asked of the volume (HeadPager). */
typedef struct {
    const uint8_t *bytes;
    uint32_t       length;
} HeadAsk;

static error_t HeadPager(const void *ask, uint64_t from, uint64_t *out, uint32_t room, bool want_total,
                         TagFSPage *page)
{
    const HeadAsk *h = ask;
    return TagFSStartingWithPage(TagFSHome(), h->bytes, h->length, from, out, room, want_total, page);
}

/* STORAGE_HEAD_QUERY  in_crate: the bytes a file begins with, 1 to
 *                     STORAGE_HEAD_MOST of them; out_crate: [u32 delivered]
 *                     [u32 total][u64 file_ids[delivered]], as storage.query's.
 *                     What a file is is told by its first bytes, and every
 *                     file's record keeps them: no file is read, and no tag is
 *                     spoken — the question asks nothing of anybody, as a
 *                     file's name and size are anybody's to see. */
static int ObjHeadQuery(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE || op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    const Crate *in  = &crates[op->in_crate];
    Crate       *out = &crates[op->out_crate];
    if (in->size == 0 || in->size > STORAGE_HEAD_MOST) return ERR_INVALID_ARGUMENT;
    if (out->capacity < 8) return ERR_BUFFER_TOO_SMALL;

    uint8_t *bytes = crate_in_buf(in, ctx);
    if (!bytes) return ERR_INVALID_ADDRESS;
    uint64_t      piece[STORAGE_HEAD_PIECE];
    uint32_t      hdr[2] = { 0, 0 };   /* delivered, total */
    const HeadAsk ask    = { bytes, (uint32_t)in->size };
    error_t       rc     = StorageAnswerPages(HeadPager, &ask, 1, out, ctx, piece, STORAGE_HEAD_PIECE, hdr);
    crate_buf_free(bytes);
    if (rc != OK) return rc;
    int crc = crate_out_commit_at(out, ctx, 0, hdr, sizeof(hdr));
    if (crc != OK) return crc;
    out->size = sizeof(hdr) + (uint64_t)hdr[0] * sizeof(uint64_t);
    return OK;
}

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

    /* The volume's own answer. Room on its way — room on the ribbon for the
     * rename's stitch — is waited for (SettleOpForRoom). */
    int rc = tagfs_rename_file(TagFSHome(), file_id, name);
    if (-rc == ERR_WOULD_BLOCK)
        return SettleOpForRoom(op, crates, crate_count, ctx, file_id);
    return rc == 0 ? OK : -rc;
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
        { STORAGE_NAME_QUERY,   ObjNameQuery,    OP_AUTH_APP, "storage.name.query" },
        { STORAGE_HEAD_QUERY,   ObjHeadQuery,    OP_AUTH_APP, "storage.head.query" },
        { STORAGE_OBJ_READ,     ObjRead,         OP_AUTH_APP, "storage.read"     },
        { STORAGE_OBJ_GET_INFO, ObjGetInfo,      OP_AUTH_APP, "storage.getinfo"  },
        { STORAGE_TAG_SHOWN,    ObjTagShown,     OP_AUTH_APP, "storage.tag.shown" },
        /* Mutating: app+ — TagFS context further restricts what's visible. */
        { STORAGE_TAG_SET,      ObjTagSet,       OP_AUTH_APP, "storage.tag.set"  },
        { STORAGE_TAG_UNSET,    ObjTagUnset,     OP_AUTH_APP, "storage.tag.unset"},
        { STORAGE_TAG_SHOW,     ObjTagShow,      OP_AUTH_APP, "storage.tag.show" },
        { STORAGE_OBJ_WRITE,    ObjWrite,        OP_AUTH_APP, "storage.write"    },
        { STORAGE_OBJ_CREATE,   ObjCreate,       OP_AUTH_APP, "storage.create"   },
        { STORAGE_OBJ_DELETE,   ObjDelete,       OP_AUTH_APP, "storage.delete"   },
        { STORAGE_OBJ_KEEP,     ObjKeep,         OP_AUTH_APP, "storage.keep"     },
        { STORAGE_OBJ_RESTORE,  ObjRestore,      OP_AUTH_APP, "storage.restore"  },
        { STORAGE_OBJ_SPAN,     ObjSpan,         OP_AUTH_APP, "storage.span"     },
        { STORAGE_OBJ_RENAME,   ObjRename,       OP_AUTH_APP, "storage.rename"   },
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
