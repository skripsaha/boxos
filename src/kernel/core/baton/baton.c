#include "baton.h"
#include "amp.h"
#include "atomics.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"
#include "lapic.h"
#include "irqchip.h"


BatonQueue *g_baton       = NULL;
volatile uint8_t        g_baton_ready = 0;

static inline void baton_link(BatonQueue *q, Baton *n)
{
    __atomic_store_n(&n->next, NULL, __ATOMIC_RELAXED);
    Baton *prev = __atomic_exchange_n(&q->tail, n, __ATOMIC_ACQ_REL);
    __atomic_store_n(&prev->next, n, __ATOMIC_RELEASE);
}

static Baton *baton_pop(BatonQueue *q)
{
    Baton *head = q->head;
    Baton *next = __atomic_load_n(&head->next, __ATOMIC_ACQUIRE);

    if (head == &q->stub) {
        if (!next) return NULL;
        q->head = next;
        head    = next;
        next    = __atomic_load_n(&head->next, __ATOMIC_ACQUIRE);
    }

    if (next) {
        q->head = next;
        return head;
    }

    if (head != __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE))
        return NULL;

    baton_link(q, &q->stub);

    next = __atomic_load_n(&head->next, __ATOMIC_ACQUIRE);
    if (next) {
        q->head = next;
        return head;
    }
    return NULL;
}

void BatonInit(void)
{
    uint32_t n = g_amp.total_cores;
    if (n == 0) n = 1;

    size_t cq_bytes = sizeof(BatonQueue) * n;
    size_t cq_pages = (cq_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void  *cq_phys  = pmm_alloc_zero(cq_pages);
    g_baton = cq_phys ? (BatonQueue *)vmm_phys_to_virt((uintptr_t)cq_phys)
                           : NULL;
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

bool BatonPass(Baton *n)
{
    if (!__atomic_load_n(&g_baton_ready, __ATOMIC_ACQUIRE)) {
        kprintf("[BATON] ERROR: a baton was passed before BatonInit — the continuation is lost\n");
        return false;
    }

    uint8_t drain = g_amp.bsp_index;
    BatonQueue *q = &g_baton[drain];

    baton_link(q, n);
    atomic_fetch_add_u64(&q->pushed, 1);

    uint8_t me = amp_get_core_index();
    if (me != drain)
        lapic_send_ipi(g_amp.cores[drain].lapic_id, IPI_WAKE_VECTOR);
    return true;
}

uint32_t BatonPump(uint8_t core_idx)
{
    if (!__atomic_load_n(&g_baton_ready, __ATOMIC_ACQUIRE)) return 0;
    if (core_idx >= g_amp.total_cores) return 0;
    if (core_idx != amp_get_core_index()) return 0;

    BatonQueue *q = &g_baton[core_idx];
    uint32_t ran = 0;

    for (;;) {
        Baton *n = baton_pop(q);
        if (!n) break;

        void (*run)(void *) = n->run;
        void  *ctx          = n->ctx;

        run(ctx);
        atomic_fetch_add_u64(&q->popped, 1);
        ran++;
    }

    return ran;
}

bool BatonPending(uint8_t core_idx)
{
    if (!__atomic_load_n(&g_baton_ready, __ATOMIC_ACQUIRE)) return false;
    if (core_idx >= g_amp.total_cores) return false;

    BatonQueue *q = &g_baton[core_idx];
    Baton *head = q->head;
    if (head != &q->stub) return true;
    return __atomic_load_n(&q->stub.next, __ATOMIC_ACQUIRE) != NULL;
}

bool BatonOutstanding(uint8_t core_idx)
{
    if (!__atomic_load_n(&g_baton_ready, __ATOMIC_ACQUIRE)) return false;
    if (core_idx >= g_amp.total_cores) return false;

    BatonQueue *q = &g_baton[core_idx];
    return atomic_load_u64(&q->pushed) != atomic_load_u64(&q->popped);
}

static inline bool knock_claim(Knock *k)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&k->raised, &expected, 1u, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

void KnockInit(Knock *k, void (*run)(void *ctx), void *ctx)
{
    __atomic_store_n(&k->baton.next, NULL, __ATOMIC_RELAXED);
    k->baton.run = run;
    k->baton.ctx = ctx;
    __atomic_store_n(&k->raised, 0u, __ATOMIC_RELAXED);
}

void KnockOn(Knock *k)
{
    if (!knock_claim(k)) return;
    if (!BatonPass(&k->baton)) {
        __atomic_store_n(&k->raised, 0u, __ATOMIC_SEQ_CST);
    }
}

void KnockOpen(Knock *k)
{
    __atomic_store_n(&k->raised, 0u, __ATOMIC_SEQ_CST);
}

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
        return OK;
    }

    BatonQueue tq;
    memset(&tq, 0, sizeof(tq));
    tq.stub.run = NULL;
    tq.stub.ctx = NULL;
    __atomic_store_n(&tq.stub.next, NULL, __ATOMIC_RELAXED);
    tq.head = &tq.stub;
    __atomic_store_n(&tq.tail, &tq.stub, __ATOMIC_RELAXED);

    for (uint32_t i = 0; i < N; i++) {
        seen[i]        = 0;
        nodes[i].run   = baton_selftest_probe;
        nodes[i].ctx   = &seen[i];
    }

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
        KnockOpen(&knock);
        n->run(n->ctx);
        knock_runs++;
    }
    uint32_t won_after_open = 0;
    if (knock_claim(&knock)) { baton_link(&tq, &knock.baton); won_after_open++; }
    if (knock_claim(&knock)) won_after_open++;
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