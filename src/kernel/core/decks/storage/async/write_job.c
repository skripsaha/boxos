/*
 * WriteJob — the one automaton of a file's changes a program asks for
 * (write_job.h).
 *
 * A synchronous job runs under the file's Helm — the caller's call when the
 * Helm was free, a service point when it was handed on: a span and a defrag in
 * one piece (HelmSpan, HelmDefrag), a write a window at a time
 * (HelmWriteWindow). An asynchronous write is a state machine whose every
 * command goes through the port — an AHCI port's slots or a USB disk's queue —
 * a stretch of the window a command, and which is pumped between two answers
 * where its seat's answers continue (write_job_port.c).
 *
 * ‼ A WRITE IS INKED A WINDOW AT A TIME (RT-16), ON EITHER ROAD. The window is
 * the run the medium takes in one command (HelmWindowBytes), and the part of
 * it that lies in one run on the medium goes out in that one command: its
 * ground is reserved and the record's room claimed before a byte of it moves
 * (RT-19), what landed of it is inked as one record (TagFSInkPlot), and
 * between two windows the job gives its core back while it keeps the Helm — a
 * synchronous job is listed for the next service point (HelmReady), its caller
 * parked, an asynchronous one posts itself behind what its seat's
 * continuations have waiting (wjob_continue). A write of any length holds no
 * more than a window's ground unnamed, and a K-Core for no longer than one
 * window.
 *
 * ‼ ONE ANSWER TO EVERY WRITE (W-ERR): the reason, the bytes the file's inked
 * record holds of the write (u64), and the size it left — in the out crate
 * [u64 written][u64 size] on every answer, a refusal included, and said on the
 * log for a refusal. A refusal on the way — a read of an overlaid block, a
 * block's write, the program's pages, the record — stops the job there: the
 * windows inked before it are the file's, what landed of the window it stopped
 * in is inked and counted, and the ground reserved past it goes back. Whenever
 * the file changed the listeners hear of it. A job that inks nothing gives
 * back everything it took (CR-1), and never a block twice (W-DFREE): what it
 * took is the Plot's Stakes, given back once. A file given up meanwhile is not
 * written further: its Helm is vacated, and the job answers ERR_FILE_NOT_FOUND
 * with its Stakes given back.
 *
 * ‼ A MEDIUM THAT LEFT (RT-13). A job asking for the Helm while the medium is
 * away is answered ERR_DEVICE_NOT_READY, nothing of it done; one that waited
 * in the FIFO from before the departure waits with the volume's open book
 * (mount/away.h) — handed the Helm when the book is taken up, answered with
 * what left with the medium when it is given up. And a window the medium's
 * leaving stopped declares nothing of itself written: what landed of it is
 * not inked, and its ground goes back.
 *
 * ‼ NO FLUSH OF ITS OWN. What a job inked reaches the medium with a Settle
 * cycle — the cycle's first flush covers the data before any record names it
 * (settle.h). A flush after every write was a SYNC CACHE per fwrite and proved
 * nothing about the record, which no cycle had written.
 *
 * ‼ A SPARE IS MADE THE ASYNCHRONOUS JOB'S OWN WAY. A continuation may not
 * stand over the medium, so an Ink with no Spare to close the tail onto answers
 * ERR_WOULD_BLOCK (INK_ASKS_SPARES) and the job stamps one through the port
 * like any block of its own (W_SPARE), hands it to the pool and inks again.
 *
 * ‼ ROOM ON ITS WAY IS WAITED FOR, NEVER ANSWERED (R6). A volume short of
 * ground a Settle cycle gives back answers ERR_WOULD_BLOCK, and nothing of
 * the turn that met it is done — a window, a span, a defrag, or the Spare a
 * window's record needs. A continuation cannot wait for a cycle: the job
 * turns synchronous, from the last window inked. The synchronous job waits
 * for the next cycle's end with its Helm held and its caller parked
 * (SettleWaitLinkCycle), and takes the same turn again at a service point
 * then; no cycle is run inside anybody's call for it. A caller in the kernel
 * standing in its call waits for the cycles itself (HelmRoomCameBack). The
 * word reaches no program as an answer (ChitAnswer).
 *
 * ‼ A USB DISK'S ANSWER IS NOT AN INTERRUPT'S. An AHCI port answers in its
 * completion interrupt, and the job's next step is the drain core's baton. A
 * USB disk runs one command at a time, and its command's own steps are the
 * drain core's batons (xhci_msd.c): its answer comes in one of them — and a
 * caller standing over the same disk on the drain core pumps those batons
 * while it waits, so the job's next step, pumped there, would run beneath
 * that caller. A step that stood over the disk itself there — a page of the
 * block map or of the integrity map brought in, a gather, ground given back
 * and its sector laid on — would moor behind the very caller that pumped it,
 * and the drain core would wait on itself for ever. So on a USB disk the job
 * goes on at a service point (HelmReady), where standing over the medium is
 * allowed, and the port carries only its blocks.
 *
 * Nothing here copies a block for a snapshot: a format-v1 volume takes none
 * (TagFS_SnapshotRefused), so every block is written where the file has it.
 */

