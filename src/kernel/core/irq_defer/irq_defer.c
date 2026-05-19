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

IrqDeferCore     *g_irq_defer        = NULL;
volatile uint8_t  g_irq_defer_ready  = 0;
volatile uint64_t g_irq_defer_predropped = 0;

static IrqDeferChunk *chunk_alloc(uint32_t capacity)
{
    size_t bytes = sizeof(IrqDeferChunk) + (size_t)capacity * sizeof(IrqDeferSlot);
    IrqDeferChunk *ch = (IrqDeferChunk *)kmalloc(bytes);
    if (!ch) return NULL;
    /* Zero everything — slot.ready=0 is the "empty" sentinel the
     * consumer relies on for the not-ready check. */
    memset(ch, 0, bytes);
    ch->capacity = capacity;
    return ch;
}

/* Atomic LIFO push of a chunk onto the per-core free list. Safe to call
 * from K-Core only (consumer side). */
static void free_list_push(IrqDeferCore *c, IrqDeferChunk *ch)
{
    /* Reset chunk state so a subsequent producer sees a clean slate.
     * prod_idx must be cleared; next must be NULL; slots must all show
     * ready=0. Slots are already left at ready=0 because we never
     * "unset" ready after processing — the consumer simply walked past
     * them and the chunk is fully drained. To be defensive, reset
     * slots here too. */
    for (uint32_t i = 0; i < ch->capacity; i++) {
        ch->slots[i].ready = 0;
    }
    atomic_store_u32(&ch->prod_idx, 0);

    /* CAS-link onto free_head. */
    IrqDeferChunk *head;
    do {
        head = (IrqDeferChunk *)c->free_head;
        ch->next = (volatile struct IrqDeferChunk *)head;
    } while (!atomic_cas_u64((volatile uint64_t *)&c->free_head,
                             (uint64_t)head, (uint64_t)ch));
    atomic_fetch_add_u64(&c->chunks_recycled, 1);
}

/* Atomic LIFO pop from the per-core free list. Returns NULL when empty.
 * Safe to call from K-Core only (producer side of the free list is
 * also the consumer of deferred slots, so only one thread pops). */
static IrqDeferChunk *free_list_pop(IrqDeferCore *c)
{
    IrqDeferChunk *head;
    IrqDeferChunk *new_head;
    do {
        head = (IrqDeferChunk *)c->free_head;
        if (!head) return NULL;
        new_head = (IrqDeferChunk *)head->next;
    } while (!atomic_cas_u64((volatile uint64_t *)&c->free_head,
                             (uint64_t)head, (uint64_t)new_head));
    head->next = NULL;
    return head;
}

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

    for (uint32_t i = 0; i < n; i++) {
        IrqDeferChunk *initial = chunk_alloc(CONFIG_IRQ_DEFER_INITIAL_CAPACITY);
        if (!initial) {
            kprintf("[IRQ_DEFER] FATAL: cannot allocate initial chunk for core %u\n", i);
            while (1) { __asm__ volatile("cli; hlt"); }
        }
        g_irq_defer[i].prod_chunk = (volatile struct IrqDeferChunk *)initial;
        g_irq_defer[i].cons_chunk = initial;
        g_irq_defer[i].cons_idx   = 0;
        g_irq_defer[i].free_head  = NULL;
        atomic_store_u64(&g_irq_defer[i].chunks_allocated, 1);
    }

    /* Release-store the ready flag so any core that subsequently
     * defers from IRQ sees the fully-initialised ring array. */
    __atomic_store_n(&g_irq_defer_ready, 1, __ATOMIC_RELEASE);

    debug_printf("[IRQ_DEFER] %u per-core ring(s) ready, initial capacity %u, max chunk %u\n",
                 n, (uint32_t)CONFIG_IRQ_DEFER_INITIAL_CAPACITY,
                 (uint32_t)CONFIG_IRQ_DEFER_MAX_CHUNK_CAPACITY);
}

