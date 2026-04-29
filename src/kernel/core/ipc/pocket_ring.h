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

typedef struct __packed {
    volatile uint64_t head;             /* consumer cursor (kernel)        */
    volatile uint64_t tail;             /* producer cursor (userspace)     */
    uint64_t          slots_base;       /* user vaddr of slot 0            */
    uint32_t          slot_size;        /* bytes per slot stride           */
    uint32_t          slot_count_max;   /* hard upper bound on tail-head   */
    uint64_t          magic;            /* POCKET_RING_MAGIC               */
    uint8_t           _pad[24];
} PocketRingHeader;

_Static_assert(sizeof(PocketRingHeader) == 64,
               "PocketRingHeader must be 64 bytes");

/* The full ring page is the header followed by zero padding. We allocate one
 * physical page; only the first 64 bytes carry meaning. */
typedef struct __packed {
    PocketRingHeader hdr;
    uint8_t          _page_pad[4096 - sizeof(PocketRingHeader)];
} PocketRing;

_Static_assert(sizeof(PocketRing) == 4096,
               "PocketRing header page must be exactly one page");

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