#include "write_job_internal.h"
#include "chit.h"        /* ChitGive / ChitDue — an answer put off is an answer promised */
#include "boardroom.h"
#include "baton.h"
#include "crate_stage.h"
#include "crate_io.h"
#include "op_registry.h"
#include "tagfs.h"
#include "helm.h"        /* the file's Helm, and the work its holder does */
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "kring.h"
#include "kresult.h"
#include "klib.h"
#include "amp.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "touch.h"
#include "ink.h"         /* TagFSInkPlot — the one way the file changes */
#include "run.h"         /* TagFSRunBlocks, TagFSDataCostWrite */

/* =========================================================================
 *  The Helm
 * ========================================================================= */

static void wjob_defer(WriteJob *j, const OpContext *ctx);
static bool wjob_answered_later(const WriteJob *j);

/* Who a job's chit and its answer name. */
static const char *const wjob_holder[] = {
    [WRITE_JOB_WRITE]  = "storage.write",
    [WRITE_JOB_SPAN]   = "storage.span",
    [WRITE_JOB_DEFRAG] = "system.fs.defrag",
};

/* The write's next window, synchronously, under the Helm the job holds: from
 * the first byte no window inked yet, where the last one ended. */
static error_t wjob_window(WriteJob *j)
{
    j->handle->offset = j->bytes_inked ? j->start_offset + j->bytes_inked : j->asked_offset;
    error_t rc = HelmWriteWindow(j->handle, &j->source, &j->bytes_inked);
    if (j->bytes_inked)
        j->start_offset = j->handle->offset - j->bytes_inked;
    j->final_size = j->handle->file_size;
    return rc;
}

/* The job's work under the Helm it holds: a span or a defrag whole, a write
 * one window. */
static error_t wjob_work(WriteJob *j)
{
    switch (j->kind) {
    case WRITE_JOB_SPAN:
        return HelmSpan(j->handle, j->span_size, &j->span_was);
    case WRITE_JOB_DEFRAG:
        return HelmDefrag(j->handle);
    default:
        return wjob_window(j);
    }
}

/* The cycle's end the job waited for — or, refused for good, the answer that
 * ends it (a medium that left, one that takes no write): noted, and the job
 * listed for a service point, which takes its turn. Run where the cycle's
 * waiters are answered, with no TagFS lock held. */
static void wjob_room_came(SettleWaiter *w, error_t rc)
{
    WriteJob *j = container_of(w, WriteJob, room);
    j->room_came = rc;
    HelmReady(&j->helm);
}

