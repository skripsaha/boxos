/*
 * The waits for the volume's Settle a program asks for.
 *
 *   OBJ_ANCHOR          params: [u32 file_id]
 *   OBJ_WRITE_SETTLED   params: [u32 file_id]
 *   use (system deck)   the Use Context remembered by the volume
 *
 * ‼ A PROGRAM NEVER WAITS INSIDE THE KERNEL FOR A CYCLE. On one core a call
 * runs with interrupts off, and a cycle run inside it would be the medium's
 * whole time with nothing else heard. So the handler looks: what is settled
 * already is answered at once; otherwise the program is parked behind its
 * Chit, a waiter is linked, and the cycle that settles it answers from its S6
 * (settle.h). The medium's lasting refusal answers with its code; one that may
 * pass waits for the seat to say it answers again.
 *
 * An anchor that is answered OK then says so on the file's tags and on the
 * bare "anchor" tag, so an observer keyed by either wakes: a file announced
 * durable is a file that is.
 */

#include "settle_ops.h"
#include "klib.h"
#include "chit.h"
#include "crate_stage.h"
#include "crate_io.h"
#include "kring.h"
#include "kresult.h"
#include "process.h"
#include "touch.h"
#include "vmm.h"
#include "tagfs.h"
#include "settle.h"
#include "ink.h"
#include "use_memory.h"

typedef enum {
    SETTLE_OP_ANCHOR = 0,
    SETTLE_OP_SETTLED,
    SETTLE_OP_USE,
} SettleOpKind;

typedef struct {
    SettleWaiter w;              /* first: the waiter is the wait */
    process_t*   target;         /* pinned until the answer */
    uint32_t     submit_cookie;
    uint32_t     file_id;
    uint8_t      kind;           /* SettleOpKind */
    uint64_t     ticket;         /* USE: the demand the word is (UseMemorySay) */
    UseText*     said;           /* USE: the word, held until the answer */
    Crate*       out;            /* USE: the out crate, inside the staged crates */
    Crate*       crates_kbuf;    /* the staged crates, released with the answer */
    uint16_t     crate_count;
    uintptr_t    crates_uaddr;
} SettleOpWait;

static uint32_t op_file_id(const ManifestOp *op)
{
    uint32_t file_id = 0;
    if ((size_t)op->param_size >= sizeof(file_id))
        memcpy(&file_id, op->params, sizeof(file_id));
    return file_id;
}

/* The anchor's Touch fan-out: every tag of the file, and the bare "anchor" tag
 * that generic listeners hold — resolved by the reader door, never named into
 * the Logbook, so publisher and subscriber meet on the same id whoever spoke
 * first (box::tagfs::on_anchor subscribes by name). */
static void AnchorPublish(uint32_t file_id, uint32_t pid)
{
    struct __attribute__((packed)) {
        uint32_t file_id;
        uint8_t  op;          /* 2 = ANCHOR */
        uint8_t  _pad[3];
        uint64_t now_us;
    } ev = { file_id, 2, {0, 0, 0}, 0 };

    if (file_id != 0 && tagfs_enter()) {
        TagFSMetadata meta;
        memset(&meta, 0, sizeof(meta));
        if (tagfs_get_metadata(file_id, &meta) == OK) {
            for (uint32_t i = 0; i < meta.tag_count; i++)
                TouchPublishId(meta.tag_ids[i], &ev, sizeof(ev), pid, TOUCH_FLAG_TAGFS);
            tagfs_metadata_free(&meta);
        }
        tagfs_leave();
    }
    TouchTag full, bare;
    (void)TouchTagResolve("anchor", sizeof("anchor") - 1, &full, &bare);
    TouchPublishPair(full, bare, &ev, sizeof(ev), pid, TOUCH_FLAG_TAGFS);
}

/* What an anchor or a settled-wait answered said out loud: a refusal, and an
 * anchor's event on OK. */
static void SettleOpSay(uint8_t kind, uint32_t file_id, uint32_t pid, error_t rc)
{
    if (rc == OK) {
        if (kind == SETTLE_OP_ANCHOR)
            AnchorPublish(file_id, pid);
        return;
    }
    kprintf("[Storage] %s of file %u: the volume's changes were not settled (error %u) — %s\n",
            kind == SETTLE_OP_ANCHOR ? "anchor" : "the wait for the write settled", file_id,
            (unsigned)rc, kind == SETTLE_OP_ANCHOR ? "no anchor event is published"
                                                   : "the caller is told");
}

/* The answer, from the cycle's S6 or a service point, with no TagFS lock held:
 * the out crate and the crates back to the program's pages, then the Result —
 * the Chit falls due and is kept by the push. A `use` is told whether the
 * volume remembers, and is never refused: its context stands either way. */