void irq_defer(void (*handler)(void *), void *ctx)
{
    /* Pre-init defers are silently dropped — counter lets us notice. */
    if (!__atomic_load_n(&g_irq_defer_ready, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&g_irq_defer_predropped, 1, __ATOMIC_RELAXED);
        return;
    }
    if (!handler) return;

    uint8_t core = amp_get_core_index();
    if (core >= g_amp.total_cores) {
        /* Defensive — should never happen but if amp state is half-init,
         * route to BSP. BSP K-Core will pump it. */
        core = g_amp.bsp_index;
    }
    IrqDeferCore *c = &g_irq_defer[core];

    /* Bounded loop: retry only enough times to chase legitimate chunk
     * advances. 4 iterations is plenty (chain length grows in doubling
     * steps; a single producer never sees more than ~log2 advances). */
    for (int attempt = 0; attempt < 4; attempt++) {
        IrqDeferChunk *ch = (IrqDeferChunk *)__atomic_load_n(
            (void *volatile *)&c->prod_chunk, __ATOMIC_ACQUIRE);

        uint32_t idx = atomic_fetch_add_u32(&ch->prod_idx, 1);
        if (idx < ch->capacity) {
            ch->slots[idx].handler = handler;
            ch->slots[idx].ctx     = ctx;
            /* Release publishes handler/ctx writes before ready bit. */
            __atomic_store_n(&ch->slots[idx].ready, 1u, __ATOMIC_RELEASE);
            return;
        }

        /* Chunk full. Try to advance to the next pre-allocated chunk. */
        IrqDeferChunk *next = (IrqDeferChunk *)__atomic_load_n(
            (void *volatile *)&ch->next, __ATOMIC_ACQUIRE);
        if (!next) {
            /* K-Core hasn't refilled headroom yet. Drop and tally. */
            atomic_fetch_add_u64(&c->overflow_count, 1);
            return;
        }

        /* CAS-advance the producer chunk. If we lose, someone else
         * already did it — retry against whatever they installed. */
        void *expected = (void *)ch;
        __atomic_compare_exchange_n((void *volatile *)&c->prod_chunk,
                                    &expected, next, false,
                                    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
        /* Loop continues regardless of CAS outcome — c->prod_chunk now
         * points at someone's successor. */
    }

    /* Gave up after retries — should only happen under pathological
     * concurrent chunk-advance pressure. Drop. */
    atomic_fetch_add_u64(&c->overflow_count, 1);
}

uint32_t irq_defer_pump(uint8_t core_idx)
{
    if (!__atomic_load_n(&g_irq_defer_ready, __ATOMIC_ACQUIRE)) return 0;
    if (core_idx >= g_amp.total_cores) return 0;

    IrqDeferCore *c = &g_irq_defer[core_idx];
    uint32_t      processed = 0;

    for (;;) {
        IrqDeferChunk *cch = c->cons_chunk;
        if (!cch) break;

        /* Drain ready slots in order. We use cons_idx as a private
         * cursor — no atomics needed (single consumer per ring). */
        while (c->cons_idx < cch->capacity) {
            uint32_t r = __atomic_load_n(&cch->slots[c->cons_idx].ready,
                                         __ATOMIC_ACQUIRE);
            if (!r) {
                /* Producer hasn't published this slot yet. We're done
                 * with this chunk for now (will retry on next pump). */
                goto check_advance;
            }
            void (*h)(void *) = cch->slots[c->cons_idx].handler;
            void *hctx        = cch->slots[c->cons_idx].ctx;
            c->cons_idx++;
            processed++;
            atomic_fetch_add_u64(&c->processed_count, 1);

            /* Invoke OUTSIDE any ring-internal critical section — handler
             * may itself call irq_defer (reentrant-safe: producer adds
             * to whatever chunk is current; no recursion through the
             * pump path). */
            h(hctx);
        }

check_advance:
        /* We've drained all that we could from this chunk. If the
         * producer has moved past it (cur_prod != cch), the chunk is
         * fully retired — recycle and advance. */
        {
            IrqDeferChunk *cur_prod = (IrqDeferChunk *)__atomic_load_n(
                (void *volatile *)&c->prod_chunk, __ATOMIC_ACQUIRE);
            IrqDeferChunk *next = (IrqDeferChunk *)__atomic_load_n(
                (void *volatile *)&cch->next, __ATOMIC_ACQUIRE);
            if (cur_prod != cch && next && c->cons_idx >= cch->capacity) {
                c->cons_chunk = next;
                c->cons_idx   = 0;
                /* Recycle the just-drained chunk for the producer's
                 * next refill request. */
                free_list_push(c, cch);
                continue;
            }
        }
        break;  /* nothing more drainable this pass */
    }

    /* Refill: ensure the producer chunk has a successor. Allocate from
     * the free list first; fall back to kmalloc only if recycling
     * couldn't keep up. */
    {
        IrqDeferChunk *prod = (IrqDeferChunk *)__atomic_load_n(
            (void *volatile *)&c->prod_chunk, __ATOMIC_ACQUIRE);
        IrqDeferChunk *prod_next = (IrqDeferChunk *)__atomic_load_n(
            (void *volatile *)&prod->next, __ATOMIC_ACQUIRE);
        if (!prod_next) {
            IrqDeferChunk *fresh = free_list_pop(c);
            if (!fresh) {
                /* Adaptive growth — double capacity up to the per-chunk
                 * ceiling, then stay constant. The chain grows in
                 * length, not in per-chunk size, once the ceiling
                 * is reached. */
                uint32_t new_cap = prod->capacity * 2u;
                if (new_cap > (uint32_t)CONFIG_IRQ_DEFER_MAX_CHUNK_CAPACITY)
                    new_cap = (uint32_t)CONFIG_IRQ_DEFER_MAX_CHUNK_CAPACITY;
                if (new_cap < prod->capacity)
                    new_cap = prod->capacity;  /* defensive on overflow */
                fresh = chunk_alloc(new_cap);
                if (fresh) {
                    atomic_fetch_add_u64(&c->chunks_allocated, 1);
                }
            }
            if (fresh) {
                /* Publish via CAS — if another pump beat us (impossible
                 * today with single consumer but defensive), drop ours. */
                void *expected = NULL;
                if (!__atomic_compare_exchange_n((void *volatile *)&prod->next,
                                                 &expected, fresh, false,
                                                 __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
                    free_list_push(c, fresh);
                }
            }
        }
    }

    return processed;
}

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
    IrqDeferChunk *cch = c->cons_chunk;
    if (!cch) return 0;
    uint32_t pidx = atomic_load_u32(&cch->prod_idx);
    if (pidx > cch->capacity) pidx = cch->capacity;
    return pidx > c->cons_idx ? (pidx - c->cons_idx) : 0;
}
