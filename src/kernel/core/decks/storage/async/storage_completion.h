#ifndef STORAGE_COMPLETION_H
#define STORAGE_COMPLETION_H

/* =========================================================================
 *  StorageCompletion — never-drop intrusive MPSC for storage async
 *  completions
 * =========================================================================
 *
 * Why this exists (the drop that irq_defer could not avoid)
 * ---------------------------------------------------------
 * Async AHCI storage completions (an ObjRead block landing, a WriteJob
 * state-machine step returning from an IRQ) used to be handed to a K-Core
 * pump through irq_defer. irq_defer is allocation-free on the producer
 * side, but it is NOT never-drop: when a completion burst outruns the
 * pump's chunk refill — or when that refill's kmalloc fails under memory
 * pressure — the producer finds no pre-allocated slot and DROPS the event
 * (overflow_count++). A dropped storage completion is fatal, not
 * best-effort: the caller parked on PROC_WAITING (or a box::ferry
 * co_await) is woken only by the completion's KResultPush, so a dropped
 * completion hangs that caller FOREVER and leaks the AHCI command slot.
 *
 * The M1 watchdog (ahci_watchdog_scan) recovers a lost *hardware* MSI, but
 * it cannot recover a dropped *software* defer — the hardware completed and
 * the IRQ fired; we simply failed to enqueue the continuation. The naive
 * fix (a registry that re-enqueues stuck jobs) is use-after-free prone
 * (a duplicate continuation races the still-live original into its free).
 *
 * The structural fix: embed the queue node inside the job. A push then
 * needs no allocation and no free slot — the node already lives in the
 * WriteJob / ObjReadAsyncCtx / ahci_port_t — so a completion can NEVER be
 * dropped for want of a slot. Drop is not made rare; it is made
 * impossible.
 *
 * Single-consumer, and why that is deadlock-free here
 * ---------------------------------------------------
 * This is a Vyukov intrusive MPSC with a persistent stub: multi-producer
 * (any core's IRQ or pump may push), single-consumer (each per-core queue
 * is drained ONLY by its owning core, from kcore_run_loop). A single
 * consumer is safe here — where irq_defer needed a multi-consumer drain —
 * because of one invariant:
 *
 *   INVARIANT P: every waiter for a storage async completion PARKS
 *   (PROC_WAITING, woken by KResultPush) and holds no spinlock across the
 *   wait. It never spins.
 *
 * irq_defer must stay multi-consumer because one of its payloads (the
 * BMIDE completion, ata_complete_deferred) has a spinning, lock-holding
 * waiter: if the BSP could not drain that from a foreign core, the classic
 * cycle forms (BSP spins on a lock held by a K-Core that is itself waiting
 * on a completion deferred to the BSP's ring). Storage completions are the
 * opposite class — their waiters park — so peeling them onto a
 * single-consumer never-drop queue introduces no cycle: nothing's progress
 * depends on the storage pump except the parked waiters, and they hold no
 * resource pending it. The spin-class payload never moves, so the deadlock
 * irq_defer's MPMC consumer prevents is not reintroduced.
 *
 * All storage completions route to the drain core (the BSP — the AHCI MSI
 * owner). Per-core queues keep the consumer trivially single (each core
 * pumps only its own), so a same-uniform StorageCompletionPump(my_idx) in
 * every K-Core needs no "am I the BSP" special case; non-BSP queues stay
 * empty and their pump is a one-load early-out.
 *
 * Memory model (x86-TSO)
 * ----------------------
 *   - Producer: write run/ctx (once, at job init, before any push), then
 *     n->next=NULL (relaxed), then LOCK XCHG on tail (full fence), then a
 *     RELEASE store publishing prev->next=n.
 *   - Consumer: ACQUIRE load of head->next; it synchronises-with the
 *     producer's RELEASE, making run/ctx and the stashed if_status visible.
 *   - The one Vyukov window — producer past its XCHG but before its
 *     prev->next store — makes pop transiently observe "empty". That node
 *     is NOT lost (it already owns tail); the next pump returns it. Same-
 *     core producers (the BSP's own MSI) cannot be mid-push during the
 *     BSP's HLT gate (CLI serialises IRQ handlers, which run to
 *     completion); cross-core producers (an App-Core initial-kick handoff)
 *     send IPI_WAKE, so even the transient window cannot lose a wakeup.
 */

