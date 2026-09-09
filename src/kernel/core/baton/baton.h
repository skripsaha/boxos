#ifndef BATON_H
#define BATON_H

/* =========================================================================
 *  Baton — the never-drop hand-off from an interrupt to a K-Core
 * =========================================================================
 *
 * A baton is passed from hand to hand; it is never thrown into a basket that
 * may have no room. The node that carries the continuation is EMBEDDED in
 * the thing it is about — a WriteJob, an ObjReadAsyncCtx, an xHCI transfer,
 * a process's park deadline, a GHES error source, an MCE migration slot —
 * so a pass needs no allocation and no free slot, and cannot be dropped for
 * want of one. Drop is not made rare; it is made impossible.
 *
 * Why this exists (the drop a basket of slots could not avoid)
 * -----------------------------------------------------------
 * The kernel used to carry interrupt bottom-halves in a per-core ring of
 * pre-allocated slots. An interrupt can neither wait for room nor allocate,
 * so when a burst outran the pump's refill — or the refill's kmalloc failed
 * under memory pressure — the producer found no slot and DROPPED the event,
 * counted and never said. For a storage completion that is fatal: the caller
 * parked on PROC_WAITING is woken only by the completion's KResultPush, so a
 * dropped completion hangs it FOREVER and leaks the command slot. For a park
 * deadline it is the same stall with a different name: the tick reschedules
 * the sleeper, but the ERR_TIMEOUT Result it is waiting for never comes, and
 * boxlib used to cover that with a +100 ms clock of its own — a guessed
 * number over a delivery the kernel had promised. For a keystroke it is a key
 * the machine heard and never said. The naive fix (a registry that
 * re-enqueues stuck jobs) is use-after-free prone (a duplicate continuation
 * races the still-live original into its free). The ring is gone; this is
 * the one road from an interrupt to a K-Core.
 *
 * Who rides it
 * ------------
 *   - storage async completions (AHCI, BMIDE, xHCI MSD): a block landed, a
 *     WriteJob step came back from an IRQ, a port recovery;
 *   - the deadline of a timed addr_park: the PIT tick passes the process's
 *     own baton (process_t.deadline_baton) and a K-Core delivers the
 *     ERR_TIMEOUT Result where KResultPush (cabin VMM) is safe;
 *   - everything an interrupt notices and cannot finish where it stands —
 *     through a Knock (below): the Touch IRQ ring (a keystroke, a port
 *     event, a BMIDE error report), the power button, a GHES error source;
 *     and an MCE migration slot, which carries a baton of its own.
 *
 * Single-consumer, and why that is deadlock-free here
 * ---------------------------------------------------
 * This is a Vyukov intrusive MPSC with a persistent stub: multi-producer
 * (any core's IRQ or pump may pass), single-consumer (each per-core queue
 * is drained ONLY by its owning core). A single consumer is safe because of
 * one invariant:
 *
 *   INVARIANT P: nothing SPINS for a baton's continuation while holding a
 *   lock. A waiter for one parks (PROC_WAITING, woken by KResultPush) and
 *   holds no spinlock across the wait; every other rider has no waiter at
 *   all — its continuation is a delivery, not an answer somebody stands on.
 *
 * The one payload that does have a spinning, lock-holding waiter — a BMIDE
 * sync completion, whose caller may hold a TagFS write lock — does not ride
 * here: the IRQ stamps the channel's own `landing` slot and the spinner
 * claims and runs the bottom half itself (ata_async.c). So the classic cycle
 * (the drain core spins on a lock held by a core that is itself waiting on
 * a continuation only the drain core can run) has no edge to form on.
 *
 * Every pass routes to the drain core (the BSP — the AHCI MSI owner and the
 * PIT's core). Per-core queues keep the consumer trivially single (each core
 * pumps only its own), so the uniform BatonPump(my_idx) in every K-Core
 * needs no "am I the BSP" special case; non-BSP queues stay empty and their
 * pump is a one-load early-out. On one core the BSP runs no K-Core loop: the
 * idle loop pumps, and so does the timer tick when it interrupted user mode
 * (the two never overlap — the tick pumps only from ring 3, and idle is
 * ring 0).
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
 *     transient window cannot lose a wakeup. A producer interrupted by an
 *     NMI that passes too is the same window from the other side: the NMI's
 *     link lands behind the interrupted one and both are delivered.
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
 * IRQ that may pass a baton — a PIT tick with a park deadline, an AHCI MSI,
 * a keystroke — on every core count; the sti that opens the machine to
 * interrupts comes after it in main.c. */
