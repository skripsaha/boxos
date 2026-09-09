#ifndef BATON_H
#define BATON_H

/* =========================================================================
 *  Baton — the never-drop hand-off from an interrupt to a K-Core
 * =========================================================================
 *
 * A baton is passed from hand to hand; it is never thrown into a basket that
 * may have no room. That is the whole of the difference from irq_defer: the
 * node that carries the continuation is EMBEDDED in the thing it is about —
 * a WriteJob, an ObjReadAsyncCtx, an xHCI transfer, a process's park
 * deadline — so a pass needs no allocation and no free slot, and cannot be
 * dropped for want of one. Drop is not made rare; it is made impossible.
 *
 * Why this exists (the drop that irq_defer could not avoid)
 * ---------------------------------------------------------
 * irq_defer is allocation-free on the producer side, but it is NOT
 * never-drop: when a burst outruns the pump's chunk refill — or when that
 * refill's kmalloc fails under memory pressure — the producer finds no
 * pre-allocated slot and DROPS the event (overflow_count++). For a Touch
 * that is best effort. For a storage completion it is fatal: the caller
 * parked on PROC_WAITING is woken only by the completion's KResultPush, so a
 * dropped completion hangs it FOREVER and leaks the command slot. For a park
 * deadline it is the same stall with a different name: the tick reschedules
 * the sleeper, but the ERR_TIMEOUT Result it is waiting for never comes, and
 * boxlib used to cover that with a +100 ms clock of its own — a guessed
 * number over a delivery the kernel had promised. The naive fix for either
 * (a registry that re-enqueues stuck jobs) is use-after-free prone (a
 * duplicate continuation races the still-live original into its free).
 *
 * Who rides it
 * ------------
 *   - storage async completions (AHCI, BMIDE, xHCI MSD): a block landed, a
 *     WriteJob step came back from an IRQ, a port recovery;
 *   - the deadline of a timed addr_park: the PIT tick passes the process's
 *     own baton (process_t.deadline_baton) and a K-Core delivers the
 *     ERR_TIMEOUT Result where KResultPush (cabin VMM) is safe.
 *
 * Single-consumer, and why that is deadlock-free here
 * ---------------------------------------------------
 * This is a Vyukov intrusive MPSC with a persistent stub: multi-producer
 * (any core's IRQ or pump may pass), single-consumer (each per-core queue
 * is drained ONLY by its owning core). A single consumer is safe here —
 * where irq_defer needed a multi-consumer drain — because of one invariant:
 *
 *   INVARIANT P: every waiter for a baton's continuation PARKS
 *   (PROC_WAITING, woken by KResultPush) and holds no spinlock across the
 *   wait. It never spins.
 *
 * irq_defer must stay multi-consumer because one of its payloads (the
 * BMIDE completion, ata_complete_deferred) has a spinning, lock-holding
 * waiter: if the BSP could not drain that from a foreign core, the classic
 * cycle forms (BSP spins on a lock held by a K-Core that is itself waiting
 * on a completion deferred to the BSP's ring). Baton riders are the
 * opposite class — their waiters park — so peeling them onto a
 * single-consumer never-drop queue introduces no cycle: nothing's progress
 * depends on the pump except the parked waiters, and they hold no
 * resource pending it. The spin-class payload never moves, so the deadlock
 * irq_defer's MPMC consumer prevents is not reintroduced.
 *
 * Every pass routes to the drain core (the BSP — the AHCI MSI owner and the
 * PIT's core). Per-core queues keep the consumer trivially single (each core
 * pumps only its own), so the uniform BatonPump(my_idx) in every K-Core
 * needs no "am I the BSP" special case; non-BSP queues stay empty and their
 * pump is a one-load early-out. On one core the BSP runs no K-Core loop: the
 * idle loop pumps, and so does the timer tick when it interrupted user mode
 * (the two never overlap — the tick pumps only from ring 3, and idle is
 * ring 0), exactly as irq_defer is drained there.
 *
 * Memory model (x86-TSO)
 * ----------------------
 *   - Producer: write run/ctx (once, before any pass), then n->next=NULL
 *     (relaxed), then LOCK XCHG on tail (full fence), then a RELEASE store
 *     publishing prev->next=n.
 *   - Consumer: ACQUIRE load of head->next; it synchronises-with the
 *     producer's RELEASE, making run/ctx visible.
 *   - The one Vyukov window — producer past its XCHG but before its
 *     prev->next store — makes pop transiently observe "empty". That node
 *     is NOT lost (it already owns tail); the next pump returns it. Same-
 *     core producers (the BSP's own IRQs) cannot be mid-pass during the
 *     BSP's HLT gate (CLI serialises IRQ handlers, which run to
 *     completion); cross-core producers send IPI_WAKE, so even the
 *     transient window cannot lose a wakeup.
 */

