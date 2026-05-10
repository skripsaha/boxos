#include "write_cont_queue.h"
#include "amp.h"
#include "kcore.h"
#include "lapic.h"
#include "irqchip.h"
#include "atomics.h"
#include "klib.h"

WriteContQueue *g_write_cont_queues = NULL;

void WriteContQueueInit(void)
{
    uint32_t n = g_amp.total_cores;
    g_write_cont_queues = kmalloc(sizeof(WriteContQueue) * n);
    if (!g_write_cont_queues) {
        kprintf("[WCQ] FATAL: cannot allocate ring array (%u cores)\n", n);
        while (1) { __asm__ volatile("cli; hlt"); }
    }
    memset(g_write_cont_queues, 0, sizeof(WriteContQueue) * n);
    for (uint32_t i = 0; i < n; i++) {
        WriteContQueue *q = &g_write_cont_queues[i];
        q->kcore_idx     = (uint8_t)i;
        q->stub.fn       = NULL;
        q->stub.job      = NULL;
        q->stub.next     = NULL;
        q->head          = &q->stub;
        q->tail          = &q->stub;
        q->pending       = 0;
    }
    debug_printf("[WCQ] %u unbounded MPSC queue(s) initialized\n", n);
}

uint32_t WriteContDepth(uint8_t kcore_idx)
{
    if (!g_write_cont_queues || kcore_idx >= g_amp.total_cores) return 0;
    return atomic_load_u32(&g_write_cont_queues[kcore_idx].pending);
}

/* Vyukov MPSC push:
 *   1. Build node: fn/job/next=NULL.
 *   2. Atomic XCHG: prev = tail; tail = node.       (linearization)
 *   3. Store-release: prev->next = node.
 * The tiny window between (2) and (3) is when the queue looks "empty"
 * to the consumer (head->next still NULL even though tail moved). The
 * producer's IPI wakes the consumer after step (3); pump retries and
 * sees the new node. */
static error_t wcq_push(WriteContQueue *q, WriteContFn fn, void *job)
{
    if (!q || !fn) return ERR_INVALID_ARGUMENT;

    WriteContNode *node = (WriteContNode *)kmalloc(sizeof(WriteContNode));
    if (!node) return ERR_NO_MEMORY;

    node->fn   = fn;
    node->job  = job;
    node->next = NULL;
    /* Make the writes above visible before the link. */
    __atomic_thread_fence(__ATOMIC_RELEASE);

    /* Linearization: atomically take the previous tail and install us. */
    WriteContNode *prev = (WriteContNode *)__atomic_exchange_n(
        (WriteContNode * volatile *)&q->tail, node, __ATOMIC_ACQ_REL);

    /* Publish the link from prev → us (release; pairs with consumer
     * acquire when reading head->next). */
    __atomic_store_n(&prev->next, node, __ATOMIC_RELEASE);

    atomic_fetch_add_u32(&q->pending, 1);
    return OK;
}

static uint8_t wcq_find_least_loaded(void)
{
    uint8_t best = g_amp.bsp_index;
    uint32_t best_depth = UINT32_MAX;
    for (uint8_t i = 0; i < g_amp.total_cores; i++) {
        if (!g_amp.cores[i].is_kcore) continue;
        uint32_t d = WriteContDepth(i);
        if (d < best_depth) { best_depth = d; best = i; }
    }
    return best;
}

error_t WriteContEnqueue(WriteContFn fn, void *job)
{
    if (!fn) return ERR_INVALID_ARGUMENT;
    if (!g_write_cont_queues) return ERR_NOT_INITIALIZED;

    uint8_t target = wcq_find_least_loaded();
    error_t rc = wcq_push(&g_write_cont_queues[target], fn, job);
    if (rc != OK) {
        debug_printf("[WCQ] heap exhausted on K-Core %u (rc=%d)\n", target, rc);
        return rc;
    }

    /* Wake the target core; HLT'd K-Cores resume on this IPI vector. */
    lapic_send_ipi(g_amp.cores[target].lapic_id, IPI_WAKE_VECTOR);
    return OK;
}

/* Vyukov MPSC pop, single consumer (owning K-Core):
 *   first = head->next      // ACQUIRE — pairs with producer release
 *   if !first: empty
 *   advance: head = first   // logically consume `first` as new dummy
 *   read fn/job from first  // valid because producer released them
 *   free old_head           // unless it was the original embedded stub
 *   invoke fn(job)
 *
 * The dummy semantics: at any time `head` points to the "last consumed
 * node still alive". Producers writing prev->next where prev was
 * previous tail (which equals our current head until we advance to a
 * pushed node) — so head must NOT be freed until we advance past it.
 * Hence we free old_head AFTER `head = first`. */
uint32_t WriteContPump(uint8_t kcore_idx)
{
    if (!g_write_cont_queues || kcore_idx >= g_amp.total_cores) return 0;
    WriteContQueue *q = &g_write_cont_queues[kcore_idx];
    uint32_t drained = 0;

    for (;;) {
        WriteContNode *first =
            __atomic_load_n((WriteContNode * volatile *)&q->head->next,
                            __ATOMIC_ACQUIRE);
        if (!first) break;

        WriteContFn fn  = first->fn;
        void       *job = first->job;
        WriteContNode *old_head = q->head;

        /* Advance head — `first` becomes the live dummy. Producer that
         * just XCHG'd tail (with prev = old `first` reference) is fine,
         * since it writes prev->next, not anything in old_head. */
        q->head = first;

        /* Free old_head unless it's the embedded stub (which lives in
         * the queue struct itself). */
        if (old_head != &q->stub) {
            kfree(old_head);
        }

        atomic_fetch_sub_u32(&q->pending, 1);
        drained++;

        /* Run continuation. May push more — fully re-entrant safe. */
        fn(job);
    }

    return drained;
}
