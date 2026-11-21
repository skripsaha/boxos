#ifndef RESULT_RING_H
#define RESULT_RING_H

#include "ktypes.h"
#include "result.h"
#include "cabin_layout.h"

/*
 * ResultRing — MPSC producer / SP consumer, monotonic 64-bit indices,
 * lazy slot allocation.
 *
 *   Kernel producers (multiple K-Cores): KResultPush atomically reserves a
 *   unique slot index via fetch_add(tail), maps the slot page on demand,
 *   spins on a per-slot generation counter (slot.seq) until the previous
 *   round has been drained by the consumer, writes the payload, and
 *   release-stores slot.seq to publish.
 *
 *   Userspace consumer (single, the cabin owner): reads slot[head%cap], gates
 *   on slot.seq matching the expected publish value for the current round,
 *   copies the payload, release-stores slot.seq to free the slot for the
 *   next producer round, and bumps head.
 *
 * Slot stride RESULT_SLOT_SIZE (32B) packs Result (24B) + per-slot seq (8B)
 * exactly. The seq generation pattern is:
 *
 *   seq == 2*round           — slot is empty / available for round (round = pos / cap)
 *   seq == 2*round + 1       — producer has written, consumer has not read
 *   seq == 2*(round + 1)     — consumer has read, slot ready for next round
 *
 * Initial state: all seq fields are zero (pages are pmm_alloc_zero'd on first
 * touch). Round-0 producers see seq == 0 == 2*0 and proceed immediately, so
 * no eager initialisation is needed — the lazy-mapping invariant is preserved.
 */

/*
 * ResultRingHeader — cacheline-separated cursors.
 *
 *   Cacheline 0: consumer cursor (userspace) + read-only init metadata.
 *                Userspace reads `head` to drain results; producers
 *                only read it (via ACQUIRE) for full-checks.
 *   Cacheline 1: producer reservation cursor (kernel MPSC) — own
 *                cacheline. Many K-Cores hammer __atomic_fetch_add(&tail)
 *                concurrently; without separation, every push would
 *                invalidate the consumer's `head` cacheline (and vice
 *                versa), producing RFO storms on real 16-core silicon.
 *                Intel SDM Vol 3 §11.5 (Cache Control Protocol — MESI
 *                line-state transitions) + Intel® 64 Optimization
 *                Reference Manual, Cache & Memory Subsystem chapter
 *                (false sharing). Cache line is 64 B on every shipping
 *                Intel/AMD x86_64 part; verified at runtime via CPUID.
 *                05H monitor-line probe (cpuid.c). Mirrors irq_defer's
 *                `3cdc79c` fix and TouchRingHeader / PocketRingHeader
 *                layouts.
 */
typedef struct __packed {
    /* Cacheline 0 — consumer cursor + read-only metadata. */
    volatile uint64_t head;             /* consumer cursor (userspace)     */
    uint64_t          slots_base;       /* user vaddr of slot 0            */
    uint32_t          slot_size;        /* RESULT_SLOT_SIZE                */
    uint32_t          slot_count_max;   /* hard upper bound on tail-head   */
    uint64_t          magic;            /* RESULT_RING_MAGIC               */
    uint8_t           _pad_line0[32];   /* fill cacheline 0                */

    /* Cacheline 1 — producer reservation cursor (kernel MPSC). */
    volatile uint64_t tail;             /* producer reservation cursor (kernel) */
    /* What this strand is waiting for, written by the WAITER.
     *
     * A strand inside result_wait is, from the kernel's side, indistinguishable
     * from a strand doing useful work — parked or spinning, neither state says
     * WHAT for. That is why a machine could stand still for an hour with
     * Nightwatch armed and silent. It is the one thing the kernel cannot infer
     * and the waiter alone knows, so the waiter says it: the cloakroom token it
     * is holding out for, zero when it holds out for none.
     *
     * The kernel only reads it (nightwatch.c, in the verdict's walk over every
     * process — parked ones included), never writes it, and treats it as a
     * HINT: a guest may write anything here and can only mislead the report
     * about itself, which is why the verdict convicts on facts and never on
     * this token alone. It costs the waiter two stores per submit. */
    volatile uint64_t awaiting;
    /* Answers the kernel ACCEPTED for this ring but could not fit into it —
     * the Owed queue's depth (kring.c). Written by the kernel under the
     * owner's result_owed_lock, read-only for userspace.
     *
     * It sits HERE, on the producer cacheline, for the same reason
     * TouchRingHeader.owed does: that is the line the consumer already
     * watches. boxlib's bounded waits arm UMONITOR on &tail and then UMWAIT
     * (result.c, result_wait_umwait / result_wait_ipc_umwait), and a ring that
     * is full has a tail that will not move — a consumer that drained its
     * slots and went back to sleep would never learn the kernel is still
     * holding answers for it. Writing this counter writes that line, so the
     * store that records the debt is also what stirs the sleeper; no bell had
     * to be invented for it. The debt itself is settled by the owner at the
     * syscall gate, not by anyone reading this number. */
    volatile uint64_t owed;
    /* Step aside: the kernel asking the strand that holds this core to give it
     * up — written by the kernel, read-only for userspace.
     *
     * 1 when another strand is waiting in this core's runqueue (set when one
     * is queued here, recomputed at every dispatch). A strand parked while
     * still on its CPU is not asked: its core is rung and takes it off
     * (sched_ring_running). A spin that
     * waits on something ANOTHER strand must produce — the display daemon's
     * hot look, a Touch or Brook budget, a reply wait — cannot end on its own
     * when that strand is queued behind it on the same core: the core it waits
     * for is the one it holds. It ends on this word instead, and yields.
     *
     * On the tail's cacheline for the reason `owed` is: UMWAIT on &tail is
     * stirred by the store that sets it. */
    volatile uint64_t step_aside;
    uint8_t           _pad_line1[32];   /* fill cacheline 1                */

    /* Cacheline 2 — the stopwatch (make MEASURE=on; zero in any other build).
     * The answer to the strand's own call pushed last, as the kernel saw it:
     * when the call came to the gate, when a K-Core took it and when it was
     * answered, in TSC cycles. Written by the kernel before it publishes that
     * answer, so a strand that has the answer reads the moments that go with
     * it; a line of its own, so neither cursor's line is touched for it. */
    volatile uint64_t call_gate;
    volatile uint64_t call_taken;
    volatile uint64_t call_answered;
    /* And the strand's yields that gave its core to another: how many, and
     * the PID of the one it went to last (UINT32_MAX the idle strand). */
    volatile uint32_t yields_given_away;
    volatile uint32_t yield_went_to;
    /* What the K-Core spent serving that answer, in TSC cycles: taken to its
     * pocket read, its Manifest staged, its crates staged in, its ops, its
     * crates written back, and on to the answer — all 0 where the marks are
     * not in order (a pocket refused before its crates were staged). */
    volatile uint32_t call_part[6];
    uint8_t           _pad_line2[8];    /* fill cacheline 2                */

    /* Cacheline 3 — the stopwatch's look at the strand this strand's yield
     * gave its core to last, as it stood then: its asks to sleep (system.turn
     * .in), how many laid it down, and how many were refused because of an
     * arrival on its Touch ring, on its reply ring, or events the kernel
     * held for it. */
    volatile uint32_t went_turnin_asked;
    volatile uint32_t went_turnin_parked;
    volatile uint32_t went_turnin_refused_touch;
    volatile uint32_t went_turnin_refused_result;
    volatile uint32_t went_turnin_refused_owed;
    uint8_t           _pad_line3[44];   /* fill cacheline 3                */
} ResultRingHeader;

