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
 * WriteJob — the one automaton every change of a file's extents a program asks
 * for goes through: a write (OBJ_WRITE), a span (OBJ_SPAN) and a defrag
 * (SYSTEM_OP_DEFRAG_FILE).
 *
 * A job holds its file's Helm for the whole of its work (tagfs/helm/helm.h),
 * and the volume's door from the moment it is built until it has answered, so
 * the volume it works on is never taken down beneath it and never mounted
 * again under it (W-DOOR). It asks for the Helm once, and nobody spins:
 *
 *   free, and the work synchronous — the job runs at once, in the caller's
 *   call, and the answer is the call's; a write longer than a window runs its
 *   first there, then parks its caller and runs each next window at a service
 *   point, giving the core back between two (RT-16);
 *
 *   held — the caller is parked on its chit and the job waits in the Helm's
 *   FIFO. The holder that lets go hands the Helm on (HelmHand): a synchronous
 *   job is listed for a service point (HelmReady) and run there, an
 *   asynchronous write goes on where its seat's answers continue, and the
 *   answer is pushed when the job ends — or, when the medium left meanwhile,
 *   the job is answered ERR_DEVICE_NOT_READY instead of being handed anything
 *   (RT-13);
 *
 *   an asynchronous write parks its caller whether or not the Helm is free:
 *   every block goes through the port — an AHCI port's slots or a USB disk's
 *   queue (BoardroomReadAsync, BoardroomWriteAsync) — the job parked on each
 *   answer and pumped where that seat's answers continue: on the drain core
 *   for an AHCI port, whose answer comes in its interrupt, and at a service
 *   point for a USB disk, whose answer comes on the drain core already
 *   (write_job.c, wjob_continue).
 *
 * The asynchronous write works on the file's Plot exactly as the synchronous
 * one does (HelmWriteWindow): the same growth rule, the same windows, each
 * reserved before a byte of it moves, the same read-modify-write rule, and one
 * inked record a window. Either way the answer is one contract (W-ERR): the
 * reason, the bytes the file's inked record holds of the write, the size.
 */

struct OpContext;

typedef enum {
    WRITE_JOB_WRITE = 0,    /* bytes at an offset, or at the file's size */
    WRITE_JOB_SPAN,         /* the file made exactly a size long */
    WRITE_JOB_DEFRAG,       /* the file gathered into one run */
} WriteJobKind;

/* What a caller asks of a job. */
typedef struct {
    WriteJobKind kind;
    uint32_t     file_id;
    PlotSource   source;        /* WRITE: the bytes, their pages brought in (PlotSourceBringIn) */
    uint64_t     offset;        /* WRITE: where the write begins ... */
    bool         append;        /* ... or at the file's size */
    uint64_t     waybill;       /* WRITE: a ferry's correlation token (0 = none) */
    uint64_t     size;          /* SPAN: the size the file is made */
    uint32_t     target_block;  /* DEFRAG: near where its one run is laid */
    Crate       *out_crate;     /* WRITE: [u64 inked][u64 size], every answer; DEFRAG: [u32 score]; or NULL */
#ifdef CONFIG_VOLUME_TESTS
    struct WriteJobStandAnswer *stand;   /* the stand's caller: its answer told here */
#endif
} WriteJobAsk;

typedef enum {
    W_INIT = 0,
    W_HELM_WAIT,       /* in the file's Helm FIFO */
    W_ALLOC,           /* the file's Plot loaded, and the window's ground reserved */
    W_LOCATE,
    W_READ_OLD,        /* async read of the block a partial write overlays */
    W_DMA_FILL,
    W_BLOCK_SUBMIT,    /* the block's write to the port */
    W_BLOCK_DONE,      /* ... and its answer */
    W_LOG_META,        /* what landed of the window made ready to be the file's */
    W_INK,             /* the window's record inked (TagFSInkPlot) */
    W_SPARE,           /* a Spare for the pool's tail stamped through the port */
    W_SPARE_DONE,
    W_DONE,
} WriteJobState;

