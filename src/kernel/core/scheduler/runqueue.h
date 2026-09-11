#ifndef RUNQUEUE_H
#define RUNQUEUE_H

#include "ktypes.h"
#include "klib.h"
#include "kernel_config.h"

#define SCHED_PRIO_IDLE    0
#define SCHED_PRIO_NORMAL  1
#define SCHED_PRIO_STARVED 2
#define SCHED_PRIO_CONTEXT 3
#define SCHED_PRIO_LEVELS  4

#define RUNQUEUE_INITIAL_CAP  CONFIG_RUNQUEUE_INITIAL_CAP
#define RUNQUEUE_MAX_CAP      CONFIG_RUNQUEUE_MAX_CAP

struct process_t;

typedef struct {
    struct process_t **procs;
    uint32_t          capacity;
    uint32_t          head;
    uint32_t          tail;
    uint32_t          count;
} SchedQueue;

typedef struct {
    SchedQueue       queues[SCHED_PRIO_LEVELS];
    uint32_t         active_bitmap;
    spinlock_t       lock;
    _Atomic uint32_t total;
} RunQueue;

void runqueue_init(RunQueue *rq);
void runqueue_shutdown(RunQueue *rq);
bool runqueue_enqueue(RunQueue *rq, struct process_t *proc, int prio);
struct process_t *runqueue_dequeue_best(RunQueue *rq);
void runqueue_remove(RunQueue *rq, struct process_t *proc);
bool runqueue_contains(RunQueue *rq, struct process_t *proc);
uint32_t RunqueueAtomicTotal(const RunQueue *rq);

#endif