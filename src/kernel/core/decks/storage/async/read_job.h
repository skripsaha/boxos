#ifndef READ_JOB_H
#define READ_JOB_H

#include "ktypes.h"
#include "error.h"
#include "tagfs.h"
#include "read.h"       /* the sink the job's bytes land in */
#include "boxos_crate.h"

struct OpContext;

/*
 * ReadJob — a program's read that does not stand in its call (OBJ_READ on a
 * seat that takes reads nobody stands over, with a K-Core to pump them).
 *
 * Its blocks go through the port (BoardroomReadAsync) a run a command — as
 * many of the file's next blocks as lie in one run on the medium and the seat
 * takes at once — into DMA pages of its own, and between two answers the job
 * is pumped where its seat's answers
 * continue — the drain core's baton for an AHCI port, whose answer comes in
 * its interrupt; a service point for a USB disk, whose answer comes in a
 * drain-core baton a caller standing over the same disk may be pumping, and
 * beneath which nothing may stand over the medium (write_job.c says why). Each
 * block is checked against the digest recorded when it was written before a
 * byte of it lands (N8), and lands in the program's pages from its DMA page
 * straight — never through a bounce of the whole read in the heap (S3c).
 *
 * ‼ ONE ANSWER, WITH ITS REAL REASON AND WHAT LANDED. The job's answer is the
 * seat's refusal as the seat gave it (ERR_BUSY, ERR_DEVICE_NOT_READY, ...,
 * never folded into ERR_IO — S3b), ERR_CHECKSUM for a block that does not
 * match its digest, ERR_METADATA_CORRUPTED for a part of the file inside its
 * size that no extent covers (S2, as the read that stands answers it), or
 * what the program's pages refused — with the bytes that landed before it in
 * the out crate's size and the Result's length.
 *
 * The job holds the volume's door from its making to its answer, so the
 * volume its handle's blocks belong to is the one mounted for as long as it
 * reads (W-DOOR, as a WriteJob does).
 */

typedef struct ReadJobAsk {
    uint64_t  file_id;
    ReadSink  sink;         /* where the bytes land, and how many at most */
    Crate    *out_crate;    /* its size <- the bytes that landed; or NULL */
    uint64_t  waybill;      /* box::ferry correlation token, 0 for none */
#ifdef CONFIG_VOLUME_TESTS
    struct ReadJobStandAnswer *stand;   /* the stand's caller: its answer told here */
#endif
} ReadJobAsk;

/* Whether a read on `seat` goes through the port: the seat takes reads nobody
 * stands over, and a K-Core pumps what they continue into. */
bool ReadJobSeatRunsAsync(uint8_t seat);

/* The job for `ask`, reading the open `handle` from its offset — at least one
 * byte of it inside the file's size. ERR_WOULD_BLOCK: the job owns the
 * handle, the answer and the staged crates, and its caller is parked (or, on
 * the stand, told through its record). Anything else: no job was made — no
 * memory for it, no DMA pages, the volume's door shut — and the handle is
 * still the caller's, to read standing in its call. */
int ReadJobSubmit(TagFSFileHandle *handle, const ReadJobAsk *ask, Crate *crates,
                  const struct OpContext *ctx);

#ifdef CONFIG_VOLUME_TESTS
/* The stand's caller of a read (tagfs/tests): a caller in the kernel has no
 * program to park, so a test that wants its read to go through the port brings
 * the place its answer is told to, and turns the machine itself while it
 * waits. */
typedef struct ReadJobStandAnswer {
    volatile uint32_t answered;   /* 1 once the job answered; read the rest after it */
    error_t           rc;
    uint64_t          landed;     /* the bytes that reached the sink */
} ReadJobStandAnswer;
#endif

#endif /* READ_JOB_H */
