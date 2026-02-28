#include "event_ring.h"

EventRingBuffer* event_ring_create(size_t initial_capacity) {
    if (initial_capacity != EVENT_RING_MIN_CAPACITY &&
        initial_capacity != 1024 &&
        initial_capacity != 2048 &&
        initial_capacity != EVENT_RING_MAX_CAPACITY) {
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

boxos_error_t event_ring_push(EventRingBuffer* ring, Event* event) {
    if (!ring || !event) {
        return BOXOS_ERR_INVALID_ARGUMENT;
    }

    // Validate Event magic number for corruption detection
    if (event->magic != EVENT_MAGIC) {
        return BOXOS_ERR_INVALID_ARGUMENT;
    }

    // TEMPORARY (Phase 1.3): Current Event structure does not have extended storage.
    // Phase 2 will add extended_prefixes/extended_data pointers with EVENT_FLAG_* flags.
    // This check is a placeholder to document the limitation - any Event with extended
    // storage will be rejected until Phase 2 implements proper deep copy semantics.
    // TODO(Phase 2): When Event structure gains flags field, uncomment this check:
    // #define EVENT_FLAG_EXTENDED_PREFIXES (1 << 0)
    // #define EVENT_FLAG_EXTENDED_DATA     (1 << 1)
    // if (event->flags & (EVENT_FLAG_EXTENDED_PREFIXES | EVENT_FLAG_EXTENDED_DATA)) {
    //     return BOXOS_ERR_NOT_IMPLEMENTED;
    // }

    spin_lock(&ring->lock);

    if (ring->user_count >= ring->capacity) {
        spin_unlock(&ring->lock);
        return BOXOS_ERR_EVENT_RING_FULL;
    }

    size_t write_pos = ring->tail % ring->capacity;

    memcpy(&ring->entries[write_pos], event, sizeof(Event));

    // NOTE: On single-core (v1.0), spinlock disables interrupts, making these
    //       sequential updates atomic. Multi-core will require CAS operations.
    ring->tail++;
    ring->user_count++;

    // Auto-grow: Check if utilization exceeds 80% threshold
    if (event_ring_should_grow(ring)) {
        spin_unlock(&ring->lock);
        boxos_error_t grow_err = event_ring_grow(ring);
        if (BOXOS_IS_ERROR(grow_err)) {
            debug_printf("[EVENTRING] Auto-grow failed: %s\n", boxos_error_string(grow_err));
        }
        return BOXOS_OK;
    }

    spin_unlock(&ring->lock);

    return BOXOS_OK;
}

boxos_error_t event_ring_pop(EventRingBuffer* ring, Event* out_event) {
    if (!ring || !out_event) {
        return BOXOS_ERR_INVALID_ARGUMENT;
    }

    spin_lock(&ring->lock);

    if (ring->user_count == 0) {
        spin_unlock(&ring->lock);
        return BOXOS_ERR_WOULD_BLOCK;
    }

    size_t read_pos = ring->head % ring->capacity;

    memcpy(out_event, &ring->entries[read_pos], sizeof(Event));

    // Clear ring entry for security and hygiene
    memset(&ring->entries[read_pos], 0, sizeof(Event));

    // NOTE: On single-core (v1.0), spinlock disables interrupts, making these
    //       sequential updates atomic. Multi-core will require CAS operations.
    ring->head++;
    ring->user_count--;

    spin_unlock(&ring->lock);

    return BOXOS_OK;
}

boxos_error_t event_ring_grow(EventRingBuffer* ring) {
    if (!ring) {
        return BOXOS_ERR_INVALID_ARGUMENT;
    }

    if (ring->capacity >= EVENT_RING_MAX_CAPACITY) {
        return BOXOS_ERR_BUFFER_LIMIT_EXCEEDED;
    }

    size_t new_capacity = ring->capacity * 2;

    size_t max_capacity = ((size_t)-1) / sizeof(Event);
    if (new_capacity > max_capacity) {
        return BOXOS_ERR_NO_MEMORY;
    }

    Event* new_entries = kmalloc(new_capacity * sizeof(Event));
    if (!new_entries) {
        return BOXOS_ERR_NO_MEMORY;
    }

    spin_lock(&ring->lock);

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

    return BOXOS_OK;
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

    // Validate Event magic number
    if (event->magic != EVENT_MAGIC) {
        return EVENT_PUSH_FULL_BLOCKED;
    }

    spin_lock(&ring->lock);

    size_t current_count = ring->user_count;
    size_t capacity = ring->capacity;

    // SYSTEM PRIORITY: Always succeeds (critical path for I/O completion)
    if (priority == EVENT_PRIORITY_SYSTEM) {
        // System events can use full capacity (no user quota enforcement)
        if (current_count >= capacity) {
            spin_unlock(&ring->lock);
            debug_printf("[EVENTRING] CRITICAL: Ring overflow even for system event\n");
            return EVENT_PUSH_FULL_BLOCKED;
        }

        size_t write_pos = ring->tail % capacity;
        memcpy(&ring->entries[write_pos], event, sizeof(Event));
        ring->tail++;
        ring->user_count++;

        // Auto-grow after system event push
        if (event_ring_should_grow(ring)) {
            spin_unlock(&ring->lock);
            boxos_error_t grow_err = event_ring_grow(ring);
            if (BOXOS_IS_ERROR(grow_err)) {
                debug_printf("[EVENTRING] Auto-grow failed after system push: %s\n",
                           boxos_error_string(grow_err));
            }
            return EVENT_PUSH_SYSTEM_OK;
        }

        spin_unlock(&ring->lock);
        return EVENT_PUSH_SYSTEM_OK;
    }

    // USER PRIORITY: Subject to capacity limits
    if (current_count >= capacity) {
        spin_unlock(&ring->lock);
        return EVENT_PUSH_FULL_BLOCKED;
    }

    size_t write_pos = ring->tail % capacity;
    memcpy(&ring->entries[write_pos], event, sizeof(Event));
    ring->tail++;
    ring->user_count++;

    // Auto-grow after user event push
    if (event_ring_should_grow(ring)) {
        spin_unlock(&ring->lock);
        boxos_error_t grow_err = event_ring_grow(ring);
        if (BOXOS_IS_ERROR(grow_err)) {
            debug_printf("[EVENTRING] Auto-grow failed after user push: %s\n",
                       boxos_error_string(grow_err));
        }
        return EVENT_PUSH_OK;
    }

    spin_unlock(&ring->lock);
    return EVENT_PUSH_OK;
}
