#include "irq_defer.h"
#include "amp.h"
#include "klib.h"
#include "atomics.h"
#include "kernel_config.h"

/* Initial and maximum chunk sizes — see kernel_config.h for production
 * tuning. The chain grows in doubling steps until either capacity is hit
 * or steady-state has been reached. */
#ifndef CONFIG_IRQ_DEFER_INITIAL_CAPACITY
#define CONFIG_IRQ_DEFER_INITIAL_CAPACITY  16U
#endif
#ifndef CONFIG_IRQ_DEFER_MAX_CHUNK_CAPACITY
#define CONFIG_IRQ_DEFER_MAX_CHUNK_CAPACITY 1024U
#endif

IrqDeferCore     *g_irq_defer            = NULL;
CoreHazard       *g_pump_hazards         = NULL;
volatile uint8_t  g_irq_defer_ready      = 0;
volatile uint64_t g_irq_defer_predropped = 0;

/* ==========================================================================
 *  Chunk allocation / reset
 * ========================================================================== */

static IrqDeferChunk *chunk_alloc(uint32_t capacity)
{
    size_t bytes = sizeof(IrqDeferChunk) + (size_t)capacity * sizeof(IrqDeferSlot);
    IrqDeferChunk *ch = (IrqDeferChunk *)kmalloc(bytes);
    if (!ch) return NULL;
    memset(ch, 0, bytes);
    ch->capacity = capacity;
    return ch;
}

static void chunk_reset(IrqDeferChunk *ch)
{
    /* Slot.ready=0 is the "untouched" sentinel the consumer relies on
     * before CAS-claim. Reset all bookkeeping cursors. */
    for (uint32_t i = 0; i < ch->capacity; i++) {
        ch->slots[i].handler = NULL;
        ch->slots[i].ctx     = NULL;
        ch->slots[i].ready   = 0;
    }
    ch->prod_idx       = 0;
    ch->cons_idx       = 0;
    ch->consumed_count = 0;
    ch->next           = NULL;
}

/* ==========================================================================
 *  Free-list and retired-list management (protected by free_lock)
 *
 *  The MPMC pump can have multiple cores push/pop these stacks concurrently.
 *  A raw CAS-stack is ABA-prone here because the same chunk pointer cycles
 *  through retired → free → in-use → retired during steady state. A short
 *  spinlock around the head-pointer transitions is simpler than tagged
 *  pointers or hazard-protected pops, and the contention surface is tiny
 *  (every couple of pump-tails, not the hot per-slot path).
 * ========================================================================== */

static void free_list_push(IrqDeferCore *c, IrqDeferChunk *ch)
{
    chunk_reset(ch);
    spin_lock(&c->free_lock);
    ch->next       = (volatile struct IrqDeferChunk *)c->free_head;
    c->free_head   = ch;
    spin_unlock(&c->free_lock);
    atomic_fetch_add_u64(&c->chunks_recycled, 1);
}

static IrqDeferChunk *free_list_pop(IrqDeferCore *c)
{
    spin_lock(&c->free_lock);
    IrqDeferChunk *ch = c->free_head;
    if (ch) {
        c->free_head = (IrqDeferChunk *)ch->next;
        ch->next     = NULL;
    }
    spin_unlock(&c->free_lock);
    return ch;
}

static void retired_list_push(IrqDeferCore *c, IrqDeferChunk *ch)
{
    spin_lock(&c->free_lock);
    ch->next         = (volatile struct IrqDeferChunk *)c->retired_head;
    c->retired_head  = ch;
    spin_unlock(&c->free_lock);
}

/* Pop the entire retired list at once (caller will iterate, freeing
 * eligible chunks and re-pushing the rest). */
static IrqDeferChunk *retired_list_drain(IrqDeferCore *c)
{
    spin_lock(&c->free_lock);
    IrqDeferChunk *head = c->retired_head;
    c->retired_head     = NULL;
    spin_unlock(&c->free_lock);
    return head;
}

