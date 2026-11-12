/*
 * The waits for the volume's Settle a program asks for — and the one it does
 * not ask for, but meets: room on its way.
 *
 *   OBJ_ANCHOR          params: [u64 file_id]
 *   OBJ_WRITE_SETTLED   params: [u64 file_id]
 *   use (system deck)   the Use Context remembered by the volume
 *   room                an operation of the deck the volume answered
 *                       ERR_WOULD_BLOCK, asked again after the cycle's end
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
 *
 * ‼ ROOM ON ITS WAY IS WAITED FOR, AND THE OPERATION ASKED AGAIN (RT-06/07,
 * RT-17). A create, a rename, a tag put on or taken off, a delete: each is a
 * change the volume refuses ERR_WOULD_BLOCK, nothing of it done, while it is
 * short of ground a Settle cycle gives back — a block for the registry to
 * grow by, a Spare for the pool's tail. The program waits behind its Chit for
 * the next cycle's end, and a service point then asks the very op again —
 * its params as they were asked, its crates the program's still — through the
 * deck's own handler (SettleOpForRoom). Its answer is the program's, or it
 * waits for the next cycle's end again. A caller in the kernel with no
 * program to park waits for the cycles in its own call.
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
#include "escheat.h"
#include "conjugate.h"
#include "async/write_job.h"   /* StorageCallWarrant */
#include "helm.h"        /* HelmReady — an op waited for room is asked again at a service point */
#include "amp.h"

typedef enum {
    SETTLE_OP_ANCHOR = 0,
    SETTLE_OP_SETTLED,
    SETTLE_OP_USE,
    SETTLE_OP_ROOM,
} SettleOpKind;

typedef struct {
    SettleWaiter w;              /* first: the waiter is the wait */
    process_t*   target;         /* pinned until the answer; NULL for the stand's */
    const char*  holder;         /* who owes the answer: the op's registered name */
    uint32_t     submit_cookie;
    uint64_t     file_id;
    uint8_t      kind;           /* SettleOpKind */
    uint64_t     ticket;         /* USE: the demand the word is (UseMemorySay) */
    UseText*     said;           /* USE: the word, held until the answer */
    Crate*       out;            /* USE: the out crate, inside the staged crates */
    Crate*       crates_kbuf;    /* the staged crates, released with the answer */
    uint16_t     crate_count;
    uintptr_t    crates_uaddr;
    ManifestOp*  op;             /* ROOM: the op as it was asked, its params with it */
    HelmWaiter   service;        /* ROOM: where it is asked again */
    error_t      came;           /* ROOM: what the cycle's end answered */
    bool         again;          /* ROOM: asked again, it met room on its way again */
#ifdef CONFIG_VOLUME_TESTS
    SettleOpStandAnswer* stand;  /* the stand's caller: its answer told here */
#endif
} SettleOpWait;

static uint64_t op_file_id(const ManifestOp *op)
{
    uint64_t file_id = 0;
    if ((size_t)op->param_size >= sizeof(file_id))
        memcpy(&file_id, op->params, sizeof(file_id));
    return file_id;
}

/* The anchor's Touch fan-out: every tag of the file, and the bare "anchor" tag
 * that generic listeners hold — resolved by the reader door, never named into
 * the Logbook, so publisher and subscriber meet on the same id whoever spoke
 * first (box::tagfs::on_anchor subscribes by name). */