#include "ktypes.h"
#include "kernel_config.h"
#include "error.h"

/* Intrusive MPSC node, embedded BY VALUE in every job that can post a
 * storage async continuation. run+ctx are stored explicitly (not via
 * container_of) so the consumer is type-blind across the heterogeneous
 * containers (WriteJob, ObjReadAsyncCtx, ahci_port_t). Set once at job
 * init and reused across every enqueue of that job; `next` is owned by the
 * queue. */
typedef struct StorageCompletion StorageCompletion;
struct StorageCompletion {
    StorageCompletion *volatile next;  /* MPSC chain; NULL at tail (atomic)   */
    void (*run)(void *ctx);            /* K-Core continuation                 */
    void  *ctx;                        /* argument — the container job        */
};

/* Per-core single-consumer queue. Producer end (`tail`) is XCHG'd by any
 * core; consumer end (`head`) is touched ONLY by the owning core. The
 * persistent `stub` keeps head/tail non-NULL so neither push nor pop ever
 * special-cases an empty queue. Producer and consumer cursors sit on
 * separate cachelines (io_uring / Disruptor pattern) so a producer XCHG
 * never invalidates the consumer's line. */
typedef struct StorageCompletionQueue {
    StorageCompletion *volatile tail;  /* producer XCHG cursor (atomic)       */
    char _pad_tail[CONFIG_CACHE_LINE_SIZE - sizeof(StorageCompletion *)];

    StorageCompletion *head;           /* consumer cursor — single-consumer   */
    char _pad_head[CONFIG_CACHE_LINE_SIZE - sizeof(StorageCompletion *)];

    StorageCompletion  stub;           /* dummy anchor, never delivered       */

    /* Telemetry only — never load-bearing. The stub re-anchor is NOT
     * counted, so at quiescence pushed == popped == real completions. */
    volatile uint64_t  pushed;
    volatile uint64_t  popped;
} StorageCompletionQueue;

/* Sized to g_amp.total_cores at init. */
extern StorageCompletionQueue *g_storage_cq;
extern volatile uint8_t        g_storage_cq_ready;

/* Allocate + init the per-core queues. Run after amp_init() and before any
 * AHCI IRQ that completes async I/O may fire (wired right after
 * irq_defer_init in main.c). */
void StorageCompletionInit(void);

/* Producer. IRQ-safe, allocation-free, NEVER drops (the node is embedded
 * in the caller's job). Routes the node to the drain core's queue and, if
 * called from another core, sends IPI_WAKE so a HLT'd drainer wakes. run
 * and ctx must already be set in `n`. */
void StorageCompletionPush(StorageCompletion *n);

/* Consumer. Drains every ready node on `core_idx`'s queue, running each
 * continuation. CONTRACT: single-consumer — call ONLY with the calling
 * core's own index, ONLY from kcore_run_loop. Returns the number run. */
uint32_t StorageCompletionPump(uint8_t core_idx);

/* HLT-gate helper: true iff `core_idx`'s queue has a real (non-stub) node
 * awaiting drain. Must be called by the owning core (it reads `head`).
 * Structural — no counter — so it never drifts. */
bool StorageCompletionPending(uint8_t core_idx);

/* Halt-drain helper: true iff any completion posted to `core_idx`'s queue
 * has not yet been consumed (pushed != popped). Uses atomic counters, so —
 * unlike StorageCompletionPending, which reads the SC-owned head — it is
 * safe to call from a core that does NOT own the queue (system_halt runs on
 * an arbitrary core while the drain core is still pumping). */
bool StorageCompletionOutstanding(uint8_t core_idx);

/* Deterministic boot self-test of the MPSC primitive (never-drop, exactly-
 * once, per-producer FIFO, stub re-anchor). Drives a private queue on the
 * calling core; does not touch the live per-core queues. Returns OK on pass.
 * Emits "[SCQ-TEST] PASS" so the phase matrix can assert it. */
error_t StorageCompletionSelfTest(void);

#endif /* STORAGE_COMPLETION_H */
