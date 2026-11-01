#ifndef RUNQUEUE_H
#define RUNQUEUE_H

#include "ktypes.h"
#include "klib.h"
#include "kernel_config.h"

#define RUNQUEUE_INITIAL_CAP  CONFIG_RUNQUEUE_INITIAL_CAP
#define RUNQUEUE_MAX_CAP      CONFIG_RUNQUEUE_MAX_CAP

struct process_t;

/*
 * One core's runnable strands, the one that has paid the least Fare first: a
 * binary min-heap on process_t.fare in a growing array. Every strand in it
 * knows its slot (process_t.rq_index, -1 in none), kept true across every
 * move, so it is taken out from anywhere by its slot. A resize copies the
 * slots where they stand.
 */
typedef struct {
    struct process_t **procs;
    uint32_t           capacity;
    uint32_t           count;
    spinlock_t         lock;
    _Atomic uint32_t   total;  // count, for readers without the lock
} RunQueue;

void runqueue_init(RunQueue *rq);
void runqueue_shutdown(RunQueue *rq);
bool runqueue_enqueue(RunQueue *rq, struct process_t *proc);
struct process_t *runqueue_dequeue_best(RunQueue *rq);
void runqueue_remove(RunQueue *rq, struct process_t *proc);
/* A queued strand's Fare changed where it stands: it settles to its slot. */
void runqueue_settle(RunQueue *rq, struct process_t *proc);
bool runqueue_contains(RunQueue *rq, struct process_t *proc);
/* The strand in slot `index`, NULL past the end — for a look along the heap
 * under its lock. */
struct process_t *runqueue_at(RunQueue *rq, uint32_t index);
uint32_t RunqueueAtomicTotal(const RunQueue *rq);

#endif // RUNQUEUE_H
