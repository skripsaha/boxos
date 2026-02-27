#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include "ktypes.h"
#include "events.h"
#include "atomics.h"
#include "kernel_config.h"
#include "klib.h"  // For spinlock_t

#define RING_SIZE      CONFIG_EVENT_RING_SIZE
#define CACHE_LINE_PAD (CONFIG_CACHE_LINE_SIZE / sizeof(uint64_t))

// Event priority classification
typedef enum {
    EVENT_PRIORITY_SYSTEM = 0,   // I/O completion, scheduler (never blocks)
    EVENT_PRIORITY_USER   = 1,   // Userspace notify() calls (may block)
} event_priority_t;

// Push result codes
typedef enum {
    EVENT_PUSH_OK = 0,               // Success
    EVENT_PUSH_FULL_BLOCKED,         // User event blocked (wait queue)
    EVENT_PUSH_SYSTEM_OK,            // System event pushed (reserved slot)
} event_push_result_t;

// Statistics (global, atomic counters)
typedef struct {
    volatile uint64_t total_pushed;        // Total events submitted
    volatile uint64_t total_blocked;       // Processes blocked count
    volatile uint64_t peak_utilization;    // Max ring count observed
    volatile uint64_t overflow_events;     // Overflow incidents
} event_ring_stats_t;

// BUG #4 FIX: Replace lock-free CAS with spinlock to prevent ABA problem
// The previous lock-free implementation was susceptible to ABA on multi-core systems:
// - Thread A reads head=5, gets preempted
// - Thread B pushes and pops multiple items, head wraps back to 5
// - Thread A resumes, CAS succeeds with stale value
// Spinlock approach is simpler, safer, and sufficient for this use case.
typedef struct __aligned(CONFIG_CACHE_LINE_SIZE) {
    volatile uint64_t head;
    uint8_t _pad1[CONFIG_CACHE_LINE_SIZE - sizeof(uint64_t)];

    volatile uint64_t tail;
    uint8_t _pad2[CONFIG_CACHE_LINE_SIZE - sizeof(uint64_t)];

    spinlock_t lock;

    // Track user events separately (for quota enforcement)
    volatile uint64_t user_count;
    uint8_t _pad3[CONFIG_CACHE_LINE_SIZE - sizeof(spinlock_t) - sizeof(uint64_t)];

    Event entries[RING_SIZE];
} EventRingBuffer;

// NOTE: ResponseRingBuffer removed - BoxOS uses result_page.h instead

static inline void event_ring_init(EventRingBuffer* ring) {
    if (!ring) return;
    ring->head = 0;
    ring->tail = 0;
    ring->user_count = 0;
    spinlock_init(&ring->lock);
    memset(ring->entries, 0, sizeof(ring->entries));
}

static inline bool event_ring_push(EventRingBuffer* ring, const Event* event) {
    if (!ring || !event) return false;

    spin_lock(&ring->lock);

    uint64_t head = ring->head;
    uint64_t tail = ring->tail;

    // Check if ring is full (use absolute indices to avoid wraparound issues)
    if ((head - tail) >= RING_SIZE) {
        spin_unlock(&ring->lock);
        return false;
    }

    // Copy event to ring buffer slot
    size_t index = head & (RING_SIZE - 1);
    memcpy(&ring->entries[index], event, sizeof(Event));

    // Advance head
    ring->head = head + 1;

    spin_unlock(&ring->lock);
    return true;
}

static inline bool event_ring_pop(EventRingBuffer* ring, Event* event) {
    if (!ring || !event) return false;

    spin_lock(&ring->lock);

    uint64_t head = ring->head;
    uint64_t tail = ring->tail;

    // Check if ring is empty
    if (head == tail) {
        spin_unlock(&ring->lock);
        return false;
    }

    // Copy event from ring buffer slot
    size_t index = tail & (RING_SIZE - 1);
    memcpy(event, &ring->entries[index], sizeof(Event));

    // Decrement user_count if this was a user event (pid != 0)
    // CRITICAL FIX: Add bounds check to prevent underflow from system events
    if (event->pid != 0 && ring->user_count > 0) {
        ring->user_count--;
    }

    // Advance tail
    ring->tail = tail + 1;

    spin_unlock(&ring->lock);
    return true;
}