typedef struct WriteJob {
    /* ---- identity / target ---- */
    WriteJobKind      kind;
    bool              async;             /* blocks through the port, parked on each answer */
    bool              resumes_at_service;/* ... its steps between two answers run at a service point */
    bool              deferred;          /* the caller is parked: the answer is pushed */
    process_t        *target;            /* pinned for the job's life; NULL for a kernel caller */
    const struct OpContext *ctx;         /* the caller's call, while the job runs inside it */
    uint32_t          file_id;
    TagFSFileHandle  *handle;
    Crate            *out_crate;

    /* ---- payload ---- */
    PlotSource        source;            /* the program's pages, pulled a block at a time */
    uint64_t          asked_offset;      /* where the caller asked the write to begin */
    bool              append;            /* ... or at the file's size */
    uint64_t          start_offset;      /* where it begins: set once the Plot is loaded */
    uint64_t          bytes_done;        /* of the source, landed */
    uint64_t          bytes_inked;       /* ... and named by the file's inked record */
    uint64_t          window_end;        /* where the window being written ends, in the source */
    uint64_t          span_size;
    uint64_t          span_was;          /* the size the span found */
    uint32_t          defrag_target;
    uint64_t          waybill;           /* Ф26e: ferry correlation token (0 = sync/no-token) */
    uint32_t          submit_cookie;     /* the submit's cloakroom token — echoed into the
                                          * plain (waybill==0) completion so the caller's
                                          * paired wait adopts only its own. */

    /* ---- the file ---- */
    HelmWaiter        helm;              /* its place in the file's Helm */
    bool              holds_helm;
    Plot              plot;              /* the async write's, loaded under the Helm (W_ALLOC) */
    bool              loaded;            /* PlotLoad answered OK */
    uint64_t          final_size;        /* the size the job leaves */
    error_t           stopped;           /* the refusal a write that stopped part way ends on */
    bool              medium_left;       /* never handed the Helm: the medium left (RT-13) */
    uint64_t          lost;              /* ... and the book given up took this many changes */
    uint32_t          spare_block;       /* the Spare W_SPARE stamps, until the pool has it */
    MetaPoolBlock    *spare_image;

    /* ---- in-flight chunk ---- */
    uint32_t          if_disk_block;
    uint32_t          if_left;           /* blocks of its run after if_disk_block */
    uint32_t          if_off_in_blk;
    uint32_t          if_chunk;
    bool              if_reading;        /* the command out is the overlaid block's read */
    error_t           if_status;         /* the command's answer writes here */
    uint64_t          if_submit_tsc;     /* when the write went out: its time on the medium */

    /* ---- DMA staging ---- */
    void             *dma_phys;
    void             *dma_virt;

    /* ---- state machine ---- */
    _Atomic int       state;

    /* ---- never-drop completion node (Ф26 M4) ----
     * Embedded MPSC link. The AHCI completion IRQ and the Helm's hand post
     * this job to the drain core via BatonPass with zero allocation, so a
     * continuation can never be dropped. run = wjob_pump, ctx = this job. A
     * job whose steps resume at a service point is listed through its Helm
     * waiter instead (HelmReady), which is just as allocation-free. */
    Baton cq_node;

    /* Where each block's read or write waits when the port has no slot for
     * it, or its door is shut for a cache flush (core/boardroom/roadstead.h).
     * The port's from the submit until the answer, then the job's again. */
    RoadsteadBerth berth;

    /* ---- CrateStage handoff (async-safe Crate descriptor staging) ----
     * The dispatcher staged the Crate[] kbuf and out_crate points into it.
     * Ownership transfers to the job the moment it parks its caller
     * (ChitGive); the job's end writes the out crate, then calls
     * crate_stage_commit_and_release to flush the descriptor array back to
     * user memory and free the kbuf. */
    Crate            *crates_kbuf;
    uint64_t          crates_uaddr;
    uint16_t          crate_count;

#ifdef CONFIG_VOLUME_TESTS
    struct WriteJobStandAnswer *stand;
#endif
} WriteJob;

/* The job for `ask`, started: the handler's answer — the work's own when it
 * ran at once, or ERR_WOULD_BLOCK once the job owns the answer and the staged
 * crates. A kernel caller (no process to park) whose file's Helm is held is
 * refused ERR_BUSY — unless, on the stand, it brought the place its answer is
 * told to (WriteJobStandAnswer). */
int WriteJobSubmit(const WriteJobAsk *ask, Crate *crates, const struct OpContext *ctx);

/* Whether a program's write on `seat` goes through the port, parked on each
 * answer: the seat takes reads and writes nobody stands over, and a K-Core
 * pumps what they continue into — on a single core the one core runs the
 * program and never pumps, so a job would strand there. */
bool WriteJobSeatRunsAsync(uint8_t seat);

#ifdef CONFIG_VOLUME_TESTS
/*
 * The stand's caller of a write (tagfs/tests): a caller in the kernel has no
 * program to park, so a test that wants the job to go through the port as a
 * program's would brings the place its answer is told to instead, and turns
 * the machine itself while it waits (the drain core's batons, the service
 * point). The job's caller never stands in its call: WriteJobSubmit answers
 * ERR_WOULD_BLOCK or the job's own end, and the answer is here either way —
 * the one contract every write answers (W-ERR), and whether the job's blocks
 * went through the port to its end.
 */
typedef struct WriteJobStandAnswer {
    volatile uint32_t answered;      /* 1 once the job ended; read the rest after it */
    error_t           rc;
    uint64_t          written;       /* the bytes of the write the file's inked record holds */
    uint64_t          size;          /* the size the job left the file */
    bool              through_port;  /* the job went through the port to its end */
} WriteJobStandAnswer;
#endif

/* A storage op that ran synchronously answered on the ferry channel its
 * submitter listens on (storage_ops.c). */
int storage_sync_ferry_finalize(const struct OpContext *ctx, Crate *crates, error_t err,
                                uint64_t bytes, uint64_t waybill);

#endif /* WRITE_JOB_H */