/* The volume is short of ground a Settle cycle gives back: its caller parked
 * if it was not yet, the job waits for the next cycle's end, its Helm held.
 * The wait is room asked for, as the allocator's refusal was — a cycle refused
 * for want of room before it was linked waits for exactly that
 * (SettleWaitersPark). A cycle on another core may answer it the moment it is
 * linked: the job is not touched again here. */
static void wjob_wait_for_room(WriteJob *j)
{
    if (!j->deferred)
        wjob_defer(j, j->ctx);
    SettleDemand(SETTLE_DEMAND_ROOM);
    SettleWaitLinkCycle(&j->room, wjob_room_came);
}

/* A synchronous job's turn under the Helm it holds: its work, or — when the
 * cycle it waited for was refused for good — that refusal. A write with
 * windows left gives its core back — its caller parked if it was not yet, the
 * job listed for the next service point, and the turn ends there; a caller in
 * the kernel standing in its call has no core to give back, and its windows
 * run on. Room on its way is waited for (wjob_wait_for_room), or, by a caller
 * in the kernel, in its own call. The handler's answer when the job ended
 * inside its caller's call, else ERR_WOULD_BLOCK: the answer is the job's to
 * push. */
static int wjob_turn(WriteJob *j)
{
    error_t came = j->room_came;
    j->room_came = OK;
    if (came != OK)
        return wjob_finalize(j, came);

    for (;;) {
        error_t rc = wjob_work(j);
        if (rc == ERR_WOULD_BLOCK && wjob_answered_later(j)) {
            wjob_wait_for_room(j);
            return ERR_WOULD_BLOCK;
        }
        if (HelmRoomCameBack(&rc))
            continue;
        if (rc != OK || j->kind != WRITE_JOB_WRITE || j->bytes_inked >= j->source.bytes)
            return wjob_finalize(j, rc);
        if (!j->deferred) {
            if (!j->target)
                continue;
            wjob_defer(j, j->ctx);
        }
        HelmReady(&j->helm);   /* the job is the service point's now: not touched again here */
        return ERR_WOULD_BLOCK;
    }
}

/* A job listed for a service point: a synchronous one handed the Helm, or come
 * back for its next window, takes its turn; an asynchronous one whose steps
 * resume here goes on from the state its last answer set. */
static void wjob_run_ready(HelmWaiter *w)
{
    WriteJob *j = container_of(w, WriteJob, helm);
    if (j->async)
        wjob_pump(j);
    else
        (void)wjob_turn(j);
}

/* The medium left while the job waited for the Helm, or it asked while the
 * medium was away: nothing of it was done, and it is answered so, with what
 * left with the medium. */
static void wjob_gone(HelmWaiter *w, uint64_t lost)
{
    WriteJob *j = container_of(w, WriteJob, helm);
    j->medium_left = true;
    j->lost        = lost;
    (void)wjob_finalize(j, ERR_DEVICE_NOT_READY);
}

/* HelmHand's call, on the core that let go, with no TagFS lock held. */
static void wjob_handed(HelmWaiter *w)
{
    WriteJob *j = container_of(w, WriteJob, helm);
    j->holds_helm = true;
    if (j->async) {
        wjob_to(j, W_ALLOC);
        wjob_continue(j);
    } else {
        HelmReady(w);
    }
}

/* =========================================================================
 *  The end — one for every job, however it ran
 * ========================================================================= */

/* A change to a file's bytes, said on every tag it wears — one 32-byte shape
 * for every road that changes them, so one listener reads them all: op 1 a
 * write (the bytes written at offset), op 3 a span (the bytes cut or added
 * from offset, the old or the new size, whichever is less). A defrag moves the
 * bytes and changes none of them, and says nothing. */
