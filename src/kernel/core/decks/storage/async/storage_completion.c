#include "storage_completion.h"
#include "amp.h"
#include "atomics.h"
#include "klib.h"
#include "lapic.h"
#include "irqchip.h"

/*
 * Vyukov intrusive MPSC with a persistent stub (1024cores.net,
 * "Intrusive MPSC node-based queue"). Producers XCHG the tail from any
 * core; a single consumer walks the head. See storage_completion.h for the
 * never-drop rationale and the single-consumer deadlock-freedom argument.
 */

StorageCompletionQueue *g_storage_cq       = NULL;
volatile uint8_t        g_storage_cq_ready = 0;

/* ==========================================================================
 *  Raw MP link — the one primitive both the public push and the consumer's
 *  stub re-anchor share. Allocation-free, IRQ-safe. Does NOT touch
 *  telemetry or routing, so re-anchoring the stub never perturbs the
 *  pushed/popped counters (the counter-drift the naive design would incur).
 * ========================================================================== */
static inline void scq_link(StorageCompletionQueue *q, StorageCompletion *n)
{
    __atomic_store_n(&n->next, NULL, __ATOMIC_RELAXED);
    /* LOCK XCHG — full fence. Publishes the producer as the new tail and
     * returns the prior tail, which we then link forward. */
    StorageCompletion *prev = __atomic_exchange_n(&q->tail, n, __ATOMIC_ACQ_REL);
    /* Release: the consumer's acquire-load of prev->next synchronises-with
     * this store, making n->run/ctx (and any if_status stashed before the
     * push) visible before the continuation runs. */
    __atomic_store_n(&prev->next, n, __ATOMIC_RELEASE);
}

/* ==========================================================================
 *  Single-consumer pop. Called ONLY by the queue's owning core.
 *
 *  Returns the next real node (never the stub), or NULL if the queue is
 *  empty OR a producer is mid-push (XCHG done, forward-link pending) — a
 *  benign transient the next pump resolves.
 * ========================================================================== */
static StorageCompletion *scq_pop(StorageCompletionQueue *q)
{
    StorageCompletion *head = q->head;                       /* SC-owned */
    StorageCompletion *next = __atomic_load_n(&head->next, __ATOMIC_ACQUIRE);

    if (head == &q->stub) {
        if (!next) return NULL;                              /* genuinely empty */
        q->head = next;                                      /* skip the stub */
        head    = next;
        next    = __atomic_load_n(&head->next, __ATOMIC_ACQUIRE);
    }

    if (next) {                                              /* head has a successor */
        q->head = next;
        return head;
    }

    /* head is the last real node. Either tail still points at it, or a
     * producer's forward-link is in flight. */
    if (head != __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE))
        return NULL;                                         /* in-flight push — retry later */

    /* head == tail == last real node. Re-anchor the stub so the queue stays
     * non-empty; then head can be returned. RAW link (no telemetry). */
    scq_link(q, &q->stub);

    next = __atomic_load_n(&head->next, __ATOMIC_ACQUIRE);
    if (next) {
        q->head = next;
        return head;
    }
    return NULL;                                             /* stub link in flight — retry */
}

/* ==========================================================================
 *  Init
 * ========================================================================== */
void StorageCompletionInit(void)
{
    uint32_t n = g_amp.total_cores;
    if (n == 0) n = 1;

    g_storage_cq = (StorageCompletionQueue *)kmalloc(sizeof(StorageCompletionQueue) * n);
    if (!g_storage_cq) {
        kprintf("[STORAGE_CQ] FATAL: cannot allocate queue array (%u cores)\n", n);
        while (1) { __asm__ volatile("cli; hlt"); }
    }
    memset(g_storage_cq, 0, sizeof(StorageCompletionQueue) * n);

    for (uint32_t i = 0; i < n; i++) {
        StorageCompletionQueue *q = &g_storage_cq[i];
        __atomic_store_n(&q->stub.next, NULL, __ATOMIC_RELAXED);
        q->stub.run = NULL;
        q->stub.ctx = NULL;
        q->head     = &q->stub;
        __atomic_store_n(&q->tail, &q->stub, __ATOMIC_RELAXED);
    }

    __atomic_store_n(&g_storage_cq_ready, 1, __ATOMIC_RELEASE);
    debug_printf("[STORAGE_CQ] %u per-core never-drop MPSC queue(s) ready\n", n);
}

