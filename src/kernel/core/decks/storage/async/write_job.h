#ifndef WRITE_JOB_H
#define WRITE_JOB_H

#include "ktypes.h"
#include "error.h"
#include "tagfs.h"
#include "boxos_crate.h"
#include "process.h"
#include "baton.h"      /* embedded never-drop MPSC node (cq_node) */
#include "roadstead.h"  /* the job's own berth at the port */

/*
 * WriteJob — async ObjWrite state machine.
 *
 * Stage 4 of storage-async migration. Process parks on PROC_WAITING,
 * IRQ pumps state-machine via K-Core continuation queue, KResultPush
 * wakes when DONE.
 */

struct OpContext;
struct ManifestOp;

typedef enum {
    W_INIT = 0,
    W_TOKEN_WAIT,
    W_LOCATE,
    W_ALLOC,
    W_READ_OLD,        /* async read of the block a partial write overlays */
    W_DMA_FILL,
    W_AHCI_SUBMIT,
    W_AHCI_DONE,
    W_LOG_META,
    W_COMMIT_TXN,
    W_PUBLISH,
    W_RELEASE_TOKEN,
    W_DONE,
    W_ERROR,
} WriteJobState;

typedef struct WriteJob {
    /* ---- identity / target ---- */
    process_t        *target;
    uint32_t          file_id;
    TagFSFileHandle  *handle;
    OpenFileEntry    *ofe;
    Crate            *out_crate;

    /* ---- payload ---- */
    const uint8_t    *src_kp;            /* kernel pointer to user bytes */
    void             *src_bounce;        /* if != NULL, kfree at W_DONE */
    uint64_t          start_offset;
    uint64_t          total_bytes;
    uint64_t          bytes_done;
    uint32_t          flags;
    uint64_t          waybill;           /* Ф26e: ferry correlation token (0 = sync/no-token) */
    uint32_t          submit_cookie;     /* the write submit's cloakroom token — echoed
                                          * into the plain (waybill==0) completion so the
                                          * caller's paired wait adopts only its own. */

    /* ---- in-flight chunk ---- */
    uint32_t          if_disk_block;
    uint32_t          if_off_in_blk;
    uint32_t          if_chunk;
    bool              if_partial_rmw;
    bool              if_alloc_fresh;        /* this chunk's block was just W_ALLOC'd */
    error_t           if_status;            /* IRQ writes here */

    /* ---- alloc tracking ---- */
    uint32_t          alloc_block;
    uint16_t          alloc_count;
    bool              alloc_pending_meta;

    /* ---- DMA staging ---- */
    void             *dma_phys;
    void             *dma_virt;

    /* ---- token handoff ---- */
    struct WriteJob  *next_pending;

    /* ---- result ---- */
    int               result_rc;

    /* ---- state machine ---- */
    _Atomic int       state;

    /* ---- never-drop completion node (Ф26 M4) ----
     * Embedded MPSC link. The AHCI completion IRQ and the token handoff
     * post this job to the drain core via BatonPass with zero
     * allocation, so a continuation can never be dropped. run = wjob_pump,
     * ctx = this job; set once in ObjWriteAsync. Distinct from next_pending
     * (the token-wait list). */
    Baton cq_node;

    /* Where each block's read or write waits when the port has no slot for
     * it, or its door is shut for a cache flush (core/boardroom/roadstead.h).
     * The port's from the submit until the answer, then the job's again. */
    RoadsteadBerth berth;

    /* ---- CrateStage handoff (async-safe Crate descriptor staging) ----
     * Dispatcher allocated the Crate[] kbuf via crate_stage_in and points
     * out_crate above into it. Ownership transfers to this WriteJob the
     * moment ObjWriteAsync returns ERR_WOULD_BLOCK with PROC_WAITING.
     * wjob_finalize writes out_crate->size = bytes_written, then calls
     * crate_stage_commit_and_release to flush the descriptor array back
     * to user memory and free the kbuf. */
    Crate            *crates_kbuf;
    uint64_t          crates_uaddr;
    uint16_t          crate_count;
} WriteJob;

int ObjWriteAsync(uint32_t           file_id,
                  uint64_t           offset,
                  uint32_t           flags,
                  const void        *src_kp,
                  uint32_t           size,
                  Crate             *out_crate,
                  const struct OpContext *ctx,
                  Crate             *crates_kbuf,   /* staged Crate[] ownership */
                  uint16_t           crate_count,
                  uint64_t           crates_uaddr,
                  uint64_t           waybill);       /* Ф26e: ferry token (0 = none) */

#endif /* WRITE_JOB_H */