static void wjob_publish(const WriteJob *j, error_t rc)
{
    uint8_t  op;
    uint64_t offset, bytes, final_size;
    switch (j->kind) {
    case WRITE_JOB_WRITE:
        if (j->bytes_inked == 0)
            return;
        op = 1, offset = j->start_offset, bytes = j->bytes_inked, final_size = j->final_size;
        break;
    case WRITE_JOB_SPAN:
        if (rc != OK || j->span_size == j->span_was)
            return;
        op         = 3;
        offset     = j->span_size < j->span_was ? j->span_size : j->span_was;
        bytes      = j->span_size < j->span_was ? j->span_was - j->span_size
                                                : j->span_size - j->span_was;
        final_size = j->span_size;
        break;
    default:
        return;
    }
    if (!TouchHasAnyListeners())
        return;

    TagFSMetadata meta;
    memset(&meta, 0, sizeof(meta));
    if (tagfs_get_metadata(j->file_id, &meta) != OK)
        return;
    struct {
        uint32_t file_id;
        uint8_t  op;
        uint8_t  _pad[3];
        uint64_t offset;
        uint64_t bytes;
        uint64_t final_size;
    } __attribute__((packed)) ev = {
        .file_id    = j->file_id,
        .op         = op,
        .offset     = offset,
        .bytes      = bytes,
        .final_size = final_size,
    };
    uint32_t pid = j->target ? j->target->pid : 0;
    for (uint32_t ti = 0; ti < meta.tag_count; ti++)
        TouchPublishId(meta.tag_ids[ti], &ev, sizeof(ev), pid, TOUCH_FLAG_TAGFS);
    tagfs_metadata_free(&meta);
}

/* What the job's out crate carries: a write's [u64 written][u64 size] — the
 * bytes an inked record names, all of them, a prefix a refusal stopped, or
 * none: answering "0 bytes" to an appending writer whose bytes are in the file
 * is exactly what would make it write the same payload twice — and a defrag's
 * [u32 score] once it moved the file. The length, 0 for nothing. */
static uint32_t wjob_out_bytes(const WriteJob *j, error_t rc, uint64_t written,
                               uint8_t out[16])
{
    if (!j->out_crate)
        return 0;
    if (j->kind == WRITE_JOB_WRITE && j->out_crate->capacity >= 16) {
        memcpy(out + 0, &written,       sizeof(uint64_t));
        memcpy(out + 8, &j->final_size, sizeof(uint64_t));
        return 16;
    }
    if (j->kind == WRITE_JOB_DEFRAG && rc == OK && j->out_crate->capacity >= sizeof(uint32_t)) {
        uint32_t score = tagfs_get_fragmentation_score(NULL, NULL);
        memcpy(out, &score, sizeof(score));
        return sizeof(score);
    }
    return 0;
}

/* The answer pushed to the parked caller. The out crate reaches the program's
 * pages first, then the descriptors — the dispatcher handed the staged kbuf
 * to the job when it parked — and only then the Result: a ferry that frees its
 * submission block on completion cannot race it. j->target is pinned for the
 * job's lifetime and commit_out walks the page tables physically (no target
 * CR3 switch). */
static void wjob_answer_pushed(WriteJob *j, error_t rc, uint64_t written)
{
    uint8_t  out[16];
    uint32_t n    = wjob_out_bytes(j, rc, written, out);
    vmm_context_t *vmm = (j->target && j->target->cabin) ? j->target->cabin->vmm : NULL;
    if (n && vmm && vmm_user_buf_commit_out(vmm, (uintptr_t)j->out_crate->addr, out, n) == OK)
        j->out_crate->size = n;
    if (j->crates_kbuf)
        crate_stage_commit_and_release(j->crates_kbuf, j->crate_count, vmm, j->crates_uaddr);

    Result r;
    memset(&r, 0, sizeof(r));
    /* The error the job actually ended on, never a blanket ERR_IO: the
     * caller tells ERR_TAGFS_BEYOND_SIZE from ERR_TAGFS_RECORD_FULL from the
     * medium's own answer by it. */
    r.error_code  = (uint32_t)rc;
    r.data_length = (uint32_t)written;
    r.sender_pid  = 0;
    /* Ф26e: a waybilled (box::ferry) write echoes its correlation token in
     * data_addr and flies the KCTX_STORAGE flag so boxlib routes it to the
     * ferry station; a plain answer keeps KCTX_GUIDE / data_addr==0. */
    if (j->waybill) { r.context = KCTX_STORAGE; r.data_addr = j->waybill; }
    else            { r.context = KCTX_PACK24(KCTX_GUIDE, j->submit_cookie); }

    if (j->target) {
        /* The answer is determined; only its delivery remains, and that is
         * the kernel's own debt. Due here, kept by the push. */
        ChitDue(j->target, j->submit_cookie);
        KResultPush(j->target, &r);
    }
}