static void AnchorPublish(uint64_t file_id, uint32_t pid)
{
    struct __attribute__((packed)) {
        uint64_t file_id;
        uint8_t  op;          /* 2 = ANCHOR */
        uint8_t  _pad[7];
        uint64_t now_us;
    } ev = { file_id, 2, {0}, 0 };

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
static void SettleOpSay(uint8_t kind, uint64_t file_id, uint32_t pid, error_t rc)
{
    if (rc == OK) {
        if (kind == SETTLE_OP_ANCHOR)
            AnchorPublish(file_id, pid);
        return;
    }
    kprintf("[Storage] %s of file %llu: the volume's changes were not settled (error %u) — %s\n",
            kind == SETTLE_OP_ANCHOR ? "anchor" : "the wait for the write settled",
            (unsigned long long)file_id,
            (unsigned)rc, kind == SETTLE_OP_ANCHOR ? "no anchor event is published"
                                                   : "the caller is told");
}

/* The answer, from the cycle's S6 or a service point, with no TagFS lock held,
 * through the border every answer passes (ChitAnswer): the out crate and the
 * crates back to the program's pages, then the Result — the Chit falls due
 * and is kept by the push. A `use` is told whether the volume remembers, and
 * is never refused: its context stands either way. An op asked again for room
 * answers what it answered. */
static void SettleOpAnswered(SettleWaiter* w, error_t rc)
{
    SettleOpWait*  s      = (SettleOpWait*)w;
    uint32_t       pid    = s->target ? s->target->pid : 0;
    vmm_context_t* vmm    = s->target && s->target->cabin ? s->target->cabin->vmm : NULL;
    error_t        answer = rc;

    if (s->kind == SETTLE_OP_USE) {
        uint8_t kept = (rc == OK && UseMemoryKept(s->ticket, s->said)) ? 1u : 0u;
        UseTextDrop(s->said);
        if (s->out && vmm && vmm_user_buf_commit_out(vmm, (uintptr_t)s->out->addr, &kept, 1) == OK)
            s->out->size = 1;
        answer = OK;
    } else if (s->kind != SETTLE_OP_ROOM) {
        SettleOpSay(s->kind, s->file_id, pid, rc);
    }
    answer = ChitAnswer(answer, pid, s->submit_cookie, s->holder);
    kfree(s->op);

#ifdef CONFIG_VOLUME_TESTS
    if (s->stand) {
        s->stand->rc = answer;
        __atomic_store_n(&s->stand->answered, 1u, __ATOMIC_RELEASE);
        kfree(s);
        return;
    }
#endif
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
                                  uint64_t file_id, const char *holder)
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
    s->holder        = holder;
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
static int SettleOpWaitFor(const OpContext *ctx, Crate *crates, uint64_t file_id, uint64_t gen,
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
    uint64_t file_id = op_file_id(op);
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    int rc = SettleOpWaitFor(ctx, crates, file_id, InkedGen(), SETTLE_OP_ANCHOR,
                             "storage.anchor");
    tagfs_leave();
    return rc;
}

/* OBJ_ESCHEAT: the act asked with the cabin's warrant as it stands now — what
 * it lets go into the out crate. Nothing waits: the next Tally owes it. */
int ObjEscheat(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    EscheatSaid said;
    (void)crate_count;
    error_t rc = TagFSEscheat(StorageCallWarrant(ctx), &said);
    if (rc == OK && op->out_crate != CRATE_INDEX_NONE)
        (void)crate_write(&crates[op->out_crate], ctx, &said, sizeof(said));
    return rc;
}

/* OBJ_CONJUGATE: the act asked with the cabin's warrant as it stands now — the
 * verdict into the out crate, a refusal's as much as the blocks laid. The
 * ribbon's room on its way is no answer: nothing was laid, and the act is
 * asked again — compared again — after the cycle's end. */
int ObjConjugate(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    ConjugateAsk  ask;
    ConjugateSaid said;
    if (op->param_size < sizeof(ask))
        return ERR_INVALID_ARGUMENT;
    memcpy(&ask, op->params, sizeof(ask));
    error_t rc = TagFSConjugate(StorageCallWarrant(ctx), &ask, &said);
    if (rc == ERR_WOULD_BLOCK && said.why == CONJUGATE_REFUSED)
        return SettleOpForRoom(op, crates, crate_count, ctx, ask.fid);
    if (op->out_crate != CRATE_INDEX_NONE)
        (void)crate_write(&crates[op->out_crate], ctx, &said, sizeof(said));
    return rc;
}

int ObjWriteSettled(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                    const OpContext *ctx)
{
    (void)crate_count;
    uint64_t file_id = op_file_id(op);
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

// ----------------------------------------------------------------------------
// Room on its way
// ----------------------------------------------------------------------------

/* Where the op this core asks again raises its word when it meets room on its
 * way again: the wait it is asked for is linked again — or its caller in the
 * kernel waits again — and no new wait is made beneath it. NULL while this
 * core asks nothing again. */
static bool* s_asked_again[MAX_CORES];

#ifdef CONFIG_VOLUME_TESTS
/* The record the next op parked for room by a caller with no program tells its
 * answer to — the stand's, armed once. */
static SettleOpStandAnswer* volatile s_stand_answer;

void SettleOpStandAnswerTo(SettleOpStandAnswer* a)
{
    __atomic_store_n(&s_stand_answer, a, __ATOMIC_RELEASE);
}
#endif

/* The op asked again through the deck's own handler, as `ctx` asks it: the
 * handler's answer, and *again raised when it met room on its way again. */
static int SettleOpAskAgain(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                            const OpContext *ctx, bool *again)
{
    const OpRegistration* reg  = OpRegistryLookup(op->op_kind);
    uint8_t               core = amp_get_core_index();

    *again = false;
    if (!reg)
        return ERR_NOT_INITIALIZED;
    s_asked_again[core] = again;
    int rc = reg->handler(op, crates, crate_count, ctx);
    s_asked_again[core] = NULL;
    return rc;
}

/* A caller in the kernel with no program to park waits for the cycles in its
 * own call, inside the volume's door, and asks its op again after each. */
static int SettleOpRoomInCall(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                              const OpContext *ctx)
{
    for (;;) {
        if (!tagfs_enter())
            return ERR_TAGFS_NOT_INITIALIZED;
        error_t room = SettleForRoom();
        tagfs_leave();
        if (room != OK)
            return (int)room;
        bool again;
        int  rc = SettleOpAskAgain(op, crates, crate_count, ctx, &again);
        if (rc != ERR_WOULD_BLOCK || !again)
            return rc;
    }
}

static void SettleOpRoomAgain(HelmWaiter *service);

/* The cycle's end the op waited for — or, refused for good, the answer that
 * ends the wait (a medium that left, one that takes no write): noted, and the
 * op listed for a service point, which asks it again. */
static void SettleOpRoomCame(SettleWaiter *w, error_t rc)
{
    SettleOpWait* s = (SettleOpWait*)w;
    s->came = rc;
    HelmReady(&s->service);
}

/* Linked for the next cycle's end, inside the volume's door: OK, or — with no
 * volume mounted — the answer. The wait is room asked for, as the allocator's
 * refusal was — a cycle refused for want of room before it was linked waits
 * for exactly that (SettleWaitersPark). A cycle on another core may answer the
 * wait the moment it is linked: it is not touched again here. */
static error_t SettleOpRoomLink(SettleOpWait *s)
{
    if (!tagfs_enter())
        return ERR_TAGFS_NOT_INITIALIZED;
    SettleDemand(SETTLE_DEMAND_ROOM);
    SettleWaitLinkCycle(&s->w, SettleOpRoomCame);
    tagfs_leave();
    return OK;
}

/* At a service point: the op asked again as its program asked it, and its
 * answer given — or, meeting room on its way again, the wait linked again. An
 * Ink the op makes is its program's own (TagFSNoteInkerSince). */
static void SettleOpRoomAgain(HelmWaiter *service)
{
    SettleOpWait* s  = container_of(service, SettleOpWait, service);
    error_t       rc = s->came;

    if (rc == OK) {
        bool      owns = false;
        OpContext ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.proc              = s->target;
        ctx.submit_cookie     = s->submit_cookie;
        ctx.crate_count       = s->crate_count;
        ctx.crates_uaddr      = s->crates_uaddr;
        ctx.async_owns_crates = &owns;

        uint64_t inked = TagFSInkedHere();
        rc = (error_t)SettleOpAskAgain(s->op, s->crates_kbuf, s->crate_count,
                                       s->target ? &ctx : NULL, &s->again);
        if (s->target)
            TagFSNoteInkerSince(s->target, inked);
        if (rc == ERR_WOULD_BLOCK && s->again) {
            rc = SettleOpRoomLink(s);
            if (rc == OK)
                return;
        }
    }
    SettleOpAnswered(&s->w, rc);
}

int SettleOpForRoom(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                    const OpContext *ctx, uint64_t file_id)
{
    bool* again = s_asked_again[amp_get_core_index()];
    if (again) {
        *again = true;
        return ERR_WOULD_BLOCK;
    }

    bool program = ctx && ctx->proc;
#ifdef CONFIG_VOLUME_TESTS
    SettleOpStandAnswer* stand = NULL;
    if (!program)
        stand = __atomic_exchange_n(&s_stand_answer, NULL, __ATOMIC_ACQ_REL);
    bool parks = program || stand;
#else
    bool parks = program;
#endif
    if (!parks)
        return SettleOpRoomInCall(op, crates, crate_count, ctx);

    const OpRegistration* reg  = OpRegistryLookup(op->op_kind);
    size_t                size = sizeof(ManifestOp) + op->param_size;
    ManifestOp*           copy = reg ? kmalloc(size) : NULL;
    if (!copy)
        return reg ? ERR_NO_MEMORY : ERR_NOT_INITIALIZED;
    memcpy(copy, op, size);

    SettleOpWait* s;
    if (program) {
        s = SettleOpPark(ctx, crates, SETTLE_OP_ROOM, file_id, reg->name);
    } else {
        s = kmalloc(sizeof(SettleOpWait));
        if (s) {
            memset(s, 0, sizeof(*s));
            s->holder      = reg->name;
            s->kind        = SETTLE_OP_ROOM;
            s->file_id     = file_id;
            s->crates_kbuf = crates;
            s->crate_count = crate_count;
#ifdef CONFIG_VOLUME_TESTS
            s->stand       = stand;
#endif
        }
    }
    if (!s) {
        kfree(copy);
        return ERR_NO_MEMORY;
    }
    s->op          = copy;
    s->service.run = SettleOpRoomAgain;

    error_t linked = SettleOpRoomLink(s);
    if (linked != OK)
        SettleOpAnswered(&s->w, linked);
    return ERR_WOULD_BLOCK;
}
