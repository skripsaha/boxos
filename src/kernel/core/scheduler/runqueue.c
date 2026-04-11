#include "runqueue.h"
#include "process.h"

static bool schedqueue_resize(SchedQueue *sq, uint32_t new_cap)
{
    if (new_cap < sq->count || new_cap > RUNQUEUE_MAX_CAP)
        return false;

    struct process_t **new_procs = kmalloc(sizeof(struct process_t *) * new_cap);
    if (!new_procs)
        return false;  // Keep old buffer intact on allocation failure

    // Copy existing entries in order
    uint32_t idx = sq->head;
    for (uint32_t i = 0; i < sq->count; i++) {
        new_procs[i] = sq->procs[idx];
        idx = (idx + 1) % sq->capacity;
    }

    // Only free old buffer AFTER successful copy
    if (sq->procs)
        kfree(sq->procs);

    sq->procs = new_procs;
    sq->capacity = new_cap;
    sq->head = 0;
    sq->tail = sq->count;
    return true;
}

void runqueue_init(RunQueue *rq)
{
    if (!rq) return;

    for (int i = 0; i < SCHED_PRIO_LEVELS; i++)
    {
        rq->queues[i].capacity = RUNQUEUE_INITIAL_CAP;
        rq->queues[i].head = 0;
        rq->queues[i].tail = 0;
        rq->queues[i].count = 0;
        rq->queues[i].procs = kmalloc(sizeof(struct process_t *) * RUNQUEUE_INITIAL_CAP);
        if (rq->queues[i].procs) {
            for (uint32_t j = 0; j < RUNQUEUE_INITIAL_CAP; j++)
                rq->queues[i].procs[j] = NULL;
        } else {
            rq->queues[i].capacity = 0;
        }
    }
    rq->active_bitmap = 0;
    spinlock_init(&rq->lock);
}

void runqueue_shutdown(RunQueue *rq)
{
    if (!rq) return;

    for (int i = 0; i < SCHED_PRIO_LEVELS; i++) {
        if (rq->queues[i].procs) {
            kfree(rq->queues[i].procs);
            rq->queues[i].procs = NULL;
        }
        rq->queues[i].capacity = 0;
    }
}

bool runqueue_enqueue(RunQueue *rq, struct process_t *proc, int prio)
{
    if (!rq || !proc || prio < 0 || prio >= SCHED_PRIO_LEVELS)
        return false;

    SchedQueue *q = &rq->queues[prio];
    if (!q->procs)
        return false;

    // Auto-resize if near capacity
    if (q->count >= q->capacity) {
        uint32_t new_cap = q->capacity * 2;
        if (new_cap > RUNQUEUE_MAX_CAP)
            new_cap = RUNQUEUE_MAX_CAP;
        if (new_cap <= q->count)
            return false;  // Truly full
        if (!schedqueue_resize(q, new_cap))
            return false;
    }

    q->procs[q->tail] = proc;
    proc->rq_prio = (int8_t)prio;
    proc->rq_index = (int16_t)q->tail;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    rq->active_bitmap |= (1u << prio);
    return true;
}

struct process_t *runqueue_dequeue_best(RunQueue *rq)
{
    if (!rq || rq->active_bitmap == 0)
        return NULL;

    int prio = 31 - __builtin_clz(rq->active_bitmap);

    SchedQueue *q = &rq->queues[prio];
    if (!q->procs || q->count == 0)
        return NULL;

    struct process_t *proc = q->procs[q->head];
    q->procs[q->head] = NULL;
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    if (q->count == 0)
        rq->active_bitmap &= ~(1u << prio);

    // Update removed process tracking
    proc->rq_prio = -1;
    proc->rq_index = -1;

    // Shrink if significantly underutilized (save memory)
    if (q->count < q->capacity / 4 && q->capacity > RUNQUEUE_INITIAL_CAP) {
        uint32_t new_cap = q->capacity / 2;
        if (new_cap < RUNQUEUE_INITIAL_CAP)
            new_cap = RUNQUEUE_INITIAL_CAP;
        schedqueue_resize(q, new_cap);
    }

    return proc;
}

void runqueue_remove(RunQueue *rq, struct process_t *proc)
{
    if (!rq || !proc)
        return;

    // O(1) removal using stored index
    int prio = proc->rq_prio;
    int idx = proc->rq_index;
    if (prio < 0 || idx < 0)
        return;  // Not enqueued

    SchedQueue *q = &rq->queues[prio];
    if ((uint32_t)idx >= q->capacity || q->procs[idx] != proc)
        return;  // Sanity check

    q->count--;

    if (q->count == 0) {
        // Queue is now empty
        q->procs[idx] = NULL;
        rq->active_bitmap &= ~(1u << prio);
        q->head = 0;
        q->tail = 0;
    } else if ((uint32_t)idx == q->head) {
        // Removing head — advance
        q->procs[idx] = NULL;
        q->head = (q->head + 1) % q->capacity;
    } else if ((uint32_t)idx == (q->tail - 1 + q->capacity) % q->capacity) {
        // Removing tail — just decrement
        q->procs[idx] = NULL;
        q->tail = (q->tail - 1 + q->capacity) % q->capacity;
    } else {
        // Removing middle — swap with tail for O(1)
        uint32_t tail_idx = (q->tail - 1 + q->capacity) % q->capacity;
        q->procs[idx] = q->procs[tail_idx];
        q->procs[idx]->rq_index = (int16_t)idx;
        q->procs[tail_idx] = NULL;
        q->tail = tail_idx;
    }

    proc->rq_prio = -1;
    proc->rq_index = -1;
}

bool runqueue_contains(RunQueue *rq, struct process_t *proc)
{
    if (!rq || !proc)
        return false;

    for (int prio = 0; prio < SCHED_PRIO_LEVELS; prio++)
    {
        SchedQueue *q = &rq->queues[prio];
        if (!q->procs || q->count == 0)
            continue;

        uint32_t idx = q->head;
        for (uint32_t n = 0; n < q->count; n++)
        {
            if (q->procs[idx] == proc)
                return true;
            idx = (idx + 1) % q->capacity;
        }
    }
    return false;
}

uint32_t runqueue_total_count(RunQueue *rq)
{
    if (!rq) return 0;

    uint32_t total = 0;
    for (int prio = 0; prio < SCHED_PRIO_LEVELS; prio++)
        total += rq->queues[prio].count;
    return total;
}
