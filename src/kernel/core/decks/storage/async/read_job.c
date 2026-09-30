/*
 * ReadJob — a program's read through the port (read_job.h).
 *
 * The job's life:
 *
 *   ReadJobSubmit (the program's call): the door, a DMA page, the caller
 *     parked, the first block submitted — or the answer at once, when the read
 *     is refused before a block moves;
 *   the seat's answer (rjob_answered): the status stashed, the job continued
 *     where its seat's answers continue — no allocation, no lock, nothing
 *     dropped: the node it posts lives inside the job;
 *   the pump (rjob_pump): the block checked and landed, the next one
 *     submitted, or the answer given;
 *   the answer (rjob_answer): the handle closed, the DMA page and the door
 *     given back, the caller told.
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
    uint32_t         block;           /* the block on its way in ... */
    uint32_t         left;            /* ... and how many of the file's next blocks follow it */
    uint32_t         in_offset;       /* the bytes of it the read takes */
    uint32_t         in_chunk;
    volatile error_t status;          /* the seat's answer, stashed where it came */
    void            *dma_phys;
    uint8_t         *dma_virt;
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
    uint32_t         file_id;
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
 * a refusal included. */
static void rjob_answer(ReadJob *j, error_t rc)
{
    if (rc != OK)
        kprintf("[Storage] read of file %u from byte %llu answered %u — %llu of %llu bytes "
                "had landed\n", j->file_id, (unsigned long long)j->start, (unsigned)rc,
                (unsigned long long)j->landed, (unsigned long long)j->total);

    tagfs_close(j->handle);
    pmm_free(j->dma_phys, 1);
    tagfs_leave();

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

/* The seat's answer to a block: stashed, and the job continued where its
 * seat's answers go on — never run here, which may be an interrupt. */
static void rjob_answered(uint8_t port, uint8_t slot, error_t status, void *ctx)
{
    (void)port;
    (void)slot;
    ReadJob *j = (ReadJob *)ctx;
    j->status  = status;
    if (j->at_service)
        HelmReady(&j->service);
    else
        BatonPass(&j->cq_node);
}

/* The next block submitted, or the answer when the read is done or refused
 * before the block moves. */
static void rjob_step(ReadJob *j)
{
    if (j->landed >= j->total) {
        rjob_answer(j, OK);
        return;
    }
    if (TagFSMediumGone()) {
        rjob_answer(j, ERR_DEVICE_NOT_READY);
        return;
    }

    uint64_t pos    = j->start + j->landed;
    uint32_t offset = (uint32_t)(pos % TAGFS_BLOCK_SIZE);
    uint32_t chunk  = TAGFS_BLOCK_SIZE - offset;
    if ((uint64_t)chunk > j->total - j->landed)
        chunk = (uint32_t)(j->total - j->landed);

    if (j->landed > 0 && j->left > 0) {
        j->block++;
        j->left--;
    } else {
        error_t rc = ReadLocate(j->handle, pos / TAGFS_BLOCK_SIZE, &j->block, &j->left);
        if (rc != OK) {
            rjob_answer(j, rc);
            return;
        }
    }
    j->in_offset = offset;
    j->in_chunk  = chunk;

    error_t rc = BoardroomReadAsync(tagfs_get_seat(), tagfs_block_to_sector(j->block),
                                    TAGFS_BLOCK_SECTORS, j->dma_phys, rjob_answered, j,
                                    &j->berth);
    if (rc != OK)
        rjob_answer(j, rc);
}

/* The block that came in: checked against its digest, landed, and the next
 * one asked for. Where the job goes on — no TagFS lock held. */
static void rjob_pump(void *job_)
{
    ReadJob *j  = (ReadJob *)job_;
    error_t  rc = j->status;

    if (rc == OK && !IntegrityVerify(j->block, j->dma_virt))
        rc = ERR_CHECKSUM;
    if (rc == OK) {
        /* The program's pages as they are now, through its pinned process: a
         * program gone meanwhile has none, and the block lands nowhere. */
        if (j->target && !j->sink.kernel)
            j->sink.vmm = j->target->cabin ? j->target->cabin->vmm : NULL;
        rc = ReadSinkPush(&j->sink, j->landed, j->dma_virt + j->in_offset, j->in_chunk);
    }
    if (rc != OK) {
        rjob_answer(j, rc);
        return;
    }
    j->landed += j->in_chunk;
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
    j->dma_phys = pmm_alloc(1, PHYS_TAG_DMA32);
    if (!j->dma_phys) {
        kfree(j);
        return ERR_NO_MEMORY;
    }
    if (!tagfs_enter()) {
        pmm_free(j->dma_phys, 1);
        kfree(j);
        return ERR_TAGFS_NOT_INITIALIZED;
    }
    if (!stand && !(j->target = process_find_ref(ctx->proc->pid))) {
        tagfs_leave();
        pmm_free(j->dma_phys, 1);
        kfree(j);
        return ERR_INVALID_STATE;
    }

    uint64_t room = handle->file_size - handle->offset;
    j->handle     = handle;
    j->sink       = ask->sink;
    j->start      = handle->offset;
    j->total      = ask->sink.bytes < room ? ask->sink.bytes : room;
    j->dma_virt   = (uint8_t *)vmm_phys_to_virt((uintptr_t)j->dma_phys);
    j->at_service = BoardroomSeatKind(tagfs_get_seat()) == BOARD_USB;
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
         * the answer may come on another core the moment a block is armed. */
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
