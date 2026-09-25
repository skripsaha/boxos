#include "baton.h"
#include "amp.h"
#include "atomics.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"
#include "lapic.h"
#include "irqchip.h"
#include "nameplate.h"        /* nameplate_name_at_kernel — name a one-way continuation */
#include "nameplate_format.h" /* NameplateHeader — the table's own minimum size */

/*
 * Vyukov intrusive MPSC with a persistent stub (1024cores.net,
 * "Intrusive MPSC node-based queue"). Producers XCHG the tail from any
 * core; a single consumer walks the head. See baton.h for the
 * never-drop rationale and the single-consumer deadlock-freedom argument.
 */

BatonQueue *g_baton       = NULL;
volatile uint8_t        g_baton_ready = 0;

/* One reserved node per core, for the only pass that cannot bring its own:
 * work that does not return outlives no container, so there is no job struct
 * left to embed a node in. Lives in the same pages as the queue array — see
 * BatonInit. Indexed by the core that TAKES the pass. */
static Baton *g_baton_oneway = NULL;

/* ==========================================================================
 *  Raw MP link — the one primitive both the public push and the consumer's
 *  stub re-anchor share. Allocation-free, IRQ-safe. Does NOT touch
 *  telemetry or routing, so re-anchoring the stub never perturbs the
 *  pushed/popped counters (the counter-drift the naive design would incur).
 * ========================================================================== */
