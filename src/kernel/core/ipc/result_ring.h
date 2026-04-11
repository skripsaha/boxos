#ifndef RESULT_RING_H
#define RESULT_RING_H

#include "ktypes.h"
#include "result.h"
#include "boxos_sizes.h"

// ResultRing: per-process SPSC ring buffer for syscall Results.
// Lives at CABIN_RESULT_RING_ADDR (0x3000), 9 pages (36KB).
// Kernel is the producer (writes Results, advances tail).
// Userspace is the consumer (reads Results, advances head).

typedef struct __packed {
    volatile uint32_t head;                     // userspace advances after reading
    volatile uint32_t tail;                     // kernel advances after writing
    Result slots[RESULT_RING_CAPACITY];         // 1535 * 24 = 36840 bytes
} ResultRing;

// 8 + 36840 = 36848 bytes, fits in 36864 (9 pages)
_Static_assert(sizeof(ResultRing) <= 36864, "ResultRing must fit in 9 pages (36KB)");

static inline bool result_ring_is_empty(const ResultRing* ring)
{
    return ring->head == ring->tail;
}

static inline bool result_ring_is_full(const ResultRing* ring)
{
    return ((ring->tail + 1) % RESULT_RING_CAPACITY) == ring->head;
}

static inline uint32_t result_ring_count(const ResultRing* ring)
{
    uint32_t h = ring->head;
    uint32_t t = ring->tail;
    if (t >= h) return t - h;
    return RESULT_RING_CAPACITY - (h - t);
}

// Kernel push: write Result to tail, advance tail.
// Returns true on success, false if ring is full.
static inline bool result_ring_push(ResultRing* ring, const Result* result)
{
    if (result_ring_is_full(ring)) return false;

    uint32_t idx = ring->tail;
    ring->slots[idx] = *result;
    __sync_synchronize();  // ensure data written before tail advance
    ring->tail = (idx + 1) % RESULT_RING_CAPACITY;
    return true;
}

// Userspace pop: read Result from head, advance head.
// Returns true on success, false if ring is empty.
static inline bool result_ring_pop(ResultRing* ring, Result* out)
{
    if (result_ring_is_empty(ring)) return false;

    uint32_t idx = ring->head;
    *out = ring->slots[idx];
    __sync_synchronize();  // ensure data read before head advance
    ring->head = (idx + 1) % RESULT_RING_CAPACITY;
    return true;
}

#endif // RESULT_RING_H
