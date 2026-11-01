#include "runqueue.h"
#include "process.h"

/* The slots are copied where they stand, so every strand's rq_index stays
 * true across a resize. */
static bool runqueue_resize(RunQueue *rq, uint32_t new_cap)
{
    if (new_cap < rq->count || new_cap > RUNQUEUE_MAX_CAP)
        return false;

    struct process_t **procs = kmalloc(sizeof(struct process_t *) * new_cap);
    if (!procs)
        return false;  // Keep old buffer intact on allocation failure

    if (rq->count)
        memcpy(procs, rq->procs, sizeof(struct process_t *) * rq->count);
    if (rq->procs)
        kfree(rq->procs);

    rq->procs = procs;
    rq->capacity = new_cap;
    return true;
}

static inline void runqueue_put(RunQueue *rq, uint32_t index, struct process_t *proc)
{
    rq->procs[index] = proc;
    proc->rq_index = (int32_t)index;
}

static void runqueue_sift_up(RunQueue *rq, uint32_t index)
{
    struct process_t *proc = rq->procs[index];
    while (index > 0)
    {
        uint32_t parent = (index - 1) / 2;
        if (rq->procs[parent]->fare <= proc->fare)
            break;
        runqueue_put(rq, index, rq->procs[parent]);
        index = parent;
    }
    runqueue_put(rq, index, proc);
}

static void runqueue_sift_down(RunQueue *rq, uint32_t index)
{
    struct process_t *proc = rq->procs[index];
    for (;;)
    {
        uint32_t child = 2 * index + 1;
        if (child >= rq->count)
            break;
        if (child + 1 < rq->count && rq->procs[child + 1]->fare < rq->procs[child]->fare)
            child++;
        if (proc->fare <= rq->procs[child]->fare)
            break;
        runqueue_put(rq, index, rq->procs[child]);
        index = child;
    }
    runqueue_put(rq, index, proc);
}

/* The strand in slot `index` leaves; the last one takes the slot and settles
 * up or down from it. */
static struct process_t *runqueue_take(RunQueue *rq, uint32_t index)
{
    struct process_t *proc = rq->procs[index];
    uint32_t last = rq->count - 1;

    rq->count = last;
    if (index != last)
    {
        runqueue_put(rq, index, rq->procs[last]);
        if (index > 0 && rq->procs[index]->fare < rq->procs[(index - 1) / 2]->fare)
            runqueue_sift_up(rq, index);
        else
            runqueue_sift_down(rq, index);
    }
    rq->procs[last] = NULL;
    proc->rq_index = -1;
    __atomic_store_n(&rq->total, rq->count, __ATOMIC_RELEASE);

    // Shrink if significantly underutilized (save memory)
    if (rq->count < rq->capacity / 4 && rq->capacity > RUNQUEUE_INITIAL_CAP)
    {
        uint32_t new_cap = rq->capacity / 2;
        if (new_cap < RUNQUEUE_INITIAL_CAP)
            new_cap = RUNQUEUE_INITIAL_CAP;
        runqueue_resize(rq, new_cap);
    }
    return proc;
}

void runqueue_init(RunQueue *rq)
{
    if (!rq) return;

    rq->procs = kmalloc(sizeof(struct process_t *) * RUNQUEUE_INITIAL_CAP);
    rq->capacity = rq->procs ? RUNQUEUE_INITIAL_CAP : 0;
    rq->count = 0;
    __atomic_store_n(&rq->total, 0, __ATOMIC_RELAXED);
    spinlock_init(&rq->lock);
}

void runqueue_shutdown(RunQueue *rq)
{
    if (!rq) return;

    if (rq->procs) {
        kfree(rq->procs);
        rq->procs = NULL;
    }
    rq->capacity = 0;
    rq->count = 0;
}

bool runqueue_enqueue(RunQueue *rq, struct process_t *proc)
{
    if (!rq || !proc)
        return false;

    if (rq->count >= rq->capacity)
    {
        uint32_t new_cap = rq->capacity ? rq->capacity * 2 : RUNQUEUE_INITIAL_CAP;
        if (new_cap > RUNQUEUE_MAX_CAP)
            new_cap = RUNQUEUE_MAX_CAP;
        if (new_cap <= rq->count || !runqueue_resize(rq, new_cap))
            return false;  // Truly full, or no memory to grow
    }

    uint32_t index = rq->count++;
    rq->procs[index] = proc;
    runqueue_sift_up(rq, index);
    __atomic_store_n(&rq->total, rq->count, __ATOMIC_RELEASE);
    return true;
}

struct process_t *runqueue_dequeue_best(RunQueue *rq)
{
    if (!rq || rq->count == 0)
        return NULL;
    return runqueue_take(rq, 0);
}

void runqueue_remove(RunQueue *rq, struct process_t *proc)
{
    if (runqueue_contains(rq, proc))
        runqueue_take(rq, (uint32_t)proc->rq_index);
}

void runqueue_settle(RunQueue *rq, struct process_t *proc)
{
    if (!runqueue_contains(rq, proc))
        return;
    uint32_t index = (uint32_t)proc->rq_index;
    if (index > 0 && proc->fare < rq->procs[(index - 1) / 2]->fare)
        runqueue_sift_up(rq, index);
    else
        runqueue_sift_down(rq, index);
}

bool runqueue_contains(RunQueue *rq, struct process_t *proc)
{
    if (!rq || !proc)
        return false;

    int32_t index = proc->rq_index;
    return index >= 0 && (uint32_t)index < rq->count && rq->procs[index] == proc;
}

struct process_t *runqueue_at(RunQueue *rq, uint32_t index)
{
    return (rq && index < rq->count) ? rq->procs[index] : NULL;
}

// Lock-free read of the running total. Safe to call without holding rq->lock.
// Uses ACQUIRE ordering so the caller sees all enqueue/dequeue side-effects
// that preceded the most recent __ATOMIC_RELEASE store.
uint32_t RunqueueAtomicTotal(const RunQueue *rq)
{
    if (!rq) return 0;
    return __atomic_load_n(&rq->total, __ATOMIC_ACQUIRE);
}