/* The answer of a job that ran inside its caller's call: the out crate
 * through the call's own crates, and the handler's return. A ferry is
 * answered on its own channel even so. Best-effort: an out crate the program
 * unmapped meanwhile is left empty, and the work stands. */
static int wjob_answer_now(WriteJob *j, error_t rc, uint64_t written)
{
    uint8_t  out[16];
    uint32_t n = wjob_out_bytes(j, rc, written, out);
    if (n)
        (void)crate_write(j->out_crate, j->ctx, out, n);
    if (j->waybill)
        return storage_sync_ferry_finalize(j->ctx, j->crates_kbuf, rc, written, j->waybill);
    return (int)rc;
}

/* A refusal said once, on the job's way out: a write's with the one answer
 * every write gives (W-ERR), and a job the medium left before it was handed
 * the Helm with what left with the medium. */
static void wjob_say_refused(const WriteJob *j, error_t rc)
{
    static const char *const what[] = {
        [WRITE_JOB_WRITE]  = "write",
        [WRITE_JOB_SPAN]   = "span",
        [WRITE_JOB_DEFRAG] = "defrag",
    };
    if (rc == OK)
        return;
    if (j->medium_left && j->lost)
        kprintf("[Storage] file %u: the medium left while this %s waited for the file's Helm — "
                "nothing of it was done, and %llu change(s) no cycle settled left with the "
                "medium\n", j->file_id, what[j->kind], (unsigned long long)j->lost);
    else if (j->medium_left)
        kprintf("[Storage] file %u: the medium the volume lives on is away, and this %s was "
                "never handed the file's Helm — nothing of it was done\n", j->file_id,
                what[j->kind]);
    else if (j->kind == WRITE_JOB_WRITE)
        kprintf("[Storage] write to file %u refused (error %u): %llu of its %llu byte(s) are the "
                "file's, and the file is %llu bytes\n", j->file_id, (unsigned)rc,
                (unsigned long long)j->bytes_inked, (unsigned long long)j->source.bytes,
                (unsigned long long)j->final_size);
}

#ifdef CONFIG_VOLUME_TESTS
/* The stand's record, filled and then marked answered: the test reads the rest
 * only after the mark, and nothing of the job touches it again. */
static void wjob_tell_stand(WriteJobStandAnswer *a, error_t rc, uint64_t written,
                            uint64_t size, bool through_port)
{
    a->rc           = rc;
    a->written      = written;
    a->size         = size;
    a->through_port = through_port;
    __atomic_store_n(&a->answered, 1u, __ATOMIC_RELEASE);
}
#endif

/* What the job took goes back first — the Stakes nobody inked, on every way
 * out, once — then the listeners hear of what changed, the Helm goes to the
 * next waiter, and the answer is given; the door is let go of last of what
 * the volume lent. The handler's return when the job ran inside its call. */
