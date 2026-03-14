#include "runqueue.h"
#include "process.h"

void runqueue_init(RunQueue* rq) {
    for (int i = 0; i < SCHED_PRIO_LEVELS; i++) {
        rq->queues[i].head  = 0;
        rq->queues[i].tail  = 0;
        rq->queues[i].count = 0;
        for (int j = 0; j < RUNQUEUE_CAPACITY; j++) {
            rq->queues[i].procs[j] = NULL;
        }
    }
    rq->active_bitmap = 0;
    spinlock_init(&rq->lock);
}

bool runqueue_enqueue(RunQueue* rq, struct process_t* proc, int prio) {
    if (!proc || prio < 0 || prio >= SCHED_PRIO_LEVELS)
        return false;

    SchedQueue* q = &rq->queues[prio];
    if (q->count >= RUNQUEUE_CAPACITY)
        return false;

    q->procs[q->tail] = proc;
    q->tail = (q->tail + 1) % RUNQUEUE_CAPACITY;
    q->count++;

    rq->active_bitmap |= (1u << prio);
    return true;
}

struct process_t* runqueue_dequeue_best(RunQueue* rq) {
    if (rq->active_bitmap == 0)
        return NULL;

    int prio = 31 - __builtin_clz(rq->active_bitmap);

    SchedQueue* q = &rq->queues[prio];
    struct process_t* proc = q->procs[q->head];
    q->procs[q->head] = NULL;
    q->head = (q->head + 1) % RUNQUEUE_CAPACITY;
    q->count--;

    if (q->count == 0)
        rq->active_bitmap &= ~(1u << prio);

    return proc;
}

void runqueue_remove(RunQueue* rq, struct process_t* proc) {
    if (!proc) return;

    for (int prio = 0; prio < SCHED_PRIO_LEVELS; prio++) {
        SchedQueue* q = &rq->queues[prio];
        if (q->count == 0) continue;

        uint32_t idx = q->head;
        for (uint32_t n = 0; n < q->count; n++) {
            if (q->procs[idx] == proc) {
                // Shift remaining entries left to fill the gap.
                uint32_t cur = idx;
                for (uint32_t m = 0; m < q->count - n - 1; m++) {
                    uint32_t next = (cur + 1) % RUNQUEUE_CAPACITY;
                    q->procs[cur] = q->procs[next];
                    cur = next;
                }
                q->procs[cur] = NULL;
                q->tail = (q->tail == 0 ? RUNQUEUE_CAPACITY : q->tail) - 1;
                q->count--;

                if (q->count == 0)
                    rq->active_bitmap &= ~(1u << prio);
                return;
            }
            idx = (idx + 1) % RUNQUEUE_CAPACITY;
        }
    }
}

bool runqueue_contains(RunQueue* rq, struct process_t* proc) {
    if (!proc) return false;

    for (int prio = 0; prio < SCHED_PRIO_LEVELS; prio++) {
        SchedQueue* q = &rq->queues[prio];
        uint32_t idx = q->head;
        for (uint32_t n = 0; n < q->count; n++) {
            if (q->procs[idx] == proc)
                return true;
            idx = (idx + 1) % RUNQUEUE_CAPACITY;
        }
    }
    return false;
}

uint32_t runqueue_total_count(RunQueue* rq) {
    uint32_t total = 0;
    for (int prio = 0; prio < SCHED_PRIO_LEVELS; prio++)
        total += rq->queues[prio].count;
    return total;
}
