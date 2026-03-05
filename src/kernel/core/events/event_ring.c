#include "event_ring.h"

EventRingBuffer* event_ring_create(size_t initial_capacity) {
    // Must be power-of-2 in [MIN, MAX]; clamp otherwise
    if (initial_capacity < EVENT_RING_MIN_CAPACITY ||
        initial_capacity > EVENT_RING_MAX_CAPACITY ||
        (initial_capacity & (initial_capacity - 1)) != 0) {
        initial_capacity = EVENT_RING_MIN_CAPACITY;
    }

    EventRingBuffer* ring = kmalloc(sizeof(EventRingBuffer));
    if (!ring) {
        return NULL;
    }

    size_t max_capacity = ((size_t)-1) / sizeof(Event);
    if (initial_capacity > max_capacity) {
        kfree(ring);
        return NULL;
    }

    ring->entries = kmalloc(initial_capacity * sizeof(Event));
    if (!ring->entries) {
        kfree(ring);
        return NULL;
    }

    ring->head = 0;
    ring->tail = 0;
    ring->capacity = initial_capacity;
    ring->user_count = 0;
    ring->generation = 0;

    spinlock_init(&ring->lock);

    memset(ring->entries, 0, initial_capacity * sizeof(Event));

    return ring;
}

void event_ring_destroy(EventRingBuffer* ring) {
    if (!ring) {
        return;
    }

    if (ring->entries) {
        kfree(ring->entries);
        ring->entries = NULL;
    }

    kfree(ring);
}

error_t event_ring_push(EventRingBuffer* ring, Event* event) {
    if (!ring || !event) {
        return ERR_INVALID_ARGUMENT;
    }

    if (event->magic != EVENT_MAGIC) {
        return ERR_INVALID_ARGUMENT;
    }

    spin_lock(&ring->lock);

    if (ring->user_count >= ring->capacity) {
        spin_unlock(&ring->lock);
        return ERR_EVENT_RING_FULL;
    }

    size_t write_pos = ring->tail % ring->capacity;
    memcpy(&ring->entries[write_pos], event, sizeof(Event));
    ring->tail++;
    ring->user_count++;

    bool needs_grow = event_ring_should_grow(ring);

    spin_unlock(&ring->lock);

    // auto-grow outside lock so kmalloc doesn't run under spinlock
    if (needs_grow) {
        error_t grow_err = event_ring_grow(ring);
        if (IS_ERROR(grow_err)) {
            debug_printf("[EVENTRING] Auto-grow failed: %s\n", error_string(grow_err));
        }
    }

    return OK;
}

error_t event_ring_pop(EventRingBuffer* ring, Event* out_event) {
    if (!ring || !out_event) {
        return ERR_INVALID_ARGUMENT;
    }

    spin_lock(&ring->lock);

    if (ring->user_count == 0) {
        spin_unlock(&ring->lock);
        return ERR_WOULD_BLOCK;
    }

    size_t read_pos = ring->head % ring->capacity;

    memcpy(out_event, &ring->entries[read_pos], sizeof(Event));

    memset(&ring->entries[read_pos], 0, sizeof(Event));
    ring->head++;
    ring->user_count--;

    spin_unlock(&ring->lock);

    return OK;
}