int wjob_finalize(WriteJob *j, error_t rc)
{
    rc = ChitAnswer(rc, j->target ? j->target->pid : 0, j->submit_cookie, wjob_holder[j->kind]);
    uint64_t written = j->bytes_inked;
    if (written == 0)
        j->final_size = j->loaded ? j->plot.size_before : j->handle->file_size;

    PlotRelease(&j->plot);
    wjob_say_refused(j, rc);
    if (rc != OK && !j->medium_left)
        TagFSWriteRefused();
    /* A job that inked owes the file's newest record — its own last Ink, or a
     * name or tag inked over it since — against its program; one that inked
     * nothing owes nothing. Inside the door, which is let go of below. */
    if (j->target && (written != 0 || (j->kind != WRITE_JOB_WRITE && rc == OK)))
        TagFSNoteInker(j->target, TagFSInkPlace(InkGenOf(j->file_id)));
    wjob_publish(j, rc);
    if (j->kind == WRITE_JOB_WRITE)
        TagFSDataCostWrite();
    if (j->holds_helm)
        HelmLetGo(j->handle->ofe, &j->helm);

    if (j->dma_phys)
        pmm_free(j->dma_phys, j->dma_blocks);
    tagfs_close(j->handle);
    tagfs_leave();

    int answer = OK;
    if (j->deferred)
        wjob_answer_pushed(j, rc, written);
    else
        answer = wjob_answer_now(j, rc, written);

    if (j->target)
        process_ref_dec(j->target);
#ifdef CONFIG_VOLUME_TESTS
    if (j->stand)
        wjob_tell_stand(j->stand, rc, written, j->final_size, j->async);
#endif
    kfree(j);
    return answer;
}

/* =========================================================================
 *  Public entry — WriteJobSubmit
 * ========================================================================= */

bool WriteJobSeatRunsAsync(uint8_t seat)
{
    return g_amp.total_cores > 1 && BoardroomSeatCanReadAsync(seat) &&
           BoardroomSeatCanWriteAsync(seat);
}

/* Whether the job's answer is told later rather than returned from its call:
 * a program it parks, or — on the stand — the record a test brought. */
static bool wjob_answered_later(const WriteJob *j)
{
#ifdef CONFIG_VOLUME_TESTS
    if (j->stand)
        return true;
#endif
    return j->target != NULL;
}

/* A write goes through the port, the job parked on each command's answer,
 * where the seat takes commands nobody stands over and a K-Core pumps what
 * they continue into (WriteJobSeatRunsAsync), for a caller whose answer is
 * told later: a caller standing in its call has nothing to do while it waits
 * but wait. On a USB disk its steps go on at a service point (wjob_continue).
 * Its DMA pages hold the run the seat takes in one command, in one piece, as
 * a command's pages must be; a job that cannot have them is synchronous. */
static void wjob_choose_async(WriteJob *j)
{
    uint8_t seat = tagfs_get_seat();
    if (j->kind != WRITE_JOB_WRITE || !wjob_answered_later(j) || !WriteJobSeatRunsAsync(seat))
        return;
    uint32_t pages = TagFSRunBlocks();
    j->dma_phys = pmm_alloc(pages, PHYS_TAG_DMA32);
    if (!j->dma_phys)
        return;
    j->dma_blocks         = pages;
    j->dma_virt           = vmm_phys_to_virt((uintptr_t)j->dma_phys);
    j->async              = true;
    j->resumes_at_service = BoardroomSeatKind(seat) == BOARD_USB;
}

/* The job for `ask`, its file open for writing and its caller pinned: OK, or
 * the refusal with nothing held. Inside the volume's door. */