void BatonInit(void);

/* Producer. IRQ-safe, NMI-safe, allocation-free, NEVER drops (the node is
 * embedded in the caller's container). Routes the node to the drain core's
 * queue and, if called from another core, sends IPI_WAKE so a HLT'd drainer
 * wakes. run and ctx must already be set in `n`, and `n` must not be queued
 * already — a container that can be passed twice before it is run keeps its
 * own one-slot gate (process_t.deadline_passed, or a Knock).
 *
 * Returns true when the node is in the queue. The one refusal is a pass
 * before BatonInit — said out loud, because a refused pass is exactly the
 * drop this queue exists to make impossible — and a gate that let the pass
 * through must reopen on it, or nobody will ever pass again. */
bool BatonPass(Baton *n);

/* Consumer. Drains every ready node on `core_idx`'s queue, running each
 * continuation. CONTRACT: single-consumer — call ONLY with the calling
 * core's own index, and never from two contexts that can interleave on
 * that core (kcore_run_loop; on one core the idle loop and the tick from
 * ring 3). A continuation may itself pump (a sync wait on the drain core
 * does): the node it is running under was popped before it ran, so the
 * nested drain sees a consistent head. Returns the number run. */
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

/* =========================================================================
 *  Knock — one baton for a thing that can be noticed many times
 * =========================================================================
 *
 * A baton must not be passed again while it is still in the queue. Some
 * things an interrupt notices have no container of their own to carry a
 * baton each: a keystroke, a port event, an error a firmware block is
 * holding. What they have is a PLACE the interrupt leaves them in — a ring
 * slot claimed by fetch_add, a status block, a mailbox — and a reader who
 * comes and takes everything that accumulated there. The Knock is the one
 * baton for that place: knock as often as the events come, and until the
 * door is opened it is one knock; when it is opened, the reader sees
 * everyone who came.
 *
 * Producer (any context, allocation-free): KnockOn. It claims the knock with
 * a CAS 0->1 and passes the baton only when it won; a loser knows a pass is
 * already on its way and that the reader, once it opens, will look at the
 * place after this producer's writes (the CAS is a full fence, and the
 * reader's open is one too — Dekker: either the producer's CAS sees the door
 * open and passes, or the reader's look after opening sees what the producer
 * left). So a producer writes its place FIRST and knocks after.
 *
 * Consumer (the continuation the baton runs): KnockOpen — EXACTLY ONCE per
 * run, and first. The node was popped before the run began, so a knock
 * arriving after the open links it again for a next visit; a second open in
 * the same run would let a knock link a node that is already queued, which
 * is corruption. Opening first and then reading up to the producers' cursor
 * is what makes a producer that finishes after the open bring the next
 * visit instead of being missed.
 */
typedef struct Knock {
    Baton             baton;
    volatile uint32_t raised;   /* 1 from the winning KnockOn until KnockOpen */
} Knock;

/* Set run/ctx once, before the first knock. */
void KnockInit(Knock *k, void (*run)(void *ctx), void *ctx);

/* Producer: pass the baton unless a pass is already on its way. IRQ- and
 * NMI-safe, allocation-free, never drops: the place the producer wrote is
 * read by the visit this knock or the one already coming brings. */
void KnockOn(Knock *k);

/* Consumer: the door opens — exactly once per run of the continuation. */
void KnockOpen(Knock *k);

/* Deterministic boot self-test of the MPSC primitive (never-drop, exactly-
 * once, per-producer FIFO, stub re-anchor) and of the Knock (many knocks,
 * one visit; a knock after the open, one more). Drives a private queue on
 * the calling core; does not touch the live per-core queues. Returns OK on
 * pass. Emits "[BATON-TEST] PASS". */
error_t BatonSelfTest(void);

#endif /* BATON_H */
