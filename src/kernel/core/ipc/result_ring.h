#ifndef RESULT_RING_H
#define RESULT_RING_H

#include "ktypes.h"
#include "result.h"
#include "cabin_layout.h"

/*
 * ResultRing — SPSC, monotonic 64-bit indices, lazy slot allocation.
 *
 *   Kernel producer: kresult_push() (see kring.h) ensures the destination slot
 *   page is mapped before writing, then writes via vmm_translate_user_addr
 *   and bumps tail.
 *
 *   Userspace consumer: reads Result at slots_base + (head % cap)*slot_size,
 *   then bumps head. The slot pages it touches were already mapped by the
 *   kernel as a side effect of the corresponding push, so no fault occurs in
 *   correct SPSC use.
 *
 * Slot stride is RESULT_SLOT_SIZE (32B), but sizeof(Result)=24 — the trailing
 * 8 bytes per slot are padding that prevents slots from straddling page
 * boundaries (4096 % 32 == 0).
 */

typedef struct __packed {
    volatile uint64_t head;             /* consumer cursor (userspace)     */
    volatile uint64_t tail;             /* producer cursor (kernel)        */
    uint64_t          slots_base;       /* user vaddr of slot 0            */
    uint32_t          slot_size;        /* RESULT_SLOT_SIZE                */
    uint32_t          slot_count_max;   /* hard upper bound on tail-head   */
    uint64_t          magic;            /* RESULT_RING_MAGIC               */
    uint8_t           _pad[24];
} ResultRingHeader;

_Static_assert(sizeof(ResultRingHeader) == 64,
               "ResultRingHeader must be 64 bytes");

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