static error_t wjob_make(const WriteJobAsk *ask, Crate *crates, const OpContext *ctx,
                         WriteJob **out)
{
    uint32_t flags = TAGFS_HANDLE_WRITE |
                     (ask->kind == WRITE_JOB_WRITE && ask->append ? TAGFS_HANDLE_APPEND : 0);
    WriteJob *j = kmalloc(sizeof(WriteJob));
    if (!j)
        return ERR_NO_MEMORY;
    memset(j, 0, sizeof(*j));

    j->handle = tagfs_open(ask->file_id, flags);
    if (!j->handle) {
        kfree(j);
        return ERR_FILE_NOT_FOUND;
    }
    j->handle->warrant = ask->warrant;
    if (ctx && ctx->proc) {
        j->target = process_find_ref(ctx->proc->pid);
        if (!j->target) {
            tagfs_close(j->handle);
            kfree(j);
            return ERR_INVALID_ARGUMENT;
        }
    }

    j->kind          = ask->kind;
    j->ctx           = ctx;
    j->file_id       = ask->file_id;
    j->out_crate     = ask->out_crate;
    j->source        = ask->source;
    j->asked_offset  = ask->offset;
    j->append        = ask->append;
    j->waybill       = ask->waybill;
    j->span_size     = ask->size;
    j->submit_cookie = ctx ? ctx->submit_cookie : 0;
    j->final_size    = j->handle->file_size;
    j->helm.hand     = wjob_handed;
    j->helm.run      = wjob_run_ready;
    j->helm.gone     = wjob_gone;
    j->cq_node.run   = wjob_pump;
    j->cq_node.ctx   = j;
    j->crates_kbuf   = crates;
    j->crate_count   = ctx ? ctx->crate_count : 0;
    j->crates_uaddr  = ctx ? ctx->crates_uaddr : 0;
#ifdef CONFIG_VOLUME_TESTS
    /* The stand's caller never stands in its call: its answer is the record. */
    j->stand         = ask->stand;
    j->deferred      = ask->stand != NULL;
#endif
    wjob_choose_async(j);
    *out = j;
    return OK;
}

/* The caller is parked and the answer becomes the job's to push — the chit,
 * and with it the staged crates, before the job can be handed the Helm: a job
 * handed it runs at once, and can finish on another core before this call
 * returns. */
static void wjob_defer(WriteJob *j, const OpContext *ctx)
{
    j->deferred = true;
    j->ctx      = NULL;
    process_set_state(ctx->proc, PROC_WAITING);
    ChitGive(ctx, wjob_holder[j->kind], j->file_id);
}

int WriteJobSubmit(const WriteJobAsk *ask, Crate *crates, const OpContext *ctx)
{
    if (!ask || (ask->kind == WRITE_JOB_WRITE && ask->source.bytes == 0))
        return ERR_INVALID_ARGUMENT;
    if (!tagfs_enter()) {
        return (ask->waybill && ctx)
                   ? storage_sync_ferry_finalize(ctx, crates, ERR_TAGFS_NOT_INITIALIZED, 0,
                                                 ask->waybill)
                   : (int)ERR_TAGFS_NOT_INITIALIZED;
    }

    WriteJob *j  = NULL;
    error_t   rc = wjob_make(ask, crates, ctx, &j);
    if (rc != OK) {
        tagfs_leave();
        return (ask->waybill && ctx) ? storage_sync_ferry_finalize(ctx, crates, rc, 0, ask->waybill)
                                     : (int)rc;
    }
    OpenFileEntry *ofe = j->handle->ofe;

    if (!j->async && HelmTry(ofe, &j->helm)) {
        j->holds_helm = true;
        return wjob_turn(j);
    }
    if (!wjob_answered_later(j)) {
        kprintf("[Storage] file %u is being changed by the holder of its Helm, and a caller "
                "with no program to park cannot wait behind it — refused\n", j->file_id);
        return wjob_finalize(j, ERR_BUSY);
    }

    if (!j->deferred)
        wjob_defer(j, ctx);
    wjob_to(j, W_HELM_WAIT);
    if (!HelmAsk(ofe, &j->helm))
        return ERR_WOULD_BLOCK;   /* the job is the Helm's now, or answered: not touched again here */

    j->holds_helm = true;
    if (j->async) {
        /* The first pump in this call's own context: it takes every lock the
         * machine needs and skips a queue round-trip, and parks at the first
         * submit. What it posts from here goes where its seat's steps
         * continue (wjob_continue). */
        wjob_to(j, W_ALLOC);
        wjob_pump(j);
    } else {
        (void)wjob_turn(j);
    }
    return ERR_WOULD_BLOCK;
}