/* ==========================================================================
 *  Producer — StorageCompletionPush()
 *
 *  IRQ-safe, allocation-free, NEVER drops. Routes to the drain core (BSP,
 *  the AHCI MSI owner). A cross-core producer (an App-Core initial-kick
 *  handoff) sends IPI_WAKE so a HLT'd drain core wakes and pumps; a
 *  same-core producer (the BSP's own MSI / pump) needs no IPI — it returns
 *  straight to kcore_run_loop, which pumps.
 * ========================================================================== */
void StorageCompletionPush(StorageCompletion *n)
{
    /* Unreachable in practice: storage async I/O is gated total_cores > 1
     * and only begins long after boot, whereas init runs right after
     * irq_defer_init. Guarded defensively to never touch uninit memory. */
    if (!__atomic_load_n(&g_storage_cq_ready, __ATOMIC_ACQUIRE)) return;

    uint8_t drain = g_amp.bsp_index;
    StorageCompletionQueue *q = &g_storage_cq[drain];

    scq_link(q, n);
    atomic_fetch_add_u64(&q->pushed, 1);

    uint8_t me = amp_get_core_index();
    if (me != drain)
        lapic_send_ipi(g_amp.cores[drain].lapic_id, IPI_WAKE_VECTOR);
}

/* ==========================================================================
 *  Consumer — StorageCompletionPump()
 *
 *  Single-consumer: only the owning core drains its queue. Every K-Core
 *  calls this with its own index from kcore_run_loop; non-drain queues stay
 *  empty and cost a single load.
 * ========================================================================== */
uint32_t StorageCompletionPump(uint8_t core_idx)
{
    if (!__atomic_load_n(&g_storage_cq_ready, __ATOMIC_ACQUIRE)) return 0;
    if (core_idx >= g_amp.total_cores) return 0;
    /* Single-consumer contract: a foreign core draining this queue would
     * corrupt the SC-owned head. kcore_run_loop always passes my_idx. */
    if (core_idx != amp_get_core_index()) return 0;

    StorageCompletionQueue *q = &g_storage_cq[core_idx];
    uint32_t ran = 0;

    for (;;) {
        StorageCompletion *n = scq_pop(q);
        if (!n) break;

        /* Copy run/ctx BEFORE the call: the continuation may free the
         * container (and its embedded node) before returning. Once popped,
         * n is unreachable from the queue, so the free is safe. */
        void (*run)(void *) = n->run;
        void  *ctx          = n->ctx;

        run(ctx);
        /* Count AFTER the continuation returns: StorageCompletionOutstanding
         * (pushed != popped) then means "a completion is posted OR still
         * running", so halt-drain waits for a write's tagfs commit to finish
         * — not merely for the node to be dequeued. */
        atomic_fetch_add_u64(&q->popped, 1);
        ran++;
    }

    return ran;
}

/* ==========================================================================
 *  HLT-gate helper — StorageCompletionPending()
 *
 *  Structural (no counter): the queue holds a real node iff head is a real
 *  node, or the stub at head has a successor. Called by the owning core in
 *  its pre-HLT re-check so it never sleeps on a queued completion.
 * ========================================================================== */
bool StorageCompletionPending(uint8_t core_idx)
{
    if (!__atomic_load_n(&g_storage_cq_ready, __ATOMIC_ACQUIRE)) return false;
    if (core_idx >= g_amp.total_cores) return false;

    StorageCompletionQueue *q = &g_storage_cq[core_idx];
    StorageCompletion *head = q->head;                       /* SC-owned */
    if (head != &q->stub) return true;                       /* real node at front */
    return __atomic_load_n(&q->stub.next, __ATOMIC_ACQUIRE) != NULL;
}

/* ==========================================================================
 *  Halt-drain helper — StorageCompletionOutstanding()
 *
 *  Cross-core-safe: compares the atomic pushed/popped counters, so a core
 *  that does not own the queue can poll it during shutdown while the drain
 *  core is still pumping. The stub re-anchor is not counted, so at
 *  quiescence pushed == popped == real completions delivered.
 * ========================================================================== */
bool StorageCompletionOutstanding(uint8_t core_idx)
{
    if (!__atomic_load_n(&g_storage_cq_ready, __ATOMIC_ACQUIRE)) return false;
    if (core_idx >= g_amp.total_cores) return false;

    StorageCompletionQueue *q = &g_storage_cq[core_idx];
    return atomic_load_u64(&q->pushed) != atomic_load_u64(&q->popped);
}

