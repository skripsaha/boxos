#ifndef BOX_CORE_POCKET_H
#define BOX_CORE_POCKET_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/core/strand_self.h"


#include "boxos_pocket.h"

typedef struct PACKED {
    volatile uint64_t head;
    uint64_t          slots_base;
    uint32_t          slot_size;
    uint32_t          slot_count_max;
    uint64_t          magic;
    uint8_t           _pad_line0[32];

    volatile uint64_t tail;
    uint8_t           _pad_line1[56];
} PocketRingHeader;

STATIC_ASSERT(sizeof(PocketRingHeader) == 128,
              "PocketRingHeader must be 128 bytes (two cachelines)");

typedef struct PACKED {
    PocketRingHeader hdr;
    uint8_t          _page_pad[4096 - sizeof(PocketRingHeader)];
} PocketRing;

STATIC_ASSERT(sizeof(PocketRing) == 4096, "PocketRing header must be one page");

INLINE PocketRing* pocket_ring(void) {
    return (PocketRing*)(uintptr_t)strand_rings().pocket_va;
}

INLINE bool pocket_ring_is_empty(const PocketRing* ring) {
    return ring->hdr.head == ring->hdr.tail;
}

INLINE bool pocket_ring_is_full(const PocketRing* ring) {
    return (ring->hdr.tail - ring->hdr.head) >= ring->hdr.slot_count_max;
}

INLINE uint32_t pocket_ring_count(const PocketRing* ring) {
    uint64_t n = ring->hdr.tail - ring->hdr.head;
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

INLINE bool pocket_ring_push(PocketRing* ring, const Pocket* p) {
    if (!ring || !p) return false;

    uint64_t magic   = __atomic_load_n(&ring->hdr.magic,          __ATOMIC_RELAXED);
    uint32_t cap     = __atomic_load_n(&ring->hdr.slot_count_max, __ATOMIC_RELAXED);
    uint64_t base    = __atomic_load_n(&ring->hdr.slots_base,     __ATOMIC_RELAXED);
    uint32_t stride  = __atomic_load_n(&ring->hdr.slot_size,      __ATOMIC_RELAXED);

    if (magic   != POCKET_RING_MAGIC) return false;
    if (cap     == 0)                 return false;
    if (base    == 0)                 return false;
    if (stride  == 0)                 return false;
    if (base    <  0x100000000ULL)    return false;

    if (pocket_ring_is_full(ring)) return false;

    uint64_t idx  = __atomic_load_n(&ring->hdr.tail, __ATOMIC_RELAXED);
    if ((idx - __atomic_load_n(&ring->hdr.head, __ATOMIC_ACQUIRE)) >= cap) return false;

    Pocket *slot = (Pocket *)(uintptr_t)(base + (idx % cap) * stride);
    *slot = *p;
    __atomic_store_n(&ring->hdr.tail, idx + 1, __ATOMIC_RELEASE);
    return true;
}

#ifdef __cplusplus
}
#endif

#endif