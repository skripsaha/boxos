#ifndef WRITE_JOB_INTERNAL_H
#define WRITE_JOB_INTERNAL_H

#include "write_job.h"
#include "atomics.h"

/*
 * The WriteJob's own parts, shared by write_job.c (the job, its Helm, its end
 * and its entry) and write_job_port.c (the asynchronous write through the
 * port). Nothing outside the two calls these.
 */

static inline void wjob_to(WriteJob *j, WriteJobState s)
{
    atomic_store_u32((volatile uint32_t *)&j->state, (uint32_t)s);
}

/* The asynchronous job's next step, posted where its seat's answers continue
 * — never run here: the drain core's baton for an AHCI port, never dropped; a
 * service point for a USB disk, where the step may stand over the medium
 * (write_job.c says why). */
void wjob_continue(WriteJob *j);

/* The asynchronous write's pump: every step it can take from the state its
 * last answer set, up to the next answer it parks for, or its end. */
void wjob_pump(void *job_);

/* The job's end, however it ran: what it took given back, the listeners told,
 * the Helm handed on, the answer given. The handler's return when the job ran
 * inside its caller's call. */
int  wjob_finalize(WriteJob *j, error_t rc);

#endif /* WRITE_JOB_INTERNAL_H */
