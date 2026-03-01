#ifndef EVENT_RING_H
#define EVENT_RING_H

#include "events.h"
#include "error.h"
#include "ktypes.h"
#include "klib.h"

#define EVENT_RING_MIN_CAPACITY 512
#define EVENT_RING_MAX_CAPACITY 4096
#define EVENT_RING_GROWTH_THRESHOLD 80

// head/tail are cache-line aligned to prevent false sharing;
// generation counter guards against ABA on concurrent grow
typedef struct EventRingBuffer {
    volatile uint64_t head __attribute__((aligned(64)));
    uint8_t _pad1[56];

    volatile uint64_t tail __attribute__((aligned(64)));
    uint8_t _pad2[56];

    spinlock_t lock;
    size_t capacity;
    size_t user_count;
    uint8_t generation;
    uint8_t _pad3[47];

    Event* entries;
} EventRingBuffer;

EventRingBuffer* event_ring_create(size_t initial_capacity);
void event_ring_destroy(EventRingBuffer* ring);

error_t event_ring_push(EventRingBuffer* ring, Event* event);
error_t event_ring_pop(EventRingBuffer* ring, Event* out_event);
error_t event_ring_grow(EventRingBuffer* ring);
bool event_ring_should_grow(const EventRingBuffer* ring);

size_t event_ring_capacity(const EventRingBuffer* ring);
size_t event_ring_count(const EventRingBuffer* ring);

typedef enum {
    EVENT_PRIORITY_SYSTEM = 0,
    EVENT_PRIORITY_USER   = 1,
} event_priority_t;

typedef enum {
    EVENT_PUSH_OK = 0,
    EVENT_PUSH_FULL_BLOCKED,
    EVENT_PUSH_SYSTEM_OK,
} event_push_result_t;

// SYSTEM priority always succeeds and never blocks.
// USER priority may return FULL_BLOCKED when at capacity.
event_push_result_t event_ring_push_priority(
    EventRingBuffer* ring,
    Event* event,
    event_priority_t priority
);

static inline bool event_ring_is_empty(const EventRingBuffer* ring) {
    return event_ring_count(ring) == 0;
}

static inline size_t event_ring_available(const EventRingBuffer* ring) {
    if (!ring) {
        return 0;
    }
    size_t cap = event_ring_capacity(ring);
    size_t cnt = event_ring_count(ring);
    if (cnt >= cap) {
        return 0;
    }
    return cap - cnt;
}

#endif