/* ==========================================================================
 *  Reclaim — runs at the tail of every pump call.
 *
 *  Walks the retired list. For each chunk, eligibility requires:
 *    1. consumed_count == capacity   (all in-flight handlers returned)
 *    2. no hazard pointer == this chunk (no pumper still references it)
 *  Ineligible chunks are requeued for the next pass.
 * ========================================================================== */

static bool chunk_is_unhazardous(const IrqDeferChunk *ch)
{
    uint32_t n = g_amp.total_cores;
    for (uint32_t i = 0; i < n; i++) {
        IrqDeferChunk *hp = (IrqDeferChunk *)__atomic_load_n(
            (void *volatile *)&g_pump_hazards[i].chunk, __ATOMIC_ACQUIRE);
        if (hp == ch) return false;
    }
    return true;
}

static void reclaim_retired(IrqDeferCore *c)
{
    if (!c->retired_head) return;  /* fast path */

    IrqDeferChunk *head = retired_list_drain(c);
    IrqDeferChunk *requeue_head = NULL;

    while (head) {
        IrqDeferChunk *next = (IrqDeferChunk *)head->next;
        head->next = NULL;

        bool eligible = (atomic_load_u32(&head->consumed_count) >= head->capacity)
                     && chunk_is_unhazardous(head);

        if (eligible) {
            free_list_push(c, head);
        } else {
            head->next   = (volatile struct IrqDeferChunk *)requeue_head;
            requeue_head = head;
        }
        head = next;
    }

    if (requeue_head) {
        /* Re-attach the not-yet-eligible chunks back at the head. */
        spin_lock(&c->free_lock);
        IrqDeferChunk *tail = requeue_head;
        while (tail->next) tail = (IrqDeferChunk *)tail->next;
        tail->next       = (volatile struct IrqDeferChunk *)c->retired_head;
        c->retired_head  = requeue_head;
        spin_unlock(&c->free_lock);
    }
}

/* ==========================================================================
 *  Refill — ensure the producer chain has headroom.
 * ========================================================================== */

