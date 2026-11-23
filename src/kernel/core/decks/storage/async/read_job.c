/*
 * ReadJob — a program's read through the port (read_job.h).
 *
 * The job's life:
 *
 *   ReadJobSubmit (the program's call): the door, DMA pages for the run the
 *     seat takes in one command, the caller parked, the first run submitted —
 *     or the answer at once, when the read is refused before a block moves;
 *   the seat's answer (rjob_answered): the status stashed, the job continued
 *     where its seat's answers continue — no allocation, no lock, nothing
 *     dropped: the node it posts lives inside the job;
 *   the pump (rjob_pump): the run's blocks checked and landed in order, the
 *     next run submitted, or the answer given;
 *   the answer (rjob_answer): the handle closed, the DMA pages and the door
 *     given back, the caller told.
 *
 * A run is as many of the file's next blocks as lie in one run on the medium,
 * the read still needs and the seat takes in one command (step 1.11): one
 * command for all of them, where it was one a block.
 */

#include "read_job.h"
#include "chit.h"         /* ChitGive / ChitDue — an answer put off is an answer promised */
#include "crate_stage.h"
#include "op_registry.h"
#include "boardroom.h"
#include "baton.h"        /* the drain core's never-drop continuation */
#include "helm.h"         /* HelmReady — a USB disk's job goes on at a service point */
#include "roadstead.h"
#include "integrity.h"
#include "run.h"          /* TagFSRunBlocks — how much the seat takes in one command */
#include "process.h"
#include "kring.h"        /* KResultPush, and the Result it pushes */
#include "kresult.h"
#include "pmm.h"
#include "vmm.h"
#include "amp.h"
#include "klib.h"

typedef struct ReadJob {
    TagFSFileHandle *handle;          /* closed at the answer */
    ReadSink         sink;
    uint64_t         start;           /* the file offset it reads from */
    uint64_t         total;           /* the bytes it reads: the sink's room, or up to the size */
    uint64_t         landed;          /* bytes that reached the sink */
    uint64_t         block;           /* the run on its way in: its first block ... */
    uint32_t         count;           /* ... its blocks ... */
    uint32_t         head;            /* ... and where the read begins in the first */
    volatile error_t status;          /* the seat's answer, stashed where it came */
    void            *dma_phys;
    uint8_t         *dma_virt;
    uint32_t         dma_blocks;      /* the run the seat takes in one command */
    bool             at_service;      /* goes on at a service point, not the drain core */

    Baton            cq_node;         /* the drain core's continuation */
    HelmWaiter       service;         /* a service point's */
    RoadsteadBerth   berth;           /* where a block waits when the seat has no place */

    /* Who is answered, and how. */
    process_t       *target;          /* pinned; NULL on the stand */
    Crate           *out_crate;
    Crate           *crates_kbuf;     /* the staged crates, the job's from ERR_WOULD_BLOCK on */
    uint64_t         crates_uaddr;
    uint16_t         crate_count;
    uint64_t         waybill;
    uint32_t         submit_cookie;
    uint64_t         file_id;
#ifdef CONFIG_VOLUME_TESTS
    ReadJobStandAnswer *stand;
#endif
} ReadJob;

static void rjob_pump(void *job_);

bool ReadJobSeatRunsAsync(uint8_t seat)
{
    return g_amp.total_cores > 1 && BoardroomSeatCanReadAsync(seat);
}

/* The answer: the handle, the DMA page and the door given back, then the
 * caller told — its crate's size and the Result carry the bytes that landed,
 * a refusal included — through the border every answer passes (ChitAnswer). */
