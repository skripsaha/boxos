#ifndef POCKET_RING_H
#define POCKET_RING_H

#include "ktypes.h"
#include "pocket.h"
#include "boxos_sizes.h"

// PocketRing: per-process SPSC ring buffer for syscall Pockets.
// Lives at CABIN_POCKET_RING_ADDR (0x2000), 1 page (4KB).
// Userspace is the producer (writes Pockets, advances tail).
// Kernel is the consumer (reads Pockets, advances head).

typedef struct __packed {
    volatile uint32_t head;                    // kernel advances after processing
    volatile uint32_t tail;                    // userspace advances after writing
    Pocket slots[POCKET_RING_CAPACITY];        // 42 * 96 = 4032 bytes
} PocketRing;

// 8 + 4032 = 4040 bytes, fits in 4096
_Static_assert(sizeof(PocketRing) <= 4096, "PocketRing must fit in one page");

static inline bool pocket_ring_is_empty(const PocketRing* ring)
{
    return ring->head == ring->tail;
}

static inline bool pocket_ring_is_full(const PocketRing* ring)
{
    return ((ring->tail + 1) % POCKET_RING_CAPACITY) == ring->head;
}

static inline uint32_t pocket_ring_count(const PocketRing* ring)
{
    uint32_t h = ring->head;
    uint32_t t = ring->tail;
    if (t >= h) return t - h;
    return POCKET_RING_CAPACITY - (h - t);
}

// Userspace push: write Pocket to tail, advance tail.
// Returns true on success, false if ring is full.
static inline bool pocket_ring_push(PocketRing* ring, const Pocket* pocket)
{
    if (pocket_ring_is_full(ring)) return false;

    uint32_t idx = ring->tail;
    ring->slots[idx] = *pocket;
    __sync_synchronize();  // ensure data written before tail advance
    ring->tail = (idx + 1) % POCKET_RING_CAPACITY;
    return true;
}

// Kernel peek: get pointer to current head Pocket without advancing.
// Returns NULL if ring is empty.
static inline Pocket* pocket_ring_peek(PocketRing* ring)
{
    if (pocket_ring_is_empty(ring)) return NULL;
    return &ring->slots[ring->head];
}

// Kernel pop: advance head after processing.
static inline void pocket_ring_pop(PocketRing* ring)
{
    if (!pocket_ring_is_empty(ring)) {
        ring->head = (ring->head + 1) % POCKET_RING_CAPACITY;
    }
}

#endif // POCKET_RING_H
