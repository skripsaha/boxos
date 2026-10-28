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
#include "settle.h"     /* the job's wait for a cycle's end, when room is on its way */
#include "helm.h"       /* the defrag's state between its windows */

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
 *   call, and the answer is the call's; work longer than a window — a write, a
 *   span's growth, a defrag's copy — runs its first there, then parks its
 *   caller and runs each next window at a service point, giving the core back
 *   between two (RT-16, R24);
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
 *   every command goes through the port — an AHCI port's slots or a USB
 *   disk's queue (BoardroomReadAsync, BoardroomWriteAsync), a stretch of the
 *   window a command — the job parked on each answer and pumped where that
 *   seat's answers continue: on the drain core for an AHCI port, whose answer
 *   comes in its interrupt, and at a service point for a USB disk, whose
 *   answer comes on the drain core already (write_job_port.c, wjob_continue).
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
    WRITE_JOB_SPAN,         /* the file made exactly a size long, or grown to it (grow_only) */
    WRITE_JOB_DEFRAG,       /* the file gathered into one run */
} WriteJobKind;

/* What a caller asks of a job. */
typedef struct {
    WriteJobKind kind;
    uint64_t     file_id;
    PlotSource   source;        /* WRITE: the bytes, their pages brought in (PlotSourceBringIn) */
    uint64_t     offset;        /* WRITE: where the write begins ... */
    bool         append;        /* ... or at the file's size */
    uint64_t     waybill;       /* WRITE: a ferry's correlation token (0 = none) */
    uint64_t     size;          /* SPAN: the size the file is made ... */
    bool         grow_only;     /* ... only if it is shorter (STORAGE_SPAN_GROW) */
    InkWarrant   warrant;       /* the caller's (StorageCallWarrant), carried in the job's handle */
    Crate       *out_crate;     /* WRITE: [u64 inked][u64 size], every answer; DEFRAG: [u32 score]; or NULL */
#ifdef CONFIG_VOLUME_TESTS
    struct WriteJobStandAnswer *stand;   /* the stand's caller: its answer told here */
    /* DEFRAG, a program's, on the stand (WriteJobAskStandHold): once its
     * first window took the run it gathers into it goes no further until the
     * file is given up (wjob_stand_holds), and says it holds on this Touch
     * tag — TOUCH_TAG_INVALID says nothing. */
    bool         hold;
    uint32_t     hold_says;
#endif
} WriteJobAsk;

/* SYSTEM_OP_DEFRAG_FILE's params past the file id: on the stand
 * (VOLUME_TESTS=on), a [u32 touch tag] there asks the stand's hold, said on
 * that tag; a kernel built without it reads nothing there. */
static inline void WriteJobAskStandHold(WriteJobAsk *ask, const uint8_t *params, uint32_t size)
{
#ifdef CONFIG_VOLUME_TESTS
    if (size >= sizeof(uint64_t) + sizeof(uint32_t)) {
        ask->hold = true;
        memcpy(&ask->hold_says, params + sizeof(uint64_t), sizeof(ask->hold_says));
    }
#else
    (void)ask, (void)params, (void)size;
#endif
}

typedef enum {
    W_INIT = 0,
    W_HELM_WAIT,       /* in the file's Helm FIFO */
    W_ALLOC,           /* the file's Plot loaded, and the window's ground reserved */
    W_LOCATE,          /* the stretch the cursor is in (PlotStretchAt) */
    W_READ_OLD,        /* async read of its first or last block, which the write overlays */
    W_READ_DONE,       /* ... and its answer */
    W_DMA_FILL,        /* the write's bytes staged in the stretch's DMA pages */
    W_BLOCK_SUBMIT,    /* the stretch's write to the port, one command */
    W_BLOCK_DONE,      /* ... and its answer */
    W_LOG_META,        /* what landed of the window made ready to be the file's */
    W_INK,             /* the window's record inked (TagFSInkPlot) */
    W_SPARE,           /* a Spare for the pool's tail stamped through the port */
    W_SPARE_DONE,
    W_DONE,
} WriteJobState;

/* The reads a stretch's first and last block still owe (WriteJob.st_reads). */
#define WJOB_READ_FIRST 1u
#define WJOB_READ_LAST  2u