_Static_assert(sizeof(ResultRingHeader) == 256,
               "ResultRingHeader must be 256 bytes (four cachelines)");
_Static_assert(__builtin_offsetof(ResultRingHeader, call_gate) == 128,
               "ResultRingHeader.call_gate must start cacheline 2 — boxlib's "
               "mirror agrees");
_Static_assert(__builtin_offsetof(ResultRingHeader, head) == 0,
               "ResultRingHeader.head must start at offset 0");
_Static_assert(__builtin_offsetof(ResultRingHeader, tail) == 64,
               "ResultRingHeader.tail must start at cacheline 1 (offset 64)");
_Static_assert(__builtin_offsetof(ResultRingHeader, awaiting) == 72,
               "ResultRingHeader.awaiting must sit at offset 72 — both sides "
               "of the boundary agree on it by number, not by luck");
_Static_assert(__builtin_offsetof(ResultRingHeader, owed) == 80,
               "ResultRingHeader.owed must share cacheline 1 with tail — the "
               "consumer's UMONITOR watches that line and nothing else");
_Static_assert(__builtin_offsetof(ResultRingHeader, step_aside) == 88,
               "ResultRingHeader.step_aside must share cacheline 1 with tail, "
               "at offset 88 — boxlib's mirror agrees");

/* Per-slot Vyukov-style envelope: payload + generation counter.
 * Lives at slots_base + (idx % slot_count_max) * RESULT_SLOT_SIZE. */
typedef struct __packed {
    Result   r;                          /* 24 bytes */
    uint64_t seq;                        /* 8  bytes — see file comment */
} ResultSlot;

_Static_assert(sizeof(ResultSlot) == 32,
               "ResultSlot must match RESULT_SLOT_SIZE for cabin layout");
_Static_assert(sizeof(ResultSlot) == RESULT_SLOT_SIZE,
               "ResultSlot stride mismatch with cabin_layout.h");
/* Straddle-safety geometry (Crate-boundary straddle hardening 7/7):
 * result_ring_slot_uvaddr translates exactly sizeof(ResultSlot) at
 * slots_base + (idx % cap) * RESULT_SLOT_SIZE. The stride divides the page and
 * the slot base is page-aligned, so a slot never crosses a page boundary (the
 * runtime per-strand base is checked in KRingResultInitAt). */
_Static_assert(4096 % RESULT_SLOT_SIZE == 0,
               "RESULT_SLOT_SIZE must divide a 4 KiB page");
_Static_assert((CABIN_RESULT_SLOTS_BASE & 0xFFFULL) == 0,
               "CABIN_RESULT_SLOTS_BASE must be page-aligned");

typedef struct __packed {
    ResultRingHeader hdr;
    uint8_t          _page_pad[4096 - sizeof(ResultRingHeader)];
} ResultRing;

_Static_assert(sizeof(ResultRing) == 4096,
               "ResultRing header page must be exactly one page");

static inline bool result_ring_is_empty(const ResultRing *r)
{
    return r->hdr.head == r->hdr.tail;
}

static inline bool result_ring_is_full(const ResultRing *r)
{
    return (r->hdr.tail - r->hdr.head) >= r->hdr.slot_count_max;
}

static inline uint32_t result_ring_count(const ResultRing *r)
{
    uint64_t n = r->hdr.tail - r->hdr.head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

static inline uintptr_t result_ring_slot_uvaddr(const ResultRing *r, uint64_t idx)
{
    return (uintptr_t)(r->hdr.slots_base
                       + (idx % r->hdr.slot_count_max) * r->hdr.slot_size);
}

#endif /* RESULT_RING_H */
