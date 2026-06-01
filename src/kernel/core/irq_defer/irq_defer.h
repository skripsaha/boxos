#ifndef IRQ_DEFER_H
#define IRQ_DEFER_H

/* =========================================================================
 *  IrqDefer — MPMC per-core deferred-work ring for IRQ bottom-halves
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
 * locks may be taken safely.
 *
 * MPMC: any core can pump any ring
 * --------------------------------
 * Legacy IO-APIC routing pins certain IRQs (BMIDE GSI 14, SCI, ...) to a
 * single CPU — typically the BSP. The deferred handler then lives on that
 * one core's ring, and only that core can drain it. If the BSP is stuck
 * spinning on a lock held by a different K-Core that is itself waiting on
 * an IRQ completion deferred to the BSP's ring, the system deadlocks.
 *
 * To break that class of bug, the ring is MPMC: producer side stays SP
 * (each IRQ writes its own core's ring), but the consumer side is multi-
 * consumer safe via CAS-claim on `cons_idx` + hazard-pointer-protected
 * chunk reclamation. Any pumping core (typically a K-Core stalled in a
 * sync I/O wait loop) may drain any core's ring without corrupting state.
 *
 * Why no producer hazard? Two structural invariants — together — make MP
 * producer-side hazards redundant:
 *
 *   (1) "Write-slot" path (fetch_add returned idx < capacity):
 *       cons_chunk can advance past chunk X only after cons_idx hits
 *       capacity. cons_idx advances only by claiming a slot with ready=1.
 *       ready=1 is set only after the owning producer has written
 *       handler/ctx and done its release store. Hence cons_idx == capacity
 *       implies every claimed slot has been fully published — no producer
 *       can be mid-write in X by the time X is eligible for retirement.
 *
 *   (2) "Advance" path (fetch_add returned idx >= capacity):
 *       The producer reads ch->next and CAS-advances prod_chunk. X cannot
 *       be retired during this window because try_advance_cons_chunk()
 *       checks `cur_prod != ch` and refuses to advance cons_chunk while
 *       prod_chunk still points at ch. cur_prod transitions away from ch
 *       only when SOME producer's CAS-advance succeeds — by which point
 *       that producer has finished accessing ch entirely (the CAS is its
 *       last touch). Consequently a producer mid-advance always sees an
 *       intact, non-reset ch.
 *
 * Both invariants hold for any number of concurrent producers — current
 * deployment is SP-per-ring (per-core IRQ serialisation), but the design
 * is correct under arbitrary MP-per-ring (cross-core notification, future
 * RT extensions) as long as producers run to publication once they have
 * fetch_add'd. Kernel IRQ context guarantees the latter (handlers run to
 * completion without preemption). A user-space port where producers may
 * be preempted between fetch_add and the ready store would be the one
 * scenario where producer-side hazard pointers (with the SEQ_CST publish-
 * then-recheck mfence) would become necessary — see commit history's
 * audit notes for the implementation sketch.
 *
 * Design constraints (matching BoxOS philosophy: динамика, асинхронность,
 * уникальность, стабильность)
 * ---------------------------------------------------------------------
 *   - **Unbounded** — no compile-time slot cap. Storage adapts to load via
 *     chunk chaining; chunk capacity grows in doubling steps.
 *   - **Dynamic** — drained chunks return to a per-core free-list; growth
 *     stops once steady-state working set is satisfied.
 *   - **No hardcode** — initial chunk size and growth factor live in
 *     kernel_config.h, not magic constants in code.
 *   - **IRQ-safe producer** — `irq_defer()` is allocation-free, lock-free,
 *     uses only atomics + pre-existing slots. Safe from any IRQ context.
 *   - **MPMC consumer** — `irq_defer_pump(core_idx)` may be called from
 *     ANY core concurrently with any other call. Slots are CAS-claimed;
 *     handlers run exactly once.
 *   - **Memory-safe chunk reclaim** — hazard pointers per pumping core
 *     guarantee no consumer dereferences a freed chunk. Retirement is
 *     two-stage: advance cons_chunk → push to retired list → reclaim
 *     when all hazards have moved on AND all in-chunk handlers returned.
 *   - **Production failure mode** — when the producer cannot find a slot
 *     (extreme burst beyond pre-allocated chain length), the event is
 *     dropped and counted. Never panics. Best-effort callers (Touch
 *     publishing, AHCI completion-with-retry) tolerate this gracefully.
 *
 * Memory model
 * ------------
 *   - Producer publishes slot via ACQ_REL fetch_add on `prod_idx`, then
 *     ordinary stores, then RELEASE store on `ready=1`.
 *   - Consumer load-acquires `ready`, ACQ_REL CAS on `cons_idx` to claim,
 *     reads slot fields, runs handler, fetch_add `consumed_count`.
 *   - Chunk-pointer migrations use ACQ_REL CAS.
 *   - Hazard pointer publish is RELEASE store; checker uses ACQUIRE load.
 *
 * Lifecycle of a chunk
 * --------------------
 *   1. chunk_alloc — kmalloc + memset(0). slot.ready=0 means "untouched".
 *   2. Producer fills slots 0..capacity-1 (in order, single producer).
 *   3. Multiple consumers race CAS-claim of cons_idx, each runs handler.
 *   4. Final consumer (cons_idx hits capacity, prod_chunk has moved past)
 *      CAS-advances cons_chunk to ch->next and pushes ch to retired list.
 *   5. reclaim_retired walks the list; for each retired ch checks
 *      (consumed_count == capacity) AND (no hazard points to ch); if both
 *      hold, chunk_reset + push to free_list. Otherwise requeue.
 *   6. refill_producer_chain pops from free_list (or kmallocs) and
 *      CAS-publishes as the producer's next chunk.
 */

