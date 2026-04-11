#include "ready_queue.h"

void ready_queue_init(ReadyQueue* rq)
{
    if (!rq) return;
    rq->head = 0;
    rq->tail = 0;
    spinlock_init(&rq->lock);
    for (uint32_t i = 0; i < READY_QUEUE_CAPACITY; i++) {
        rq->entries[i] = NULL;
    }
}

bool ready_queue_push(ReadyQueue* rq, process_t* proc)
{
    if (!rq || !proc) return false;

    spin_lock(&rq->lock);

    uint32_t next_tail = (rq->tail + 1) % READY_QUEUE_CAPACITY;
    if (next_tail == rq->head) {
        spin_unlock(&rq->lock);
        return false;  // queue full
    }

    rq->entries[rq->tail] = proc;
    rq->tail = next_tail;

    spin_unlock(&rq->lock);
    return true;
}

process_t* ready_queue_pop(ReadyQueue* rq)
{
    if (!rq) return NULL;

    spin_lock(&rq->lock);

    if (rq->head == rq->tail) {
        spin_unlock(&rq->lock);
        return NULL;  // queue empty
    }

    process_t* proc = rq->entries[rq->head];
    rq->entries[rq->head] = NULL;
    rq->head = (rq->head + 1) % READY_QUEUE_CAPACITY;

    spin_unlock(&rq->lock);
    return proc;
}

// Lock-free check: safe because this is only called from the single-core
// sync path (guide()) where IRQs are disabled and no other core accesses
// the ready queue.  head/tail are volatile, so reads are not reordered.
bool ready_queue_is_empty(const ReadyQueue* rq)
{
    if (!rq) return true;
    return rq->head == rq->tail;
}

uint32_t ready_queue_count(const ReadyQueue* rq)
{
    if (!rq) return 0;
    uint32_t h = rq->head;
    uint32_t t = rq->tail;
    if (t >= h) return t - h;
    return READY_QUEUE_CAPACITY - (h - t);
}
