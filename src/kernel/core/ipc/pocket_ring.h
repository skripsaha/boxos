#ifndef POCKET_RING_H
#define POCKET_RING_H

#include "ktypes.h"
#include "pocket.h"
#include "cabin_layout.h"


typedef struct __packed {
    volatile uint64_t head;
    uint64_t          slots_base;
    uint32_t          slot_size;
    uint32_t          slot_count_max;
    uint64_t          magic;
    uint8_t           _pad_line0[32];

    volatile uint64_t tail;
    uint8_t           _pad_line1[56];
} PocketRingHeader;

_Static_assert(sizeof(PocketRingHeader) == 128,
               "PocketRingHeader must be 128 bytes (two cachelines)");
_Static_assert(__builtin_offsetof(PocketRingHeader, head) == 0,
               "PocketRingHeader.head must start at offset 0");
_Static_assert(__builtin_offsetof(PocketRingHeader, tail) == 64,
               "PocketRingHeader.tail must start at cacheline 1 (offset 64)");

typedef struct __packed {
    PocketRingHeader hdr;
    uint8_t          _page_pad[4096 - sizeof(PocketRingHeader)];
} PocketRing;

_Static_assert(sizeof(PocketRing) == 4096,
               "PocketRing header page must be exactly one page");

_Static_assert(sizeof(Pocket) == POCKET_SLOT_SIZE,
               "Pocket must match POCKET_SLOT_SIZE so a slot never straddles a page");
_Static_assert(4096 % POCKET_SLOT_SIZE == 0,
               "POCKET_SLOT_SIZE must divide a 4 KiB page");
_Static_assert((CABIN_POCKET_SLOTS_BASE & 0xFFFULL) == 0,
               "CABIN_POCKET_SLOTS_BASE must be page-aligned");


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

#endif