static void rjob_answer(ReadJob *j, error_t rc)
{
    rc = ChitAnswer(rc, j->target ? j->target->pid : 0, j->submit_cookie, "storage.read");
    if (rc != OK)
        kprintf("[Storage] read of file %llu from byte %llu answered %u — %llu of %llu bytes "
                "had landed\n", j->file_id, (unsigned long long)j->start, (unsigned)rc,
                (unsigned long long)j->landed, (unsigned long long)j->total);

    tagfs_close(TagFSHome(), j->handle);
    pmm_free(j->dma_phys, j->dma_blocks);
    tagfs_leave(TagFSHome());

#ifdef CONFIG_VOLUME_TESTS
    if (j->stand) {
        j->stand->rc     = rc;
        j->stand->landed = j->landed;
        __atomic_store_n(&j->stand->answered, 1u, __ATOMIC_RELEASE);
        kfree(j);
        return;
    }
#endif

    if (j->out_crate)
        j->out_crate->size = j->landed;
    /* The staged crates were the job's from ERR_WOULD_BLOCK on: written back
     * and freed here, on every way out — the dispatcher does not. */
    if (j->crates_kbuf)
        crate_stage_commit_and_release(j->crates_kbuf, j->crate_count,
                                       j->target->cabin ? j->target->cabin->vmm : NULL,
                                       j->crates_uaddr);

    Result r;
    memset(&r, 0, sizeof(r));
    r.error_code  = rc;
    r.data_length = (uint32_t)j->landed;
    /* A waybilled (box::ferry) read echoes its token on the storage channel;
     * a plain one answers on the guide's, to its own submit's token. */
    if (j->waybill) {
        r.context   = KCTX_STORAGE;
        r.data_addr = j->waybill;
    } else {
        r.context = KCTX_PACK24(KCTX_GUIDE, j->submit_cookie);
    }
    ChitDue(j->target, j->submit_cookie);
    KResultPush(j->target, &r);
    process_ref_dec(j->target);
    kfree(j);
}

/* The seat's answer to a run: stashed, and the job continued where its
 * seat's answers go on — never run here, which may be an interrupt. */
static void rjob_answered(uint8_t port, uint8_t slot, error_t status, void *ctx)
{
    (void)port;
    (void)slot;
    ReadJob *j = (ReadJob *)ctx;
    j->status  = status;
    if (j->at_service)
        HelmReady(TagFSHome(), &j->service);
    else
        BatonPass(&j->cq_node);
}

/* The blocks of the file's hole at `spot` the read still needs, from file
 * byte `pos` on, landed as zeros — no block of the medium asked. */
static error_t rjob_hole(ReadJob *j, uint64_t pos, const FileSpot *spot)
{
    uint64_t end   = (pos / TAGFS_BLOCK_SIZE + (uint64_t)spot->left + 1u) * TAGFS_BLOCK_SIZE;
    uint64_t bytes = end - pos;
    if (bytes > j->total - j->landed)
        bytes = j->total - j->landed;
    error_t rc = ReadSinkHole(&j->sink, j->landed, j->handle->rec, pos, bytes);
    if (rc == OK)
        j->landed += bytes;
    return rc;
}

/* The next run submitted, or the answer when the read is done or refused
 * before a block of it moves. The file's holes on the way land as zeros here,
 * one Strip at a time, and the step goes on to what follows them. */