static inline bool event_ring_is_empty(const EventRingBuffer* ring) {
    if (!ring) return true;

    // No lock needed for simple read (race is acceptable here)
    return ring->head == ring->tail;
}

static inline size_t event_ring_count(const EventRingBuffer* ring) {
    if (!ring) return 0;

    // Snapshot head and tail (may be slightly stale, but safe)
    uint64_t head = ring->head;
    uint64_t tail = ring->tail;
    return (size_t)(head - tail);
}

static inline size_t event_ring_available(const EventRingBuffer* ring) {
    if (!ring) return 0;
    size_t count = event_ring_count(ring);
    if (count >= RING_SIZE) return 0;
    return RING_SIZE - count - 1;  // Reserve 1 slot to distinguish full from empty
}

static inline const Event* event_ring_peek(const EventRingBuffer* ring) {
    if (!ring) return NULL;

    // WARNING: peek is inherently racy without holding lock
    // Use only for debugging/monitoring
    uint64_t head = ring->head;
    uint64_t tail = ring->tail;
    if (head == tail) return NULL;

    size_t index = tail & (RING_SIZE - 1);
    return &ring->entries[index];
}

// Global statistics (atomic updates)
static event_ring_stats_t g_event_ring_stats = {0};

static inline event_push_result_t event_ring_push_priority(
    EventRingBuffer* ring,
    const Event* event,
    event_priority_t priority
) {
    if (!ring || !event) return EVENT_PUSH_FULL_BLOCKED;

    spin_lock(&ring->lock);

    uint64_t head = ring->head;
    uint64_t tail = ring->tail;
    uint64_t count = head - tail;

    // Update statistics
    atomic_fetch_add_u64(&g_event_ring_stats.total_pushed, 1);
    if (count > g_event_ring_stats.peak_utilization) {
        g_event_ring_stats.peak_utilization = count;
    }

    // SYSTEM EVENT: Always succeeds (uses reserved slots)
    if (priority == EVENT_PRIORITY_SYSTEM) {
        if (count >= RING_SIZE) {
            // Should NEVER happen with reservation - critical error
            debug_printf("[EVENTRING] CRITICAL: Ring overflow even for system event!\n");
            spin_unlock(&ring->lock);
            return EVENT_PUSH_FULL_BLOCKED;
        }

        size_t index = head & (RING_SIZE - 1);
        memcpy(&ring->entries[index], event, sizeof(Event));
        ring->head = head + 1;

        spin_unlock(&ring->lock);
        return EVENT_PUSH_SYSTEM_OK;
    }

    // USER EVENT: Check user quota (1792 max)
    uint64_t user_count = ring->user_count;

    if (user_count >= CONFIG_EVENT_RING_USER_MAX) {
        // User quota exceeded - block process
        atomic_fetch_add_u64(&g_event_ring_stats.total_blocked, 1);
        atomic_fetch_add_u64(&g_event_ring_stats.overflow_events, 1);

        spin_unlock(&ring->lock);
        return EVENT_PUSH_FULL_BLOCKED;
    }

    // Push user event
    size_t index = head & (RING_SIZE - 1);
    memcpy(&ring->entries[index], event, sizeof(Event));
    ring->head = head + 1;
    ring->user_count++;

    spin_unlock(&ring->lock);
    return EVENT_PUSH_OK;
}

static inline void event_ring_get_stats(event_ring_stats_t* out) {
    if (!out) return;
    out->total_pushed = g_event_ring_stats.total_pushed;
    out->total_blocked = g_event_ring_stats.total_blocked;
    out->peak_utilization = g_event_ring_stats.peak_utilization;
    out->overflow_events = g_event_ring_stats.overflow_events;
}

#endif // RINGBUFFER_H
