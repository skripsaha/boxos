#include "ready_queue.h"
#include "process.h"

void ready_queue_init(ReadyQueue *rq) {
    if (!rq) return;
    rq->head  = NULL;
    rq->tail  = NULL;
    rq->count = 0;
    spinlock_init(&rq->lock);
}

bool ready_queue_push(ReadyQueue *rq, process_t *proc) {
    if (!rq || !proc) return false;

    // CAS guard: prevent double-enqueue from concurrent cores
    uint8_t expected = 0;
    if (!__atomic_compare_exchange_n(&proc->in_ready, &expected, 1,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        return false;
    }

    proc->ready_next = NULL;

    spin_lock(&rq->lock);
    if (rq->tail) {
        rq->tail->ready_next = proc;
    } else {
        rq->head = proc;
    }
    rq->tail = proc;
    rq->count++;
    spin_unlock(&rq->lock);
    return true;
}

process_t *ready_queue_pop(ReadyQueue *rq) {
    if (!rq) return NULL;

    spin_lock(&rq->lock);
    process_t *proc = rq->head;
    if (!proc) {
        spin_unlock(&rq->lock);
        return NULL;
    }
    rq->head = proc->ready_next;
    if (!rq->head) rq->tail = NULL;
    rq->count--;
    spin_unlock(&rq->lock);

    proc->ready_next = NULL;
    __atomic_store_n(&proc->in_ready, 0, __ATOMIC_RELEASE);
    return proc;
}

bool ready_queue_is_empty(const ReadyQueue *rq) {
    if (!rq) return true;
    return rq->head == NULL;
}

uint32_t ready_queue_count(const ReadyQueue *rq) {
    if (!rq) return 0;
    return rq->count;
}