typedef struct WriteJob {
    /* ---- identity / target ---- */
    WriteJobKind      kind;
    bool              async;             /* blocks through the port, parked on each answer */
    bool              resumes_at_service;/* ... its steps between two answers run at a service point */
    bool              deferred;          /* the caller is parked: the answer is pushed */
    process_t        *target;            /* pinned for the job's life; NULL for a kernel caller */
    const struct OpContext *ctx;         /* the caller's call, while the job runs inside it */
    uint64_t          file_id;
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
    bool              span_grow;         /* grown to span_size only if shorter */
    uint64_t          span_was;          /* the size its first window found; UINT64_MAX before */
    bool              ended;             /* SPAN, DEFRAG: no window is left */
    HelmDefragging    defrag;            /* DEFRAG: its Plot and copy between windows */
    uint64_t          waybill;           /* Ф26e: ferry correlation token (0 = sync/no-token) */
    uint32_t          submit_cookie;     /* the submit's cloakroom token — echoed into the
                                          * plain (waybill==0) completion so the caller's
                                          * paired wait adopts only its own. */

    /* ---- the file ---- */
    HelmWaiter        helm;              /* its place in the file's Helm */
    bool              holds_helm;
    SettleWaiter      room;              /* its wait for a cycle's end, the volume short of
                                          * ground a Settle cycle gives back */
    error_t           room_came;         /* ... and what that cycle's end answered */
    Plot              plot;              /* the async write's, loaded under the Helm (W_ALLOC) */
    bool              loaded;            /* PlotLoad answered OK */
    uint64_t          final_size;        /* the size the job leaves */
    error_t           stopped;           /* the refusal a write that stopped part way ends on */
    bool              medium_left;       /* never handed the Helm: the medium left (RT-13) */
    uint64_t          lost;              /* ... and the book given up took this many changes */
    uint32_t          spare_block;       /* the Spare W_SPARE stamps, until the pool has it */
    MetaPoolBlock    *spare_image;

    /* ---- the stretch in flight (PlotStretchAt) ---- */
    PlotStretch       st;
    uint8_t           st_reads;          /* WJOB_READ_FIRST / _LAST still to be read */
    uint32_t          st_reading;        /* the block of it the read out is for */
    uint32_t          st_staged;         /* its blocks the write carries, from its first */
    error_t           st_stop;           /* why staging stopped at st_staged; OK when it did not */
    error_t           if_status;         /* the command's answer writes here */
    uint64_t          if_submit_tsc;     /* when the write went out: its time on the medium */

    /* ---- DMA staging: the run the seat takes in one command, in one piece ---- */
    void             *dma_phys;
    void             *dma_virt;
    uint32_t          dma_blocks;

    /* ---- state machine ---- */
    _Atomic int       state;

    /* ---- never-drop completion node (Ф26 M4) ----
     * Embedded MPSC link. The AHCI completion IRQ and the Helm's hand post
     * this job to the drain core via BatonPass with zero allocation, so a
     * continuation can never be dropped. run = wjob_pump, ctx = this job. A
     * job whose steps resume at a service point is listed through its Helm
     * waiter instead (HelmReady), which is just as allocation-free. */
    Baton cq_node;

    /* Where each command waits when the port has no slot for it, or its door
     * is shut for a cache flush (core/boardroom/roadstead.h). The port's from
     * the submit until the answer, then the job's again — with what a refused
     * write kept (berth.landed). */
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
    bool              hold;              /* WriteJobAsk.hold, until the job is held once */
    bool              held;              /* the stand held it: its answer is said */
    uint32_t          hold_says;
    HelmStandHold     stand_hold;        /* where helm.c keeps it while it is held */
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

/* The port's own write door knocked at by the stand, with no job behind it: a
 * volume under glass answers it before the job is looked at (write_job_port.c,
 * the door every write of a job's blocks goes through). */
error_t WriteJobStandWriteDoor(uint64_t block);
#endif

/* A storage op that ran synchronously answered on the ferry channel its
 * submitter listens on (storage_ops.c). */
int storage_sync_ferry_finalize(const struct OpContext *ctx, Crate *crates, error_t err,
                                uint64_t bytes, uint64_t waybill);

/* The warrant a program's call carries to the Ink guard (InkWarrant): its
 * cabin holds system or god. Read once, where a deck takes the call; a caller
 * in the kernel has no cabin and carries none (storage_ops.c). */
InkWarrant StorageCallWarrant(const struct OpContext *ctx);

#endif /* WRITE_JOB_H */