static void refill_producer_chain(IrqDeferCore *c)
{
    IrqDeferChunk *prod = (IrqDeferChunk *)__atomic_load_n(
        (void *volatile *)&c->prod_chunk, __ATOMIC_ACQUIRE);
    if (!prod) return;
    IrqDeferChunk *prod_next = (IrqDeferChunk *)__atomic_load_n(
        (void *volatile *)&prod->next, __ATOMIC_ACQUIRE);
    if (prod_next) return;  /* already has a successor */

    /* Prefer recycled chunks; fall back to a fresh kmalloc that doubles
     * capacity (up to the per-chunk ceiling). */
    IrqDeferChunk *fresh = free_list_pop(c);
    if (!fresh) {
        uint32_t new_cap = prod->capacity * 2u;
        if (new_cap > (uint32_t)CONFIG_IRQ_DEFER_MAX_CHUNK_CAPACITY)
            new_cap = (uint32_t)CONFIG_IRQ_DEFER_MAX_CHUNK_CAPACITY;
        if (new_cap < prod->capacity) new_cap = prod->capacity;
        fresh = chunk_alloc(new_cap);
        if (fresh) atomic_fetch_add_u64(&c->chunks_allocated, 1);
    }
    if (!fresh) return;

    /* Publish as prod->next. If another pump beat us, recycle ours. */
    void *expected = NULL;
    if (!__atomic_compare_exchange_n((void *volatile *)&prod->next,
                                     &expected, fresh, false,
                                     __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        free_list_push(c, fresh);
    }
}

/* ==========================================================================
 *  Init
 * ========================================================================== */

void irq_defer_init(void)
{
    uint32_t n = g_amp.total_cores;
    if (n == 0) n = 1;

    g_irq_defer = (IrqDeferCore *)kmalloc(sizeof(IrqDeferCore) * n);
    if (!g_irq_defer) {
        kprintf("[IRQ_DEFER] FATAL: cannot allocate ring array (%u cores)\n", n);
        while (1) { __asm__ volatile("cli; hlt"); }
    }
    memset(g_irq_defer, 0, sizeof(IrqDeferCore) * n);

    g_pump_hazards = (CoreHazard *)kmalloc(sizeof(CoreHazard) * n);
    if (!g_pump_hazards) {
        kprintf("[IRQ_DEFER] FATAL: cannot allocate hazard table (%u cores)\n", n);
        while (1) { __asm__ volatile("cli; hlt"); }
    }
    memset(g_pump_hazards, 0, sizeof(CoreHazard) * n);

    for (uint32_t i = 0; i < n; i++) {
        spinlock_init(&g_irq_defer[i].free_lock);
        IrqDeferChunk *initial = chunk_alloc(CONFIG_IRQ_DEFER_INITIAL_CAPACITY);
        if (!initial) {
            kprintf("[IRQ_DEFER] FATAL: cannot allocate initial chunk for core %u\n", i);
            while (1) { __asm__ volatile("cli; hlt"); }
        }
        g_irq_defer[i].prod_chunk = (volatile struct IrqDeferChunk *)initial;
        g_irq_defer[i].cons_chunk = (volatile struct IrqDeferChunk *)initial;
        atomic_store_u64(&g_irq_defer[i].chunks_allocated, 1);
    }

    __atomic_store_n(&g_irq_defer_ready, 1, __ATOMIC_RELEASE);

    debug_printf("[IRQ_DEFER] MPMC: %u per-core ring(s) ready, "
                 "initial capacity %u, max chunk %u\n",
                 n, (uint32_t)CONFIG_IRQ_DEFER_INITIAL_CAPACITY,
                 (uint32_t)CONFIG_IRQ_DEFER_MAX_CHUNK_CAPACITY);
}

/* ==========================================================================
 *  Producer — irq_defer()
 *
 *  Lock-free, allocation-free. Single producer per ring in current use
 *  (per-core IRQ serialisation guarantees this), but written MP-safely via
 *  atomic fetch_add on prod_idx and CAS on prod_chunk so remote-producer
 *  use cases (cross-core notifications, future RT extensions) work too.
 * ========================================================================== */

void irq_defer(void (*handler)(void *), void *ctx)
{
    if (!__atomic_load_n(&g_irq_defer_ready, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&g_irq_defer_predropped, 1, __ATOMIC_RELAXED);
        return;
    }
    if (!handler) return;

    uint8_t core = amp_get_core_index();
    if (core >= g_amp.total_cores) core = g_amp.bsp_index;
    IrqDeferCore *c = &g_irq_defer[core];

    /* Bounded loop: retry only enough times to chase legitimate chunk
     * advances. 4 iterations covers the worst-case race-stack we can
     * generate with a single producer (chunk-advance + concurrent CAS
     * loss). */
    for (int attempt = 0; attempt < 4; attempt++) {
        IrqDeferChunk *ch = (IrqDeferChunk *)__atomic_load_n(
            (void *volatile *)&c->prod_chunk, __ATOMIC_ACQUIRE);

        uint32_t idx = atomic_fetch_add_u32(&ch->prod_idx, 1);
        if (idx < ch->capacity) {
            ch->slots[idx].handler = handler;
            ch->slots[idx].ctx     = ctx;
            /* Release publishes the handler/ctx writes before ready. */
            __atomic_store_n(&ch->slots[idx].ready, 1u, __ATOMIC_RELEASE);
            return;
        }

        /* Chunk full. Advance to pre-allocated successor. */
        IrqDeferChunk *next = (IrqDeferChunk *)__atomic_load_n(
            (void *volatile *)&ch->next, __ATOMIC_ACQUIRE);
        if (!next) {
            atomic_fetch_add_u64(&c->overflow_count, 1);
            return;
        }
        void *expected = (void *)ch;
        __atomic_compare_exchange_n((void *volatile *)&c->prod_chunk,
                                    &expected, next, false,
                                    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
        /* Either we won the CAS or someone else did; either way prod_chunk
         * now points at a successor and the loop retries against it. */
    }

    atomic_fetch_add_u64(&c->overflow_count, 1);
}

/* ==========================================================================
 *  Consumer — irq_defer_pump()
 *
 *  MPMC drain. Any core may call this concurrently with any other call,
 *  including pumps targeting the same ring.
 *
 *  Hazard-pointer protocol:
 *    1. Read cons_chunk into local ch.
 *    2. Publish ch into g_pump_hazards[my_core].chunk (RELEASE).
 *    3. Re-load cons_chunk; if it advanced, retry from (1).
 *    Otherwise: ch is now hazard-protected — reclaim cannot free it
 *    while my hazard points to it.
 *
 *  Slot-claim protocol:
 *    Atomic CAS(cons_idx, i, i+1) wins ownership of slot i. A consumer
 *    that observes ready=0 at slot i (producer hasn't published yet) does
 *    NOT advance cons_idx — it returns from pump (the slot will be visible
 *    on a future pump call once the producer publishes).
 * ========================================================================== */

typedef enum {
    SLOT_GOT,
    SLOT_NOT_READY,
    SLOT_CHUNK_FULL,
} slot_state_t;

static slot_state_t claim_slot(IrqDeferChunk *ch, uint32_t *out_idx)
{
    for (;;) {
        uint32_t i = atomic_load_u32(&ch->cons_idx);
        if (i >= ch->capacity) return SLOT_CHUNK_FULL;

        uint32_t r = __atomic_load_n(&ch->slots[i].ready, __ATOMIC_ACQUIRE);
        if (!r) return SLOT_NOT_READY;

        if (atomic_cas_u32(&ch->cons_idx, i, i + 1)) {
            *out_idx = i;
            return SLOT_GOT;
        }
        /* CAS lost — another consumer claimed slot i. Re-read and retry. */
    }
}

/* Returns true iff cons_chunk was successfully advanced past ch.
 * On true return, *out_retired = ch (caller is responsible for retire).
 * On false: either producer still on this chunk, or no successor yet,
 * or another consumer won the CAS race. */
static bool try_advance_cons_chunk(IrqDeferCore *c, IrqDeferChunk *ch,
                                   IrqDeferChunk **out_retired)
{
    IrqDeferChunk *cur_prod = (IrqDeferChunk *)__atomic_load_n(
        (void *volatile *)&c->prod_chunk, __ATOMIC_ACQUIRE);
    if (cur_prod == ch) return false;

    IrqDeferChunk *next = (IrqDeferChunk *)__atomic_load_n(
        (void *volatile *)&ch->next, __ATOMIC_ACQUIRE);
    if (!next) return false;

    void *expected = (void *)ch;
    if (__atomic_compare_exchange_n((void *volatile *)&c->cons_chunk,
                                    &expected, next, false,
                                    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        *out_retired = ch;
        return true;
    }
    atomic_fetch_add_u64(&c->advance_failures, 1);
    return false;
}

/* Publish the hazard pointer with the standard re-check pattern. Returns
 * the chunk pointer the hazard now protects, or NULL if cons_chunk is
 * empty. */
static IrqDeferChunk *acquire_hazard(IrqDeferCore *c,
                                     volatile struct IrqDeferChunk **hazard)
{
    for (;;) {
        IrqDeferChunk *ch = (IrqDeferChunk *)__atomic_load_n(
            (void *volatile *)&c->cons_chunk, __ATOMIC_ACQUIRE);
        if (!ch) {
            __atomic_store_n(hazard, NULL, __ATOMIC_RELEASE);
            return NULL;
        }
        __atomic_store_n(hazard, (volatile struct IrqDeferChunk *)ch,
                         __ATOMIC_RELEASE);

        IrqDeferChunk *recheck = (IrqDeferChunk *)__atomic_load_n(
            (void *volatile *)&c->cons_chunk, __ATOMIC_ACQUIRE);
        if (recheck == ch) return ch;
        /* cons_chunk advanced between load and publish — retry. */
    }
}

uint32_t irq_defer_pump(uint8_t core_idx)
{
    if (!__atomic_load_n(&g_irq_defer_ready, __ATOMIC_ACQUIRE)) return 0;
    if (core_idx >= g_amp.total_cores) return 0;

    IrqDeferCore *c = &g_irq_defer[core_idx];
    uint32_t      processed = 0;

    uint8_t my_core = amp_get_core_index();
    if (my_core >= g_amp.total_cores) my_core = g_amp.bsp_index;
    volatile struct IrqDeferChunk **hazard = &g_pump_hazards[my_core].chunk;

    for (;;) {
        IrqDeferChunk *ch = acquire_hazard(c, hazard);
        if (!ch) break;

        uint32_t i;
        slot_state_t state = claim_slot(ch, &i);

        if (state == SLOT_GOT) {
            /* We own slot i. Hazard still protects ch — both handler and
             * the consumed_count fetch_add stay safe. */
            void (*h)(void *) = ch->slots[i].handler;
            void *hctx        = ch->slots[i].ctx;

            h(hctx);

            processed++;
            atomic_fetch_add_u64(&c->processed_count, 1);
            atomic_fetch_add_u32(&ch->consumed_count, 1);
            continue;  /* try the next slot */
        }

        if (state == SLOT_NOT_READY) {
            /* Producer hasn't published this slot yet. Stop pumping — the
             * caller will retry on the next iteration. */
            break;
        }

        /* SLOT_CHUNK_FULL — try to advance cons_chunk past ch. */
        IrqDeferChunk *retired = NULL;
        bool advanced = try_advance_cons_chunk(c, ch, &retired);

        /* Drop hazard whether we advanced or not (advance path needs to
         * retire ch, which checks hazards). */
        __atomic_store_n(hazard, NULL, __ATOMIC_RELEASE);

        if (advanced && retired) {
            retired_list_push(c, retired);
            continue;  /* re-acquire hazard on the new cons_chunk */
        }
        if (!advanced) {
            /* Producer still on this chunk, or no successor — exit pump. */
            break;
        }
        /* advanced but no retire? unreachable, defensive */
    }

    /* Always release the hazard before tail-maintenance. */
    __atomic_store_n(hazard, NULL, __ATOMIC_RELEASE);

    reclaim_retired(c);
    refill_producer_chain(c);

    return processed;
}

/* ==========================================================================
 *  Telemetry
 * ========================================================================== */

uint64_t irq_defer_overflow(uint8_t core_idx)
{
    if (!g_irq_defer || core_idx >= g_amp.total_cores) return 0;
    return atomic_load_u64(&g_irq_defer[core_idx].overflow_count);
}

uint64_t irq_defer_chunks(uint8_t core_idx)
{
    if (!g_irq_defer || core_idx >= g_amp.total_cores) return 0;
    return atomic_load_u64(&g_irq_defer[core_idx].chunks_allocated);
}

uint64_t irq_defer_processed(uint8_t core_idx)
{
    if (!g_irq_defer || core_idx >= g_amp.total_cores) return 0;
    return atomic_load_u64(&g_irq_defer[core_idx].processed_count);
}

uint32_t irq_defer_pending(uint8_t core_idx)
{
    if (!g_irq_defer || core_idx >= g_amp.total_cores) return 0;
    IrqDeferCore *c = &g_irq_defer[core_idx];
    IrqDeferChunk *cch = (IrqDeferChunk *)__atomic_load_n(
        (void *volatile *)&c->cons_chunk, __ATOMIC_ACQUIRE);
    if (!cch) return 0;
    uint32_t prod = atomic_load_u32(&cch->prod_idx);
    if (prod > cch->capacity) prod = cch->capacity;
    uint32_t cons = atomic_load_u32(&cch->cons_idx);
    return prod > cons ? (prod - cons) : 0;
}
