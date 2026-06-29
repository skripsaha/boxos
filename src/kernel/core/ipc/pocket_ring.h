#ifndef POCKET_RING_H
#define POCKET_RING_H

#include "ktypes.h"
#include "pocket.h"
#include "cabin_layout.h"

/*
 * PocketRing — SPSC, monotonic 64-bit indices, lazy slot allocation.
 *
 *   Userspace producer: writes Pocket at slots_base + (tail % cap)*slot_size,
 *   then bumps tail. The slot page is mapped on-demand by the kernel page
 *   fault handler (vmm_handle_page_fault → vmm_demand_map_user_page).
 *
 *   Kernel consumer: reads Pocket at slots_base + (head % cap)*slot_size, then
 *   bumps head. Slot pages are guaranteed mapped (the producer only writes
 *   slots that already faulted in), but kernel access goes through
 *   vmm_translate_user_addr because Cabin's CR3 may not be active.
 *
 * Empty: head == tail
 * Full:  tail - head == slot_count_max  (8192 slots = 1 MiB at 128B/slot)
 *
 * Indices are 64-bit and never wrap during the lifetime of the universe.
 */

/*
 * PocketRingHeader — cacheline-separated cursors.
 *
 *   Cacheline 0: consumer cursor (kernel) + read-only init metadata.
 *                Kernel reads `head` to advance; the metadata fields
 *                are written once at init.
 *   Cacheline 1: producer cursor (userspace) — own cacheline.
 *                Userspace writes `tail` on every pocket submit; without
 *                separation, the kernel-side load of `head` on a
 *                different core would invalidate this line on every
 *                submit (RFO storm under 16-core stress). Intel SDM
 *                Vol 3 §11.5 (Cache Control Protocol — MESI line-state
 *                transitions) + Intel® 64 Optimization Reference
 *                Manual, Cache & Memory Subsystem chapter (false
 *                sharing). Cache line is 64 B on every shipping
 *                Intel/AMD x86_64 part; verified at runtime via CPUID.
 *                05H monitor-line probe (cpuid.c). Mirrors irq_defer's
 *                `3cdc79c` fix and the TouchRingHeader layout in
 *                touch_ring.h.
 */
typedef struct __packed {
    /* Cacheline 0 — consumer cursor + read-only metadata. */
    volatile uint64_t head;             /* consumer cursor (kernel)        */
    uint64_t          slots_base;       /* user vaddr of slot 0            */
    uint32_t          slot_size;        /* bytes per slot stride           */
    uint32_t          slot_count_max;   /* hard upper bound on tail-head   */
    uint64_t          magic;            /* POCKET_RING_MAGIC               */
    uint8_t           _pad_line0[32];   /* fill cacheline 0                */

    /* Cacheline 1 — producer cursor (userspace). */
    volatile uint64_t tail;             /* producer cursor (userspace)     */
    uint8_t           _pad_line1[56];   /* fill cacheline 1                */
} PocketRingHeader;

_Static_assert(sizeof(PocketRingHeader) == 128,
               "PocketRingHeader must be 128 bytes (two cachelines)");
_Static_assert(__builtin_offsetof(PocketRingHeader, head) == 0,
               "PocketRingHeader.head must start at offset 0");
_Static_assert(__builtin_offsetof(PocketRingHeader, tail) == 64,
               "PocketRingHeader.tail must start at cacheline 1 (offset 64)");

/* The full ring page is the header followed by zero padding. We allocate one
 * physical page; only the first sizeof(PocketRingHeader) bytes carry meaning. */
typedef struct __packed {
    PocketRingHeader hdr;
    uint8_t          _page_pad[4096 - sizeof(PocketRingHeader)];
} PocketRing;

_Static_assert(sizeof(PocketRing) == 4096,
               "PocketRing header page must be exactly one page");

/* Straddle-safety geometry (Crate-boundary straddle hardening 7/7).
 * pocket_ring_slot_uvaddr returns slots_base + (idx % cap) * POCKET_SLOT_SIZE
 * and the kernel translates exactly sizeof(Pocket) there. That single
 * translation can never cross a 4 KiB page boundary because the stride equals
 * the struct size, POCKET_SLOT_SIZE divides the page, and the slot base is
 * page-aligned (the runtime per-strand base is checked in KRingPocketInitAt). */
_Static_assert(sizeof(Pocket) == POCKET_SLOT_SIZE,
               "Pocket must match POCKET_SLOT_SIZE so a slot never straddles a page");
_Static_assert(4096 % POCKET_SLOT_SIZE == 0,
               "POCKET_SLOT_SIZE must divide a 4 KiB page");
_Static_assert((CABIN_POCKET_SLOTS_BASE & 0xFFFULL) == 0,
               "CABIN_POCKET_SLOTS_BASE must be page-aligned");

/* ---- Inline accessors (callable from kernel where slots_base is *user* VA
 *      mapped into the current cabin; otherwise use the kring helpers below) */

static inline bool pocket_ring_is_empty(const PocketRing *r)
{
    return r->hdr.head == r->hdr.tail;
}

static inline bool pocket_ring_is_full(const PocketRing *r)
{
    return (r->hdr.tail - r->hdr.head) >= r->hdr.slot_count_max;
}

static inline uint32_t pocket_ring_count(const PocketRing *r)
{
    uint64_t n = r->hdr.tail - r->hdr.head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

static inline uintptr_t pocket_ring_slot_uvaddr(const PocketRing *r, uint64_t idx)
{
    return (uintptr_t)(r->hdr.slots_base
                       + (idx % r->hdr.slot_count_max) * r->hdr.slot_size);
}

#endif /* POCKET_RING_H */