static void rjob_step(ReadJob *j)
{
    FileSpot spot;
    for (;;) {
        if (j->landed >= j->total) {
            rjob_answer(j, OK);
            return;
        }
        if (TagFSMediumGone(TagFSHome())) {
            rjob_answer(j, ERR_DEVICE_NOT_READY);
            return;
        }
        uint64_t pos = j->start + j->landed;
        error_t  rc  = ReadLocate(j->handle, pos / TAGFS_BLOCK_SIZE, &spot);
        if (rc == OK && spot.kind == STRIP_HOLE && (rc = rjob_hole(j, pos, &spot)) == OK)
            continue;
        if (rc != OK) {
            rjob_answer(j, rc);
            return;
        }
        j->block = spot.block;
        j->head  = (uint32_t)(pos % TAGFS_BLOCK_SIZE);
        break;
    }
    uint64_t need = (j->head + (j->total - j->landed) + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE;
    if (need > (uint64_t)spot.left + 1)
        need = (uint64_t)spot.left + 1;
    if (need > j->dma_blocks)
        need = j->dma_blocks;
    j->count = (uint32_t)need;

    /* A seat emptied between the look above and the submit refuses it as a
     * seat that is not there: the medium left, and that is the answer. */
    error_t rc = BoardroomReadAsync(tagfs_get_seat(TagFSHome()), tagfs_block_to_sector(TagFSHome(), j->block),
                                    j->count * TAGFS_BLOCK_SECTORS, j->dma_phys, rjob_answered,
                                    j, &j->berth);
    if (rc != OK)
        rjob_answer(j, TagFSMediumGone(TagFSHome()) ? ERR_DEVICE_NOT_READY : rc);
}

/* The run that came in: each block checked against its digest and landed in
 * order, so a block that does not match stops the read after the ones before
 * it; then the next run asked for. Where the job goes on — no TagFS lock held. */
static void rjob_pump(void *job_)
{
    ReadJob *j  = (ReadJob *)job_;
    error_t  rc = j->status;

    /* The program's pages as they are now, through its pinned process: a
     * program gone meanwhile has none, and the block lands nowhere. */
    if (rc == OK && j->target && !j->sink.kernel)
        j->sink.vmm = j->target->cabin ? j->target->cabin->vmm : NULL;
    for (uint32_t i = 0; i < j->count && rc == OK && j->landed < j->total; i++) {
        const uint8_t *data  = j->dma_virt + (size_t)i * TAGFS_BLOCK_SIZE;
        uint32_t       off   = i == 0 ? j->head : 0;
        uint32_t       chunk = TAGFS_BLOCK_SIZE - off;
        if ((uint64_t)chunk > j->total - j->landed)
            chunk = (uint32_t)(j->total - j->landed);
        uint64_t       k     = (j->start + j->landed) / TAGFS_BLOCK_SIZE;
        if (!IntegrityVerify(TagFSHome(), j->block + i, data))
            rc = ERR_CHECKSUM;
        else if ((rc = ReadSinkBlock(&j->sink, j->landed, j->handle->rec, k, data, off, chunk)) == OK)
            j->landed += chunk;
    }
    if (rc != OK) {
        rjob_answer(j, rc);
        return;
    }
    rjob_step(j);
}

static void rjob_run_at_service(HelmWaiter *w)
{
    rjob_pump((uint8_t *)w - __builtin_offsetof(ReadJob, service));
}

int ReadJobSubmit(TagFSFileHandle *handle, const ReadJobAsk *ask, Crate *crates,
                  const struct OpContext *ctx)
{
    bool stand = false;
#ifdef CONFIG_VOLUME_TESTS
    stand = ask->stand != NULL;
#endif
    if (!stand && (!ctx || !ctx->proc))
        return ERR_INVALID_ARGUMENT;
    if (handle->offset >= handle->file_size || ask->sink.bytes == 0)
        return ERR_INVALID_ARGUMENT;

    ReadJob *j = kmalloc(sizeof(*j));
    if (!j)
        return ERR_NO_MEMORY;
    memset(j, 0, sizeof(*j));
    /* The run the seat takes in one command, in one piece, as a command's
     * pages must be: a read that cannot have them stands in its call. */
    j->dma_blocks = TagFSRunBlocks(TagFSHome());
    j->dma_phys   = pmm_alloc(j->dma_blocks, PHYS_TAG_DMA32);
    if (!j->dma_phys) {
        kfree(j);
        return ERR_NO_MEMORY;
    }
    if (!tagfs_enter(TagFSHome())) {
        pmm_free(j->dma_phys, j->dma_blocks);
        kfree(j);
        return ERR_TAGFS_NOT_INITIALIZED;
    }
    if (!stand && !(j->target = process_find_ref(ctx->proc->pid))) {
        tagfs_leave(TagFSHome());
        pmm_free(j->dma_phys, j->dma_blocks);
        kfree(j);
        return ERR_INVALID_STATE;
    }

    uint64_t room = handle->file_size - handle->offset;
    j->handle     = handle;
    j->sink       = ask->sink;
    j->start      = handle->offset;
    j->total      = ask->sink.bytes < room ? ask->sink.bytes : room;
    j->dma_virt   = (uint8_t *)vmm_phys_to_virt((uintptr_t)j->dma_phys);
    j->at_service = BoardroomSeatKind(tagfs_get_seat(TagFSHome())) == BOARD_USB;
    j->file_id    = ask->file_id;
    j->out_crate  = ask->out_crate;
    j->waybill    = ask->waybill;
    j->cq_node.run  = rjob_pump;
    j->cq_node.ctx  = j;
    j->service.run  = rjob_run_at_service;
#ifdef CONFIG_VOLUME_TESTS
    j->stand = ask->stand;
#endif

    if (!stand) {
        /* The staged crates become the job's, and the answer with them: the
         * caller parked BEFORE the first submit — a fast answer that finds it
         * still working would be lost — and the chit given before it too, for
         * the answer may come on another core the moment a run is armed. */
        j->crates_kbuf   = crates;
        j->crates_uaddr  = ctx->crates_uaddr;
        j->crate_count   = ctx->crate_count;
        j->submit_cookie = ctx->submit_cookie;
        process_set_state(ctx->proc, PROC_WAITING);
        ChitGive(ctx, "storage.read", ask->file_id);
    }
    rjob_step(j);
    return ERR_WOULD_BLOCK;
}