static void SettleOpAnswered(SettleWaiter* w, error_t rc)
{
    SettleOpWait*  s      = (SettleOpWait*)w;
    vmm_context_t* vmm    = s->target->cabin ? s->target->cabin->vmm : NULL;
    error_t        answer = rc;

    if (s->kind == SETTLE_OP_USE) {
        uint8_t kept = (rc == OK && UseMemoryKept(s->ticket, s->said)) ? 1u : 0u;
        UseTextDrop(s->said);
        if (s->out && vmm && vmm_user_buf_commit_out(vmm, (uintptr_t)s->out->addr, &kept, 1) == OK)
            s->out->size = 1;
        answer = OK;
    } else {
        SettleOpSay(s->kind, s->file_id, s->target->pid, rc);
    }

    if (s->crates_kbuf && s->crate_count)
        crate_stage_commit_and_release(s->crates_kbuf, s->crate_count, vmm, s->crates_uaddr);
    Result r;
    memset(&r, 0, sizeof(r));
    r.error_code = (uint32_t)answer;
    r.context    = KCTX_PACK24(KCTX_GUIDE, s->submit_cookie);
    ChitDue(s->target, s->submit_cookie);
    KResultPush(s->target, &r);
    process_ref_dec(s->target);
    kfree(s);
}

/* The program parked behind its Chit and the wait made ready for its waiter —
 * or NULL, and nothing parked, when there is no memory or no program. */
static SettleOpWait* SettleOpPark(const OpContext *ctx, Crate *crates, uint8_t kind,
                                  uint32_t file_id, const char *holder)
{
    SettleOpWait* s = kmalloc(sizeof(SettleOpWait));
    if (!s)
        return NULL;
    memset(s, 0, sizeof(*s));
    s->target = process_find_ref(ctx->proc->pid);
    if (!s->target) {
        kfree(s);
        return NULL;
    }
    s->submit_cookie = ctx->submit_cookie;
    s->file_id       = file_id;
    s->kind          = kind;
    s->crates_kbuf   = crates;
    s->crate_count   = ctx->crate_count;
    s->crates_uaddr  = ctx->crates_uaddr;

    /* Parked and promised before the waiter is linked: a cycle on another
     * core may answer it the moment it is. */
    process_set_state(ctx->proc, PROC_WAITING);
    ChitGive(ctx, holder, file_id);
    return s;
}

/* The wait for Ink `gen`: its answer now, or ERR_WOULD_BLOCK with the program
 * parked and the waiter linked. Inside the door. */
static int SettleOpWaitFor(const OpContext *ctx, Crate *crates, uint32_t file_id, uint64_t gen,
                           uint8_t kind, const char *holder)
{
    uint32_t pid = ctx && ctx->proc ? ctx->proc->pid : 0;
    error_t  now = SettleWaitNow(gen);
    if (now != ERR_WOULD_BLOCK) {
        SettleOpSay(kind, file_id, pid, now);
        return (int)now;
    }

    /* A caller with no program to park — a kernel path through the deck —
     * may wait for the cycle itself. */
    if (!pid) {
        error_t settled = TagFSSettle();
        SettleOpSay(kind, file_id, 0, settled);
        return (int)settled;
    }

    SettleOpWait* s = SettleOpPark(ctx, crates, kind, file_id, holder);
    if (!s)
        return ERR_NO_MEMORY;
    SettleWaitLink(&s->w, gen, SettleOpAnswered);
    return ERR_WOULD_BLOCK;
}

int ObjAnchor(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    uint32_t file_id = op_file_id(op);
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    int rc = SettleOpWaitFor(ctx, crates, file_id, InkedGen(), SETTLE_OP_ANCHOR,
                             "storage.anchor");
    tagfs_leave();
    return rc;
}

int ObjWriteSettled(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                    const OpContext *ctx)
{
    (void)crate_count;
    uint32_t file_id = op_file_id(op);
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    uint64_t gen = file_id ? InkGenOf(file_id) : InkedGen();
    int rc = SettleOpWaitFor(ctx, crates, file_id, gen, SETTLE_OP_SETTLED,
                             "storage.write.settled");
    tagfs_leave();
    return rc;
}

static void SettleUseSayNow(Crate *out, const OpContext *ctx, bool kept)
{
    uint8_t byte = kept ? 1u : 0u;
    if (out)
        (void)crate_write(out, ctx, &byte, 1);
}

int SettleUseRemembered(UseText *said, uint64_t saying, const OpContext *ctx, Crate *crates,
                        Crate *out)
{
    if (!tagfs_enter()) {
        SettleUseSayNow(out, ctx, false);
        return OK;
    }

    uint64_t ticket = 0;
    UseSaid  told   = UseMemorySay(said, saying, &ticket);
    int      rc     = OK;

    if (told != USE_SAID_OWED) {
        SettleUseSayNow(out, ctx, told == USE_SAID_LANDED);
    } else if (!ctx || !ctx->proc) {
        SettleUseSayNow(out, ctx, TagFSSettle() == OK && UseMemoryKept(ticket, said));
    } else {
        SettleOpWait* s = SettleOpPark(ctx, crates, SETTLE_OP_USE, 0, "system.use");
        if (s) {
            s->ticket = ticket;
            s->said   = UseTextHold(said);
            s->out    = out;
            SettleWaitLinkCycle(&s->w, SettleOpAnswered);
            rc = ERR_WOULD_BLOCK;
        } else {
            SettleUseSayNow(out, ctx, false);
        }
    }
    tagfs_leave();
    return rc;
}
