#ifndef IRQ_DEFER_H
#define IRQ_DEFER_H

/* =========================================================================
 *  IrqDefer — elastic per-core deferred-work ring for IRQ bottom-halves
 * =========================================================================
 *
 * Problem this solves
 * -------------------
 * Several BoxOS IRQ handlers historically reached into kmalloc / pmm_free /
 * tagfs_close / process_snapshot_pids directly from interrupt context. That
 * violates the project's "Lock ordering → check deadlocks before any fix"
 * rule (CLAUDE.md) because the same locks (heap_lock, g_open_table_lock,
 * process_table_lock) are routinely held by non-IRQ kernel code on the
 * same core. If an IRQ fired while a thread on the same core held the
 * lock, the IRQ handler spun forever waiting for it — visible as random
 * hangs, random faults, "first command fails" races, and occasional
 * Touch garbage.
 *
 * The fix is the universal pattern: every IRQ handler that needs heap or
 * tagfs or process state defers the work to a K-Core context where those
 * locks may be taken safely. This file implements that defer mechanism.
 *
 * Design constraints (matching BoxOS philosophy: динамика, асинхронность,
 * уникальность, стабильность)
 * ---------------------------------------------------------------------
 *   - **Unbounded** — no compile-time slot cap. Storage adapts to load.
 *   - **Dynamic** — capacity grows in steps with the working set, shrinks
 *     (via free-list reuse) when load subsides.
 *   - **No hardcode** — initial chunk size and growth factor are
 *     configurable (kernel_config.h), not magic constants in code.
 *   - **IRQ-safe** — producer path is allocation-free, lock-free; uses
 *     only atomics and pre-existing slots. NO kmalloc/free/spinlock_t.
 *   - **Production failure mode** — when the producer cannot find a slot
 *     (extreme burst beyond any pre-allocated chunk plus its successors),
 *     it drops the event and increments a counter. K-Core observes the
 *     counter via telemetry; never panics. Drop is graceful because the
 *     only callers that defer are AHCI completion (process times out and
 *     retries) and Touch publishing (event delivery is best-effort by
 *     design).
 *
 * Structure
 * ---------
 *   Per core:
 *     prod_chunk -> [chunk_n]<- IRQ writes via fetch_add(prod_idx)
 *                       |
 *                       | when full, IRQ CAS-advances prod_chunk
 *                       v
 *                   [chunk_{n+1}]   <- pre-allocated by K-Core pump
 *
 *   Each chunk:
 *     [slot 0][slot 1]...[slot capacity-1]    <- IrqDeferSlot, ready flag
 *
 *   Consumer (K-Core) walks slots in order, advances to next chunk when
 *   current is fully drained AND producer has moved past it. Drained
 *   chunks return to a per-core free-list (LIFO), reused for the next
 *   pre-allocation; growth only happens when free-list is empty.
 *
 * SPSC / MPSC properties
 * ----------------------
 * Producer = IRQ on the owning core (single — IRQs on a given core are
 * serialised by the LAPIC). Consumer = whichever K-Core pumps this core's
 * ring (single — a ring is pumped by exactly one K-Core: by default the
 * core itself if it's a K-Core, else by an assigned partner K-Core).
 *
 * Memory model
 * ------------
 *   - Producer publishes slot fields with __ATOMIC_RELEASE on `ready`.
 *   - Consumer observes slot fields with __ATOMIC_ACQUIRE on `ready`.
 *   - Chunk-advance writes use ACQ_REL CAS so cross-core observers see
 *     a consistent (chunk-pointer, slots-init) view.
 */

#include "ktypes.h"

/* One pending bottom-half. handler must be safe to invoke from K-Core
 * context (may kmalloc / take tagfs / process locks). ctx semantics are
 * caller-defined — typically a persistent pointer (WriteJob, event struct)
 * or NULL for fire-and-forget. */
typedef struct IrqDeferSlot {
    void (*handler)(void *ctx);
    void *ctx;
    /* 0 = empty / being written; 1 = published. Single-bit ABA-free
     * because each slot is written once per chunk lifetime (we never
     * rewrite a slot — we move to a fresh chunk instead). */
    volatile uint32_t ready;
} IrqDeferSlot;

/* Flexible array — slots are appended in memory immediately after the
 * header so a single kmalloc allocates the whole chunk. */
typedef struct IrqDeferChunk {
    volatile uint32_t            prod_idx;    /* IRQ producer claim */
    volatile struct IrqDeferChunk *next;       /* chain to next chunk */
    uint32_t                      capacity;    /* slot count */
    IrqDeferSlot                  slots[];
} IrqDeferChunk;

/* Per-core ring control block. */
typedef struct IrqDeferCore {
    volatile struct IrqDeferChunk *prod_chunk;     /* IRQ producer side */
    struct IrqDeferChunk          *cons_chunk;     /* K-Core consumer side */
    uint32_t                       cons_idx;       /* index within cons_chunk */

    /* LIFO of recycled chunks ready for reuse — populated by consumer
     * when a chunk is fully drained, drained by pump when refilling
     * the producer's chain. Use atomic CAS on this head pointer. */
    volatile struct IrqDeferChunk *free_head;

    /* Telemetry — purely informational, never load-bearing. */
    volatile uint64_t overflow_count;     /* IRQ couldn't get a slot */
    volatile uint64_t chunks_allocated;   /* fresh kmalloc events */
    volatile uint64_t chunks_recycled;    /* served from free_head */
    volatile uint64_t processed_count;    /* slots handled by pump */
} IrqDeferCore;

/* g_irq_defer is sized to g_amp.total_cores at irq_defer_init time. */
extern IrqDeferCore *g_irq_defer;

/* Set to 1 once the rings are usable. IRQs that fire before this
 * silently drop (incrementing g_irq_defer_predropped) rather than touch
 * uninitialised memory. */
extern volatile uint8_t g_irq_defer_ready;
extern volatile uint64_t g_irq_defer_predropped;

/* Initialise per-core rings. Must run after amp_init() (g_amp.total_cores
 * is consulted) and before any IRQ handler that defers may fire. */
void irq_defer_init(void);

/* IRQ producer entry point. Safe to call from IRQ context. handler will
 * eventually run in some K-Core's pump loop with full kernel privileges
 * and the right to take any kernel lock that K-Core code normally takes.
 * Returns silently on overflow (counter incremented). */
void irq_defer(void (*handler)(void *), void *ctx);

/* Consumer drain — call from K-Core guide loop. Drains all ready slots
 * for `core_idx` and refills the producer's chain headroom. Returns the
 * number of handlers invoked. */
uint32_t irq_defer_pump(uint8_t core_idx);

/* Telemetry accessors — used by bench / diagnostic commands. */
uint64_t irq_defer_overflow(uint8_t core_idx);
uint64_t irq_defer_chunks(uint8_t core_idx);
uint64_t irq_defer_processed(uint8_t core_idx);
uint32_t irq_defer_pending(uint8_t core_idx);

#endif /* IRQ_DEFER_H */