#include "ktypes.h"
#include "kernel_config.h"
#include "error.h"

/* Intrusive MPSC node, embedded BY VALUE in whatever the continuation is
 * about. run+ctx are stored explicitly (not via container_of) so the
 * consumer is type-blind across the heterogeneous containers (WriteJob,
 * ObjReadAsyncCtx, ahci_port_t, process_t). Set before the first pass and
 * reused across every pass of that container; `next` is owned by the
 * queue. */
typedef struct Baton Baton;
struct Baton {
    Baton *volatile next;  /* MPSC chain; NULL at tail (atomic)   */
    void (*run)(void *ctx);            /* K-Core continuation                 */
    void  *ctx;                        /* argument — the container job        */
};

/* Per-core single-consumer queue. Producer end (`tail`) is XCHG'd by any
 * core; consumer end (`head`) is touched ONLY by the owning core. The
 * persistent `stub` keeps head/tail non-NULL so neither push nor pop ever
 * special-cases an empty queue. Producer and consumer cursors sit on
 * separate cachelines (io_uring / Disruptor pattern) so a producer XCHG
 * never invalidates the consumer's line. */
typedef struct BatonQueue {
    Baton *volatile tail;  /* producer XCHG cursor (atomic)       */
    char _pad_tail[CONFIG_CACHE_LINE_SIZE - sizeof(Baton *)];

    Baton *head;           /* consumer cursor — single-consumer   */
    char _pad_head[CONFIG_CACHE_LINE_SIZE - sizeof(Baton *)];

    Baton  stub;           /* dummy anchor, never delivered       */

    /* Telemetry only — never load-bearing. The stub re-anchor is NOT
     * counted, so at quiescence pushed == popped == real completions. */
    volatile uint64_t  pushed;
    volatile uint64_t  popped;
} BatonQueue;

/* Sized to g_amp.total_cores at init. */
extern BatonQueue *g_baton;
extern volatile uint8_t        g_baton_ready;

/* Allocate + init the per-core queues. Run after amp_init() and before any
 * IRQ that may pass a baton — a PIT tick with a park deadline, an AHCI MSI —
 * on every core count (wired right after irq_defer_init in main.c). */
void BatonInit(void);

/* Producer. IRQ-safe, allocation-free, NEVER drops (the node is embedded
 * in the caller's container). Routes the node to the drain core's queue
 * and, if called from another core, sends IPI_WAKE so a HLT'd drainer wakes.
 * run and ctx must already be set in `n`, and `n` must not be queued
 * already — a container that can be passed twice before it is run keeps
 * its own one-slot gate (see process_t.deadline_passed). */
void BatonPass(Baton *n);

/* Consumer. Drains every ready node on `core_idx`'s queue, running each
 * continuation. CONTRACT: single-consumer — call ONLY with the calling
 * core's own index, and never from two contexts that can interleave on
 * that core (kcore_run_loop; on one core the idle loop and the tick from
 * ring 3). Returns the number run. */
uint32_t BatonPump(uint8_t core_idx);

/* HLT-gate helper: true iff `core_idx`'s queue has a real (non-stub) node
 * awaiting drain. Must be called by the owning core (it reads `head`).
 * Structural — no counter — so it never drifts. */
bool BatonPending(uint8_t core_idx);

/* Halt-drain helper: true iff any completion posted to `core_idx`'s queue
 * has not yet been consumed (pushed != popped). Uses atomic counters, so —
 * unlike BatonPending, which reads the SC-owned head — it is
 * safe to call from a core that does NOT own the queue (system_halt runs on
 * an arbitrary core while the drain core is still pumping). */
bool BatonOutstanding(uint8_t core_idx);

/* Deterministic boot self-test of the MPSC primitive (never-drop, exactly-
 * once, per-producer FIFO, stub re-anchor). Drives a private queue on the
 * calling core; does not touch the live per-core queues. Returns OK on pass.
 * Emits "[BATON-TEST] PASS". */
error_t BatonSelfTest(void);

#endif /* BATON_H */
