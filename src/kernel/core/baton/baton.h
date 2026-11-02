#ifndef BATON_H
#define BATON_H

/* =========================================================================
 *  Baton — the never-drop hand-off from an interrupt to a K-Core
 * =========================================================================
 *
 * A baton is passed from hand to hand; it is never thrown into a basket that
 * may have no room. The node that carries the continuation is EMBEDDED in
 * the thing it is about — a WriteJob, a ReadJob, an xHCI transfer,
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
 *   - storage async completions (AHCI, xHCI MSD): a block landed, a
 *     WriteJob step came back from an IRQ, an AHCI port recovery;
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
 * sync command, whose caller may hold a TagFS lock — does not ride here in
 * any part: the IRQ stamps the channel's own `landing` slot and the spinner
 * claims and runs the bottom half itself, and a reset the channel owes is run
 * by the spinner that finds it owed (ata_async.c). So the classic cycle (the
 * drain core spins on a lock held by a core that is itself waiting on a
 * continuation only the drain core can run) has no edge to form on.
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
 * ReadJob, ahci_port_t, process_t). Set before the first pass and
 * reused across every pass of that container; `next` is owned by the
 * queue. */
typedef struct Baton Baton;
struct Baton {
    Baton *volatile next;  /* MPSC chain; NULL at tail (atomic)   */
    void (*run)(void *ctx);            /* K-Core continuation                 */
    void  *ctx;                        /* argument — the container job        */

    /* Owned by the PASS, never by the container: BatonPass clears it and
     * BatonPassOneWay sets it, both before the node is linked, so a container
     * that never zeroed its node cannot make an ordinary pass look one-way and
     * a caller cannot make a pass one-way by writing here. It rides on the
     * NODE because the property belongs to the WORK: when the pump has to
     * decide whether the count may wait for a return, the popped node is all
     * it holds. */
    volatile uint32_t one_way;
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

    /* Also telemetry, and for one question only: when a queue will not go
     * quiet, WHOSE work is sitting in it. The continuation pointer is enough
     * to answer that, because the Nameplate can turn it back into the name of
     * the function that was going to run — so a stuck queue can be reported as
     * "a write's commit" or "an xHCI transfer" instead of as a number. Plain
     * volatile stores beside pushed/popped: a torn read would misname a line
     * of diagnostics and nothing else, which is why this is not worth an
     * atomic on the push path. */
    void (*volatile last_push_run)(void *ctx);
    void (*volatile last_pop_run)(void *ctx);
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

/* A pass whose continuation will NOT return — it halts, resets or parks
 * the machine. Counted when it is TAKEN, never on return, because it
 * never comes back. Every other pass keeps the return-counted rule, which
 * is what lets a halt drain wait for a write's commit rather than merely
 * for its node to be dequeued.
 *
 * Takes no node, because work that does not return has no container that
 * outlives it to embed one in: the baton layer reserves ONE node per core for
 * this, and the claim on that node is the marker itself, never cleared. A core
 * therefore takes at most one one-way pass for the life of the boot — the
 * first does not come back to release it — and a second attempt is refused and
 * said, which is the truth and not a drop: the machine is already going where
 * the loser wanted to send it. `core` is the core that will take the pass, and
 * it is named rather than assumed, because a one-way pass is about WHERE the
 * machine stops, not about where completions happen to be drained.
 *
 * Refused (false, and said out loud) before BatonInit, on an unknown core, on
 * a NULL continuation, or when that core is already one-way. */
bool BatonPassOneWay(uint8_t core, void (*run)(void *ctx), void *ctx);

/* Consumer. Drains every ready node on `core_idx`'s queue, running each
 * continuation. CONTRACT: single-consumer — call ONLY with the calling
 * core's own index, and never from two contexts that can interleave on
 * that core (kcore_run_loop; on one core the idle loop and the tick from
 * ring 3). A continuation may itself pump (a sync wait on the drain core
 * does): the node it is running under was popped before it ran, so the
 * nested drain sees a consistent head. Returns the number run.
 *
 * BatonPump is the pump of a core that is in the middle of something — a
 * wait for a Scribe, a stood-over command, a drain: it never takes a one-way
 * pass. One it meets is set aside, and the core's own loop takes it
 * (BatonPumpAtRest), because the halt a one-way pass begins syncs the volume,
 * and a sync begun beneath a pass this core holds would wait on that pass for
 * ever. BatonPumpAtRest is the pump of a core that stands on nothing — the
 * K-Core loop between pockets, the idle loop, the one-core tick that
 * interrupted user mode — and is the one place a one-way pass is taken. */
uint32_t BatonPump(uint8_t core_idx);
uint32_t BatonPumpAtRest(uint8_t core_idx);

/* Whether this core is running a continuation that will return — the ones
 * that may not take a Scribe (scribe.h, rule 6). */
bool BatonRunningHere(void);

/* Whether the only core has taken its one-way pass: then the idle context is
 * owed the next quantum from whoever ran this one, as RT-10 owes it for a
 * cycle (idle.c, IdleOwed). By the scheduler, on its own core. */
bool BatonOneWayIdleOwed(void);

/* How many passes this core's queue is still holding. BatonOutstanding only
 * ever said WHETHER a queue was busy, which made "one unit of work" mean "one
 * core has something" and hid how much — the shutdown drain reported a single
 * unit stalled when it had no idea whether that was one pass or a thousand.
 *
 * A one-way pass stops counting the moment it is taken, so a depth read from
 * inside one (system_halt's drain runs inside exactly that) does not include
 * the pass it is standing in. Waiting for it would be waiting for a return
 * that was declared never to come. */
uint64_t BatonDepth(uint8_t core_idx);

/* The continuation most recently PUSHED to, and most recently POPPED from,
 * this core's queue. Diagnostics only; NULL when the queue has never been
 * used. Pass them to the Nameplate to get a name. */
void BatonLastSeen(uint8_t core_idx,
                   void (**out_pushed)(void *ctx),
                   void (**out_popped)(void *ctx));

/* HLT-gate helper: true iff `core_idx`'s queue has a real (non-stub) node
 * awaiting drain, or a one-way pass set aside for its loop. Must be called by
 * the owning core (it reads `head`). Structural — no counter — so it never
 * drifts. */
bool BatonPending(uint8_t core_idx);

/* Halt-drain helper: true iff any completion posted to `core_idx`'s queue has
 * not yet been consumed (pushed != popped) — a pass posted, or dequeued and
 * still running, EXCEPT a one-way pass, which is counted when taken and is
 * therefore never outstanding while it runs. Uses atomic counters, so —
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
