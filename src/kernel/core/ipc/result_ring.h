#ifndef RESULT_RING_H
#define RESULT_RING_H

#include "ktypes.h"
#include "result.h"
#include "cabin_layout.h"


typedef struct __packed {
    volatile uint64_t head;
    uint64_t          slots_base;
    uint32_t          slot_size;
    uint32_t          slot_count_max;
    uint64_t          magic;
    uint8_t           _pad_line0[32];

    volatile uint64_t tail;
    volatile uint64_t awaiting;
    uint8_t           _pad_line1[48];
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

typedef struct __packed {
    Result   r;
    uint64_t seq;
} ResultSlot;

_Static_assert(sizeof(ResultSlot) == 32,
               "ResultSlot must match RESULT_SLOT_SIZE for cabin layout");
_Static_assert(sizeof(ResultSlot) == RESULT_SLOT_SIZE,
               "ResultSlot stride mismatch with cabin_layout.h");
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

#endif