error_t event_ring_grow(EventRingBuffer* ring) {
    if (!ring) {
        return ERR_INVALID_ARGUMENT;
    }

    if (ring->capacity >= EVENT_RING_MAX_CAPACITY) {
        return ERR_BUFFER_LIMIT_EXCEEDED;
    }

    size_t new_capacity = ring->capacity * 2;

    size_t max_capacity = ((size_t)-1) / sizeof(Event);
    if (new_capacity > max_capacity) {
        return ERR_NO_MEMORY;
    }

    // allocate outside lock; kmalloc may block or be slow
    Event* new_entries = kmalloc(new_capacity * sizeof(Event));
    if (!new_entries) {
        return ERR_NO_MEMORY;
    }

    spin_lock(&ring->lock);

    // re-check under lock: another caller may have already grown
    if (!event_ring_should_grow(ring) || ring->capacity >= EVENT_RING_MAX_CAPACITY) {
        spin_unlock(&ring->lock);
        kfree(new_entries);
        return OK;
    }

    if (new_capacity <= ring->capacity) {
        spin_unlock(&ring->lock);
        kfree(new_entries);
        return OK;
    }

    size_t count = ring->user_count;
    uint64_t read_pos = ring->head;

    for (size_t i = 0; i < count; i++) {
        size_t old_idx = read_pos % ring->capacity;
        memcpy(&new_entries[i], &ring->entries[old_idx], sizeof(Event));
        read_pos++;
    }

    Event* old_entries = ring->entries;
    ring->entries = new_entries;

    ring->capacity = new_capacity;
    ring->head = 0;
    ring->tail = count;
    ring->generation++;

    spin_unlock(&ring->lock);

    kfree(old_entries);

    return OK;
}

bool event_ring_should_grow(const EventRingBuffer* ring) {
    if (!ring) {
        return false;
    }

    if (ring->capacity >= EVENT_RING_MAX_CAPACITY) {
        return false;
    }

    size_t used = ring->user_count;
    size_t capacity = ring->capacity;

    if (capacity == 0) {
        return false;
    }

    size_t utilization_percent = (used * 100) / capacity;

    return utilization_percent >= EVENT_RING_GROWTH_THRESHOLD;
}

size_t event_ring_capacity(const EventRingBuffer* ring) {
    if (!ring) {
        return 0;
    }
    return ring->capacity;
}

size_t event_ring_count(const EventRingBuffer* ring) {
    if (!ring) {
        return 0;
    }
    return ring->user_count;
}

event_push_result_t event_ring_push_priority(
    EventRingBuffer* ring,
    Event* event,
    event_priority_t priority
) {
    if (!ring || !event) {
        return EVENT_PUSH_FULL_BLOCKED;
    }

    if (event->magic != EVENT_MAGIC) {
        return EVENT_PUSH_FULL_BLOCKED;
    }

    spin_lock(&ring->lock);

    size_t current_count = ring->user_count;
    size_t capacity = ring->capacity;

    // SYSTEM priority uses full capacity; USER priority is subject to limits
    if (priority == EVENT_PRIORITY_SYSTEM) {
        if (current_count >= capacity) {
            spin_unlock(&ring->lock);
            debug_printf("[EVENTRING] CRITICAL: Ring overflow even for system event\n");
            return EVENT_PUSH_FULL_BLOCKED;
        }

        size_t write_pos = ring->tail % capacity;
        memcpy(&ring->entries[write_pos], event, sizeof(Event));
        ring->tail++;
        ring->user_count++;

        bool needs_grow = event_ring_should_grow(ring);
        spin_unlock(&ring->lock);

        if (needs_grow) {
            error_t grow_err = event_ring_grow(ring);
            if (IS_ERROR(grow_err)) {
                debug_printf("[EVENTRING] Auto-grow failed after system push: %s\n",
                           error_string(grow_err));
            }
        }
        return EVENT_PUSH_SYSTEM_OK;
    }

    if (current_count >= capacity) {
        spin_unlock(&ring->lock);
        return EVENT_PUSH_FULL_BLOCKED;
    }

    size_t write_pos = ring->tail % capacity;
    memcpy(&ring->entries[write_pos], event, sizeof(Event));
    ring->tail++;
    ring->user_count++;

    bool needs_grow = event_ring_should_grow(ring);
    spin_unlock(&ring->lock);

    if (needs_grow) {
        error_t grow_err = event_ring_grow(ring);
        if (IS_ERROR(grow_err)) {
            debug_printf("[EVENTRING] Auto-grow failed after user push: %s\n",
                       error_string(grow_err));
        }
    }
    return EVENT_PUSH_OK;
}
