#ifndef EVENT_RING_DYNAMIC_H
#define EVENT_RING_DYNAMIC_H

#include "events.h"
#include "error.h"
#include "../../lib/kernel/ktypes.h"
#include "../../lib/kernel/klib.h"

#define EVENT_RING_MIN_CAPACITY 512
#define EVENT_RING_MAX_CAPACITY 4096
#define EVENT_RING_GROWTH_THRESHOLD 80

/**
 * Dynamic EventRingBuffer with automatic capacity expansion
 *
 * Architecture:
 * - Cache-aligned head/tail for optimal performance
 * - Dynamic entries array (grows 512 -> 1024 -> 2048 -> 4096)
 * - Generation counter for ABA prevention
 * - Adaptive growth at 80% utilization threshold
 */
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

/**
 * Create dynamic EventRing with specified initial capacity
 *
 * @param initial_capacity Initial ring capacity (512/1024/2048/4096)
 * @return Pointer to allocated ring, or NULL on failure
 */
EventRingBuffer* event_ring_create(size_t initial_capacity);

/**
 * Destroy EventRing and free all resources
 *
 * This function frees the entries array and the ring structure itself.
 * After calling this function, the ring pointer becomes invalid.
 *
 * @param ring Ring to destroy (can be NULL)
 *
 * @warning After calling this function, the caller MUST set their ring
 *          pointer to NULL to prevent use-after-free bugs. This function
 *          does NOT protect against double-free if called twice with the
 *          same pointer.
 *
 * @example
 *   EventRingBuffer* my_ring = event_ring_create(512);
 *   // ... use ring ...
 *   event_ring_destroy(my_ring);
 *   my_ring = NULL;  // REQUIRED to prevent use-after-free
 */
void event_ring_destroy(EventRingBuffer* ring);

/**
 * Push event into ring (auto-grows if needed)
 *
 * @param ring Target ring
 * @param event Event to push
 * @return BOXOS_OK on success, error code otherwise
 */
boxos_error_t event_ring_push(EventRingBuffer* ring, Event* event);

/**
 * Pop event from ring
 *
 * @param ring Source ring
 * @param out_event Destination for popped event
 * @return BOXOS_OK on success, error code otherwise
 */
boxos_error_t event_ring_pop(EventRingBuffer* ring, Event* out_event);

/**
 * Grow ring capacity using double-buffering
 *
 * @param ring Ring to grow
 * @return BOXOS_OK on success, error code otherwise
 */
boxos_error_t event_ring_grow(EventRingBuffer* ring);

/**
 * Check if ring should grow based on utilization
 *
 * @param ring Ring to check
 * @return true if growth recommended, false otherwise
 */
bool event_ring_should_grow(const EventRingBuffer* ring);

/**
 * Get current ring capacity
 *
 * @param ring Target ring
 * @return Current capacity in entries
 */
size_t event_ring_capacity(const EventRingBuffer* ring);

/**
 * Get current event count in ring
 *
 * @param ring Target ring
 * @return Number of events currently in ring
 */
size_t event_ring_count(const EventRingBuffer* ring);

/**
 * Event priority classification (compatibility with old API)
 */
typedef enum {
    EVENT_PRIORITY_SYSTEM = 0,
    EVENT_PRIORITY_USER   = 1,
} event_priority_t;

/**
 * Push result codes (compatibility with old API)
 */
typedef enum {
    EVENT_PUSH_OK = 0,
    EVENT_PUSH_FULL_BLOCKED,
    EVENT_PUSH_SYSTEM_OK,
} event_push_result_t;

/**
 * Push event with priority classification
 *
 * SYSTEM priority: Always succeeds, uses reserved capacity, never blocks.
 * USER priority: Subject to quota limits, may return FULL_BLOCKED.
 *
 * After successful push, automatically checks growth threshold and
 * grows ring if utilization exceeds 80%.
 *
 * @param ring Target ring
 * @param event Event to push
 * @param priority EVENT_PRIORITY_SYSTEM or EVENT_PRIORITY_USER
 * @return EVENT_PUSH_OK, EVENT_PUSH_FULL_BLOCKED, or EVENT_PUSH_SYSTEM_OK
 */
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