#include "ktypes.h"
#include "klib.h"
#include "kernel_config.h"

/* One pending bottom-half. handler must be safe to invoke from K-Core
 * context (may kmalloc / take tagfs / process locks). ctx semantics are
 * caller-defined — typically a persistent pointer (WriteJob, event struct)
 * or NULL for fire-and-forget. */
typedef struct IrqDeferSlot {
    void (*handler)(void *ctx);
    void *ctx;
    /* 0 = empty / being written; 1 = published. ACQ_REL ordering ties
     * handler/ctx writes to the ready store. */
    volatile uint32_t ready;
} IrqDeferSlot;

/* Flexible array — slots are appended in memory immediately after the
 * header so a single kmalloc allocates the whole chunk.
 *
 * Cacheline layout: the producer cursor (prod_idx) and the consumer
 * cursors (cons_idx, consumed_count) sit on SEPARATE cachelines so the
 * producer's atomic fetch_add never invalidates the consumer's CAS line
 * and vice-versa. This is the io_uring / Disruptor / rigtorp pattern —
 * skipping it leaves visible "cache-line bouncing" tail latencies under
 * cross-core production, which the MPMC pump explicitly enables. */
typedef struct IrqDeferChunk {
    volatile uint32_t            prod_idx;        /* producer claim cursor */
    char _pad_prod[CONFIG_CACHE_LINE_SIZE - sizeof(uint32_t)];

    volatile uint32_t            cons_idx;        /* consumer CAS claim cursor */
    volatile uint32_t            consumed_count;  /* handlers actually returned */
    char _pad_cons[CONFIG_CACHE_LINE_SIZE - 2 * sizeof(uint32_t)];

    volatile struct IrqDeferChunk *next;          /* set once at chain extension */
    uint32_t                      capacity;       /* immutable after init */
    IrqDeferSlot                  slots[];
} IrqDeferChunk;

/* Per-core ring control block. */
typedef struct IrqDeferCore {
    volatile struct IrqDeferChunk *prod_chunk;     /* producer side */
    volatile struct IrqDeferChunk *cons_chunk;     /* consumer side */

    /* free_list (recycle) + retired_list (pending reclaim) — protected by
     * free_lock to avoid ABA on raw CAS-stacks. Brief contention only on
     * the pump's tail (cold path). */
    spinlock_t                     free_lock;
    struct IrqDeferChunk          *free_head;
    struct IrqDeferChunk          *retired_head;

    /* Telemetry — never load-bearing. */
    volatile uint64_t overflow_count;
    volatile uint64_t chunks_allocated;
    volatile uint64_t chunks_recycled;
    volatile uint64_t processed_count;
    volatile uint64_t advance_failures;  /* CAS lost on cons_chunk advance */
} IrqDeferCore;

/* One hazard slot per pumping core. A pumper publishes the cons_chunk it
 * is currently traversing here; reclaim refuses to free a chunk while
 * any hazard still references it. Cacheline-padded to avoid false-sharing
 * between cores during the per-pump publish-and-clear traffic. */
typedef struct CoreHazard {
    volatile struct IrqDeferChunk *chunk;
    char _pad[CONFIG_CACHE_LINE_SIZE - sizeof(volatile struct IrqDeferChunk *)];
} CoreHazard;

/* Sized to g_amp.total_cores at irq_defer_init time. */
extern IrqDeferCore *g_irq_defer;
extern CoreHazard   *g_pump_hazards;

/* Set to 1 once the rings are usable. IRQs that fire before this
 * silently drop (incrementing g_irq_defer_predropped) rather than touch
 * uninitialised memory. */
extern volatile uint8_t  g_irq_defer_ready;
extern volatile uint64_t g_irq_defer_predropped;

/* Initialise per-core rings + hazard table. Must run after amp_init() and
 * before any IRQ handler that defers may fire. */
void irq_defer_init(void);

/* IRQ producer entry point. Safe to call from IRQ context. handler will
 * eventually run in some K-Core's pump loop with full kernel privileges
 * and the right to take any kernel lock that K-Core code normally takes.
 * Returns silently on overflow (counter incremented). */
void irq_defer(void (*handler)(void *), void *ctx);

/* Consumer drain — may be called from ANY core concurrently with any
 * other call. Drains as many ready slots as possible for `core_idx` and
 * runs reclaim/refill maintenance on the tail. Returns the number of
 * handlers invoked. */
uint32_t irq_defer_pump(uint8_t core_idx);

/* Telemetry accessors — used by bench / diagnostic commands. */
uint64_t irq_defer_overflow(uint8_t core_idx);
uint64_t irq_defer_chunks(uint8_t core_idx);
uint64_t irq_defer_processed(uint8_t core_idx);
uint32_t irq_defer_pending(uint8_t core_idx);

#endif /* IRQ_DEFER_H */