/* ==========================================================================
 *  Self-test — StorageCompletionSelfTest()
 *
 *  Deterministic, single-core proof of the primitive. Runs on a PRIVATE
 *  queue (not a live per-core queue) so it is safe to call at boot before
 *  any drain core is looping.
 *
 *  Phase 1 (never-drop): push N nodes with N far beyond any fixed-slot ring
 *  (irq_defer's un-pumped chain is a single CONFIG_IRQ_DEFER_INITIAL_CAPACITY
 *  chunk, and even a fully grown chunk caps at CONFIG_IRQ_DEFER_MAX_CHUNK_
 *  CAPACITY) WITHOUT draining, then drain. A slot-based ring would have
 *  dropped; the embedded node carries its own link, so all N survive. Assert
 *  every node delivered exactly once, in per-producer FIFO order.
 *
 *  Phase 2 (edge cases): push/drain in tiny batches so the queue empties every
 *  round, forcing the stub re-anchor and the single-node / transient-empty pop
 *  paths the bulk burst never reaches. Assert exactly-once again.
 * ========================================================================== */
static void scq_selftest_probe(void *ctx)
{
    uint32_t *seen = (uint32_t *)ctx;
    (*seen)++;
}

error_t StorageCompletionSelfTest(void)
{
    const uint32_t N = 8192;   /* 8x CONFIG_IRQ_DEFER_MAX_CHUNK_CAPACITY */

    StorageCompletion *nodes = (StorageCompletion *)kmalloc(sizeof(StorageCompletion) * N);
    uint32_t          *seen  = (uint32_t *)kmalloc(sizeof(uint32_t) * N);
    if (!nodes || !seen) {
        if (nodes) kfree(nodes);
        if (seen)  kfree(seen);
        kprintf("[SCQ-TEST] SKIP (alloc)\n");
        return OK;   /* environment shortage, not a logic failure — don't block boot */
    }

    StorageCompletionQueue tq;
    memset(&tq, 0, sizeof(tq));
    tq.stub.run = NULL;
    tq.stub.ctx = NULL;
    __atomic_store_n(&tq.stub.next, NULL, __ATOMIC_RELAXED);
    tq.head = &tq.stub;
    __atomic_store_n(&tq.tail, &tq.stub, __ATOMIC_RELAXED);

    for (uint32_t i = 0; i < N; i++) {
        seen[i]        = 0;
        nodes[i].run   = scq_selftest_probe;
        nodes[i].ctx   = &seen[i];
    }

    /* ---- Phase 1: never-drop burst ---- */
    for (uint32_t i = 0; i < N; i++)
        scq_link(&tq, &nodes[i]);

    uint32_t popped = 0, misorder = 0, prev = 0;
    bool first = true;
    for (;;) {
        StorageCompletion *n = scq_pop(&tq);
        if (!n) break;
        uint32_t idx = (uint32_t)(n->ctx == NULL ? 0 : ((uint32_t *)n->ctx - seen));
        if (!first && idx != prev + 1) misorder++;
        prev  = idx;
        first = false;
        n->run(n->ctx);
        popped++;
    }

    uint32_t delivered = 0, dup = 0;
    for (uint32_t i = 0; i < N; i++) {
        if (seen[i] == 1)      delivered++;
        else if (seen[i] > 1)  dup++;
    }
    uint32_t lost = N - delivered;

    /* ---- Phase 2: empty/re-anchor edge cases ---- */
    for (uint32_t i = 0; i < N; i++) seen[i] = 0;
    static const uint32_t batches[8] = { 1, 1, 2, 1, 3, 1, 5, 1 };
    uint32_t pushed_i = 0, checked = 0;
    while (pushed_i < N) {
        uint32_t b = batches[pushed_i & 7];
        for (uint32_t k = 0; k < b && pushed_i < N; k++)
            scq_link(&tq, &nodes[pushed_i++]);
        StorageCompletion *n;
        while ((n = scq_pop(&tq)) != NULL) { n->run(n->ctx); checked++; }
    }
    uint32_t inter_bad = 0;
    for (uint32_t i = 0; i < N; i++)
        if (seen[i] != 1) inter_bad++;

    kprintf("[SCQ-TEST] never-drop N=%u pushed=%u popped=%u delivered=%u dup=%u lost=%u misorder=%u | "
            "interleave checked=%u bad=%u\n",
            N, N, popped, delivered, dup, lost, misorder, checked, inter_bad);

    kfree(nodes);
    kfree(seen);

    if (popped != N || delivered != N || dup != 0 || lost != 0 || misorder != 0 ||
        checked != N || inter_bad != 0) {
        kprintf("[SCQ-TEST] FAIL\n");
        return ERR_IO;
    }
    kprintf("[SCQ-TEST] PASS (never-drop + exactly-once + FIFO + stub-re-anchor)\n");
    return OK;
}
