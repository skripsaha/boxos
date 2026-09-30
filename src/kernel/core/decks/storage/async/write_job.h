#ifndef WRITE_JOB_H
#define WRITE_JOB_H

#include "ktypes.h"
#include "error.h"
#include "tagfs.h"
#include "plot.h"       /* the file's Plot, and where the job's bytes come from */
#include "boxos_crate.h"
#include "process.h"
#include "baton.h"      /* embedded never-drop MPSC node (cq_node) */
#include "roadstead.h"  /* the job's own berth at the port */

/*
 * WriteJob — async ObjWrite state machine.
 *
 * The caller parks on PROC_WAITING, each AHCI completion posts the job to the
 * drain core's pump, and KResultPush wakes the caller when the job is done.
 * The job works on the file's Plot exactly as a synchronous write does: the
 * same growth rule, the same reservation before any byte moves, the same
 * read-modify-write rule, and one inked record.
 */

struct OpContext;
struct ManifestOp;

typedef enum {
    W_INIT = 0,
    W_TOKEN_WAIT,
    W_ALLOC,           /* the file's Plot loaded and the write's ground reserved */
    W_LOCATE,
    W_READ_OLD,        /* async read of the block a partial write overlays */
    W_DMA_FILL,
    W_AHCI_SUBMIT,
    W_AHCI_DONE,
    W_LOG_META,        /* what landed made ready to be the file's */
    W_INK,             /* the record inked (TagFSInkPlot) */
    W_SPARE,           /* a Spare for the pool's tail stamped through the port */
    W_SPARE_DONE,
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
    PlotSource        source;            /* the program's pages, pulled a block at a time */
    uint64_t          asked_offset;      /* where the caller asked the write to begin */
    bool              append;            /* ... or at the file's size */
    uint64_t          start_offset;      /* where it begins: set once the Plot is loaded */
    uint64_t          bytes_done;
    uint64_t          waybill;           /* Ф26e: ferry correlation token (0 = sync/no-token) */
    uint32_t          submit_cookie;     /* the write submit's cloakroom token — echoed
                                          * into the plain (waybill==0) completion so the
                                          * caller's paired wait adopts only its own. */

    /* ---- the file ---- */
    Plot              plot;              /* loaded under the token (W_ALLOC) */
    bool              loaded;            /* PlotLoad answered OK */
    bool              committed;         /* an inked record naming bytes_done is the file's */
    error_t           stopped;           /* the refusal a write that stopped part way ends on */
    uint32_t          spare_block;       /* the Spare W_SPARE stamps, until the pool has it */
    MetaPoolBlock    *spare_image;

    /* ---- in-flight chunk ---- */
    uint32_t          if_disk_block;
    uint32_t          if_left;           /* blocks of its run after if_disk_block */
    uint32_t          if_off_in_blk;
    uint32_t          if_chunk;
    bool              if_reading;        /* the command out is the overlaid block's read */
    error_t           if_status;         /* IRQ writes here */

    /* ---- DMA staging ---- */
    void             *dma_phys;
    void             *dma_virt;

    /* ---- token handoff ---- */
    struct WriteJob  *next_pending;

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

/* The source's bytes written into the file at `offset` — at its size when
 * `append` — by a job the caller parks on: ERR_WOULD_BLOCK once the job owns
 * the write and the staged crates, anything else a refusal before it did, the
 * source untouched and the caller free to write it synchronously. */
int ObjWriteAsync(uint32_t                file_id,
                  uint64_t                offset,
                  bool                    append,
                  const PlotSource       *source,
                  Crate                  *out_crate,
                  const struct OpContext *ctx,
                  Crate                  *crates_kbuf,   /* staged Crate[] ownership */
                  uint16_t                crate_count,
                  uint64_t                crates_uaddr,
                  uint64_t                waybill);      /* Ф26e: ferry token (0 = none) */

#endif /* WRITE_JOB_H */
