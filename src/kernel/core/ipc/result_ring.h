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
    uint8_t           _pad_line1[48];   /* fill cacheline 1                */
} ResultRingHeader;

_Static_assert(sizeof(ResultRingHeader) == 128,
               "ResultRingHeader must be 128 bytes (two cachelines)");
_Static_assert(__builtin_offsetof(ResultRingHeader, head) == 0,
               "ResultRingHeader.head must start at offset 0");
_Static_assert(__builtin_offsetof(ResultRingHeader, tail) == 64,
               "ResultRingHeader.tail must start at cacheline 1 (offset 64)");
_Static_assert(__builtin_offsetof(ResultRingHeader, awaiting) == 72,
               "ResultRingHeader.awaiting must sit at offset 72 — both sides "
               "of the boundary agree on it by number, not by luck");

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