static inline void baton_link(BatonQueue *q, Baton *n)
{
    __atomic_store_n(&n->next, NULL, __ATOMIC_RELAXED);
    /* LOCK XCHG — full fence. Publishes the producer as the new tail and
     * returns the prior tail, which we then link forward. */
    Baton *prev = __atomic_exchange_n(&q->tail, n, __ATOMIC_ACQ_REL);
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
static Baton *baton_pop(BatonQueue *q)
{
    Baton *head = q->head;                       /* SC-owned */
    Baton *next = __atomic_load_n(&head->next, __ATOMIC_ACQUIRE);

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
    baton_link(q, &q->stub);

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
void BatonInit(void)
{
    uint32_t n = g_amp.total_cores;
    if (n == 0) n = 1;

    /* From the PMM, for the same reason as the K-Core queue array: sized by
     * CORE COUNT, allocated once, never freed. The kernel heap is a fixed
     * small-object pool and must not carry per-machine-scale arrays. */
    size_t cq_bytes = sizeof(BatonQueue) * n;
    /* The reserved one-way nodes follow the queue array in the same
     * allocation: sizeof(BatonQueue) is a multiple of a pointer's alignment,
     * so the node array that starts at cq_bytes is aligned for a Baton. */
    size_t ow_bytes = sizeof(Baton) * n;
    size_t cq_pages = (cq_bytes + ow_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void  *cq_phys  = pmm_alloc_zero(cq_pages);
    char  *base     = cq_phys ? (char *)vmm_phys_to_virt((uintptr_t)cq_phys)
                              : NULL;
    g_baton        = (BatonQueue *)(void *)base;
    g_baton_oneway = base ? (Baton *)(void *)(base + cq_bytes) : NULL;
    if (!g_baton) {
        kprintf("[BATON] FATAL: cannot allocate queue array (%u cores, %zu pages)\n",
                n, cq_pages);
        while (1) { __asm__ volatile("cli; hlt"); }
    }

    for (uint32_t i = 0; i < n; i++) {
        BatonQueue *q = &g_baton[i];
        __atomic_store_n(&q->stub.next, NULL, __ATOMIC_RELAXED);
        q->stub.run = NULL;
        q->stub.ctx = NULL;
        q->head     = &q->stub;
        __atomic_store_n(&q->tail, &q->stub, __ATOMIC_RELAXED);
    }

    __atomic_store_n(&g_baton_ready, 1, __ATOMIC_RELEASE);
    debug_printf("[BATON] %u per-core never-drop queue(s) ready\n", n);
}

/* The half both producers share: publish the node and wake the core that will
 * take it. The caller has already decided WHICH queue and has already set the
 * node's one_way for this pass. */
static void baton_post(BatonQueue *q, Baton *n, uint8_t target)
{
    /* Both bookkeeping stores happen BEFORE the link, and for the same reason
     * the pop path copies run/ctx before it calls: the instant this node is
     * linked it is reachable by the consumer, which may pop it and let its
     * continuation free the whole container — node included — before this core
     * gets another instruction.
     *
     * Counting `pushed` before the link is also what makes BatonDepth's
     * subtraction safe. The link is what makes a node poppable, so a pass now
     * raises `pushed` strictly before its own `popped` can rise. Counting after
     * the link left a window — a producer interrupted between the two while the
     * drain core popped and counted the very same node — in which popped led
     * pushed and an unsigned depth read eighteen quintillion. The remaining
     * error is that a queue looks busy a few instructions early, which is the
     * direction a drain wants to be wrong in. */
    q->last_push_run = n->run;
    atomic_fetch_add_u64(&q->pushed, 1);

    baton_link(q, n);

    if (amp_get_core_index() != target)
        lapic_send_ipi(g_amp.cores[target].lapic_id, IPI_WAKE_VECTOR);
}

/* ==========================================================================
 *  Producer — BatonPass()
 *
 *  IRQ-safe, allocation-free, NEVER drops. Routes to the drain core (BSP,
 *  the AHCI MSI owner). A cross-core producer (an App-Core initial-kick
 *  handoff, an xHCI MSI on another core) sends IPI_WAKE so a HLT'd drain
 *  core wakes and pumps; a same-core producer (the BSP's own IRQ / pump)
 *  needs no IPI — it returns straight to kcore_run_loop, which pumps.
 * ========================================================================== */
bool BatonPass(Baton *n)
{
    /* Init runs before the sti that opens the machine to interrupts, so
     * nothing can pass a baton before the queues exist; a pass that does is
     * refused — and said, because a refused pass is exactly the drop this
     * queue exists to make impossible. */
    if (!__atomic_load_n(&g_baton_ready, __ATOMIC_ACQUIRE)) {
        kprintf("[BATON] ERROR: a baton was passed before BatonInit — the continuation is lost\n");
        return false;
    }

    uint8_t drain = g_amp.bsp_index;

    /* Every ordinary pass is return-counted, said here rather than assumed of
     * the container: a node reused across passes, or one whose container was
     * never zeroed, cannot carry a stale one-way marker into this pass. */
    __atomic_store_n(&n->one_way, 0u, __ATOMIC_RELAXED);

    baton_post(&g_baton[drain], n, drain);
    return true;
}

/* ==========================================================================
 *  Producer — BatonPassOneWay()
 *
 *  For the continuation that does not come back. See baton.h for the contract;
 *  the claim on the core's reserved node is the marker itself and is never
 *  cleared, so a core goes one-way at most once per boot.
 * ========================================================================== */
bool BatonPassOneWay(uint8_t core, void (*run)(void *ctx), void *ctx)
{
    if (!__atomic_load_n(&g_baton_ready, __ATOMIC_ACQUIRE)) {
        kprintf("[BATON] ERROR: a one-way pass was made before BatonInit — "
                "the machine was told to stop and nobody took the message\n");
        return false;
    }
    if (!run || core >= g_amp.total_cores) {
        kprintf("[BATON] ERROR: one-way pass refused — core %u of %u, "
                "continuation %p\n",
                core, g_amp.total_cores, (void *)(uintptr_t)run);
        return false;
    }

    Baton *n = &g_baton_oneway[core];

    /* LOCK CMPXCHG — a full fence, and the claim and the marker in one word.
     * A loser is not a drop: the pass already on its way takes the machine to
     * the same place this one wanted to. It is still said, because this queue
     * never swallows a pass in silence. */
    if (!atomic_cas_u32(&n->one_way, 0u, 1u)) {
        kprintf("[BATON] one-way pass to core %u refused — that core is "
                "already taking one\n", core);
        return false;
    }

    /* Safe to write plainly: the node is not linked yet, so no consumer can
     * reach it, and the claim above locked out every other producer. The link
     * inside baton_post is the release that publishes all three fields. */
    n->run = run;
    n->ctx = ctx;

    baton_post(&g_baton[core], n, core);
    return true;
}

/* The kernel's own name table. A machine about to stop must say what it is
 * stopping FOR, and a continuation pointer is only a number until the
 * Nameplate turns it back into the function that was going to run — the same
 * lookup the panic dump and the halt drain make, for the same reason. Never
 * fails loudly: an absent or too-small table yields the raw address. */
extern const char __nameplate_start[];
extern const char __nameplate_end[];

static void baton_say_one_way(uint8_t core_idx, void (*run)(void *ctx))
{
    NameplateName name;
    uint64_t bytes = (uint64_t)(__nameplate_end - __nameplate_start);

    if (bytes >= sizeof(NameplateHeader) &&
        nameplate_name_at_kernel((uintptr_t)__nameplate_start, bytes,
                                 (uintptr_t)run, &name))
        kprintf("[BATON] core %u takes a one-way pass: %s — control does not "
                "come back\n", core_idx, name.Text);
    else
        kprintf("[BATON] core %u takes a one-way pass: %016lx — control does "
                "not come back\n", core_idx, (uint64_t)(uintptr_t)run);
}

/* ==========================================================================
 *  Consumer — BatonPump()
 *
 *  Single-consumer: only the owning core drains its queue. Every K-Core
 *  calls this with its own index from kcore_run_loop; non-drain queues stay
 *  empty and cost a single load.
 * ========================================================================== */
uint32_t BatonPump(uint8_t core_idx)
{
    if (!__atomic_load_n(&g_baton_ready, __ATOMIC_ACQUIRE)) return 0;
    if (core_idx >= g_amp.total_cores) return 0;
    /* Single-consumer contract: a foreign core draining this queue would
     * corrupt the SC-owned head. kcore_run_loop always passes my_idx. */
    if (core_idx != amp_get_core_index()) return 0;

    BatonQueue *q = &g_baton[core_idx];
    uint32_t ran = 0;

    for (;;) {
        Baton *n = baton_pop(q);
        if (!n) break;

        /* Copy run/ctx BEFORE the call: the continuation may free the
         * container (and its embedded node) before returning, or — a Knock
         * opened inside the run — let a producer link this very node again.
         * Once popped, n is unreachable from the queue, so both are safe. */
        void (*run)(void *) = n->run;
        void  *ctx          = n->ctx;
        bool   one_way      = __atomic_load_n(&n->one_way, __ATOMIC_RELAXED) != 0;

        /* Recorded BEFORE the call, not after, because the case this exists to
         * report is the continuation that never returns: a queue that will not
         * go quiet must be able to name the work standing in it. */
        q->last_pop_run = run;

        if (one_way) {
            /* Counted when TAKEN, because nothing comes back to count it: a
             * drain that waited for this pass would be waiting for the pass it
             * is running inside, which is the thirty seconds the power button
             * used to take before it gave up on itself.
             *
             * LOCK XADD is a full barrier and locked instructions have a
             * total order (Intel SDM Vol 3A §9.2.2; §8.2.2 in older
             * revisions), so every other core observes `popped` rise before
             * the continuation's first store — and the drain that runs INSIDE
             * the continuation sees it by plain program order. No extra fence
             * is owed: the acquire in baton_pop already made the marker
             * visible, and this add is itself the barrier that orders it
             * against the run.
             *
             * Counted exactly once even if a caller lied and the continuation
             * did return: there is no second count after the call. */
            baton_say_one_way(core_idx, run);
            atomic_fetch_add_u64(&q->popped, 1);
            run(ctx);
            ran++;
            continue;
        }

        run(ctx);
        /* Count AFTER the continuation returns: BatonOutstanding
         * (pushed != popped) then means "a completion is posted OR still
         * running", so halt-drain waits for a write's tagfs commit to finish
         * — not merely for the node to be dequeued. */
        atomic_fetch_add_u64(&q->popped, 1);
        ran++;
    }

    return ran;
}

/* ==========================================================================
 *  HLT-gate helper — BatonPending()
 *
 *  Structural (no counter): the queue holds a real node iff head is a real
 *  node, or the stub at head has a successor. Called by the owning core in
 *  its pre-HLT re-check so it never sleeps on a queued completion.
 * ========================================================================== */
bool BatonPending(uint8_t core_idx)
{
    if (!__atomic_load_n(&g_baton_ready, __ATOMIC_ACQUIRE)) return false;
    if (core_idx >= g_amp.total_cores) return false;

    BatonQueue *q = &g_baton[core_idx];
    Baton *head = q->head;                       /* SC-owned */
    if (head != &q->stub) return true;                       /* real node at front */
    return __atomic_load_n(&q->stub.next, __ATOMIC_ACQUIRE) != NULL;
}

/* ==========================================================================
 *  Halt-drain helper — BatonOutstanding()
 *
 *  Cross-core-safe: compares the atomic pushed/popped counters, so a core
 *  that does not own the queue can poll it during shutdown while the drain
 *  core is still pumping. The stub re-anchor is not counted, so at
 *  quiescence pushed == popped == real completions delivered.
 * ========================================================================== */
bool BatonOutstanding(uint8_t core_idx)
{
    if (!__atomic_load_n(&g_baton_ready, __ATOMIC_ACQUIRE)) return false;
    if (core_idx >= g_amp.total_cores) return false;

    BatonQueue *q = &g_baton[core_idx];
    return atomic_load_u64(&q->pushed) != atomic_load_u64(&q->popped);
}

/* ==========================================================================
 *  Halt-drain helper — BatonDepth()
 *
 *  What BatonOutstanding could not say: HOW MUCH. The counters were always
 *  there; collapsing them to a bool is what made a shutdown report "one unit
 *  of storage work" when it had no idea whether that was one pass or a
 *  thousand, and a measure that cannot fall cannot show progress either.
 *
 *  POPPED IS READ FIRST, and that ordering is the whole correctness argument.
 *  Both counters only ever rise, and a pass raises `pushed` strictly before
 *  its own `popped`. Sampling `popped` first can therefore only make the
 *  difference too LARGE — never negative, which in unsigned arithmetic would
 *  not be a small error but a depth of eighteen quintillion. A depth that errs
 *  high errs toward looking again; the other way errs toward a lie.
 * ========================================================================== */
uint64_t BatonDepth(uint8_t core_idx)
{
    if (!__atomic_load_n(&g_baton_ready, __ATOMIC_ACQUIRE)) return 0;
    if (core_idx >= g_amp.total_cores) return 0;

    BatonQueue *q = &g_baton[core_idx];
    uint64_t popped = atomic_load_u64(&q->popped);
    uint64_t pushed = atomic_load_u64(&q->pushed);
    return pushed - popped;
}

/* ==========================================================================
 *  Diagnostics — BatonLastSeen()
 *
 *  The continuation most recently pushed to, and most recently popped from,
 *  this core's queue. Both are plain function pointers snapshotted into the
 *  queue itself and NEVER dereferenced here — which is the point. The obvious
 *  alternative, reading `head->run` to see what is stuck, is not available to
 *  a core that does not own the queue: the node lives inside the caller's job
 *  and a continuation is entitled to free that job the moment it runs, so a
 *  foreign reader following head would be reading freed memory precisely when
 *  the machine is already in trouble.
 *
 *  The caller turns these into names through the Nameplate.
 * ========================================================================== */
void BatonLastSeen(uint8_t core_idx,
                   void (**out_pushed)(void *ctx),
                   void (**out_popped)(void *ctx))
{
    if (out_pushed) *out_pushed = NULL;
    if (out_popped) *out_popped = NULL;

    if (!__atomic_load_n(&g_baton_ready, __ATOMIC_ACQUIRE)) return;
    if (core_idx >= g_amp.total_cores) return;

    BatonQueue *q = &g_baton[core_idx];
    if (out_pushed) *out_pushed = q->last_push_run;
    if (out_popped) *out_popped = q->last_pop_run;
}

/* ==========================================================================
 *  Knock — see baton.h.
 *
 *  The claim is a SEQ_CST CAS (LOCK CMPXCHG, a full fence): every store the
 *  producer made to its place is globally visible before the claim is, so a
 *  reader that opens the door after this claim and then looks at the place
 *  sees them. The open is a SEQ_CST store (MOV + MFENCE) followed by the
 *  reader's look: on x86-TSO that fence is what forbids the look from being
 *  served before the open is visible — the Dekker half that makes "either
 *  the producer's CAS finds the door open, or the reader's look finds the
 *  producer's writes" a theorem rather than a hope.
 * ========================================================================== */
static inline bool knock_claim(Knock *k)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&k->raised, &expected, 1u, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

void KnockInit(Knock *k, void (*run)(void *ctx), void *ctx)
{
    __atomic_store_n(&k->baton.next, NULL, __ATOMIC_RELAXED);
    k->baton.run     = run;
    k->baton.ctx     = ctx;
    k->baton.one_way = 0u;      /* a knock always comes back; see BatonPass */
    __atomic_store_n(&k->raised, 0u, __ATOMIC_RELAXED);
}

void KnockOn(Knock *k)
{
    if (!knock_claim(k)) return;              /* a pass is already on its way */
    if (!BatonPass(&k->baton)) {
        /* Refused (before BatonInit, and said there). The door was never
         * knocked as far as the queue is concerned: reopen it, or this
         * place would be deaf for the rest of the boot. */
        __atomic_store_n(&k->raised, 0u, __ATOMIC_SEQ_CST);
    }
}

void KnockOpen(Knock *k)
{
    __atomic_store_n(&k->raised, 0u, __ATOMIC_SEQ_CST);
}

/* ==========================================================================
 *  Self-test — BatonSelfTest()
 *
 *  Deterministic, single-core proof of the primitive. Runs on a PRIVATE
 *  queue (not a live per-core queue) so it is safe to call at boot before
 *  any drain core is looping.
 *
 *  Phase 1 (never-drop): push N nodes — N far beyond any pre-allocated
 *  place in this kernel (the Touch IRQ ring, an MCE slot pool, a GHES
 *  source table) — WITHOUT draining, then drain. A slot-based ring would
 *  have dropped; the embedded node carries its own link, so all N survive.
 *  Assert every node delivered exactly once, in per-producer FIFO order.
 *
 *  Phase 2 (edge cases): push/drain in tiny batches so the queue empties
 *  every round, forcing the stub re-anchor and the single-node /
 *  transient-empty pop paths the bulk burst never reaches. Assert
 *  exactly-once again.
 *
 *  Phase 3 (the knock): many claims before the door opens are one pass; a
 *  claim after the open is one more. The gate is exercised on the private
 *  queue through the same claim the live KnockOn uses.
 * ========================================================================== */
static void baton_selftest_probe(void *ctx)
{
    uint32_t *seen = (uint32_t *)ctx;
    (*seen)++;
}

error_t BatonSelfTest(void)
{
    const uint32_t N = 8192;

    Baton *nodes = (Baton *)kmalloc(sizeof(Baton) * N);
    uint32_t          *seen  = (uint32_t *)kmalloc(sizeof(uint32_t) * N);
    if (!nodes || !seen) {
        if (nodes) kfree(nodes);
        if (seen)  kfree(seen);
        kprintf("[BATON-TEST] SKIP (alloc)\n");
        return OK;   /* environment shortage, not a logic failure — don't block boot */
    }

    BatonQueue tq;
    memset(&tq, 0, sizeof(tq));
    tq.stub.run = NULL;
    tq.stub.ctx = NULL;
    __atomic_store_n(&tq.stub.next, NULL, __ATOMIC_RELAXED);
    tq.head = &tq.stub;
    __atomic_store_n(&tq.tail, &tq.stub, __ATOMIC_RELAXED);

    for (uint32_t i = 0; i < N; i++) {
        seen[i]          = 0;
        nodes[i].run     = baton_selftest_probe;
        nodes[i].ctx     = &seen[i];
        nodes[i].one_way = 0u;   /* kmalloc does not zero; a node is born two-way */
    }

    /* ---- Phase 1: never-drop burst ---- */
    for (uint32_t i = 0; i < N; i++)
        baton_link(&tq, &nodes[i]);

    uint32_t popped = 0, misorder = 0, prev = 0;
    bool first = true;
    for (;;) {
        Baton *n = baton_pop(&tq);
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
            baton_link(&tq, &nodes[pushed_i++]);
        Baton *n;
        while ((n = baton_pop(&tq)) != NULL) { n->run(n->ctx); checked++; }
    }
    uint32_t inter_bad = 0;
    for (uint32_t i = 0; i < N; i++)
        if (seen[i] != 1) inter_bad++;

    /* ---- Phase 3: the knock ---- */
    uint32_t visits = 0;
    Knock knock;
    KnockInit(&knock, baton_selftest_probe, &visits);
    uint32_t won_before_open = 0;
    for (uint32_t i = 0; i < 1000; i++)
        if (knock_claim(&knock)) { baton_link(&tq, &knock.baton); won_before_open++; }
    uint32_t knock_runs = 0;
    for (;;) {
        Baton *n = baton_pop(&tq);
        if (!n) break;
        KnockOpen(&knock);                     /* once, as the continuation does */
        n->run(n->ctx);
        knock_runs++;
    }
    uint32_t won_after_open = 0;
    if (knock_claim(&knock)) { baton_link(&tq, &knock.baton); won_after_open++; }
    if (knock_claim(&knock)) won_after_open++;  /* must lose: the door is shut again */
    for (;;) {
        Baton *n = baton_pop(&tq);
        if (!n) break;
        KnockOpen(&knock);
        n->run(n->ctx);
        knock_runs++;
    }
    bool knock_ok = (won_before_open == 1 && won_after_open == 1 &&
                     knock_runs == 2 && visits == 2);

    kprintf("[BATON-TEST] never-drop N=%u pushed=%u popped=%u delivered=%u dup=%u lost=%u misorder=%u | "
            "interleave checked=%u bad=%u | knock: 1000 knocks -> %u pass, after open -> %u, visits=%u\n",
            N, N, popped, delivered, dup, lost, misorder, checked, inter_bad,
            won_before_open, won_after_open, visits);

    kfree(nodes);
    kfree(seen);

    if (popped != N || delivered != N || dup != 0 || lost != 0 || misorder != 0 ||
        checked != N || inter_bad != 0 || !knock_ok) {
        kprintf("[BATON-TEST] FAIL\n");
        return ERR_IO;
    }
    kprintf("[BATON-TEST] PASS (never-drop + exactly-once + FIFO + stub-re-anchor + knock)\n");
    return OK;
}
