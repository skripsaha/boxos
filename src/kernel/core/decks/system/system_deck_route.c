#include "system_deck_route.h"
#include "guide.h"
#include "process.h"
#include "klib.h"
#include "vmm.h"
#include "result_page.h"
#include "error.h"
#include "listen_table.h"

static bool tag_matches_process(process_t* proc, const char* route_tag) {
    if (!proc || !route_tag || route_tag[0] == '\0') {
        return false;
    }

    char tags_snapshot[PROCESS_TAG_SIZE];
    process_snapshot_tags(proc, tags_snapshot, sizeof(tags_snapshot));

    const char* pos = tags_snapshot;
    while (*pos) {
        const char* comma = strchr(pos, ',');
        size_t tag_len = comma ? (size_t)(comma - pos) : strlen(pos);

        char current_tag[PROCESS_TAG_SIZE];
        size_t copy_len = tag_len < PROCESS_TAG_SIZE - 1 ? tag_len : PROCESS_TAG_SIZE - 1;
        memcpy(current_tag, pos, copy_len);
        current_tag[copy_len] = '\0';

        if (tag_match(route_tag, current_tag)) {
            return true;
        }

        if (!comma) break;
        pos = comma + 1;
    }

    return false;
}

int system_deck_route(Event* event) {
    if (!event) return -1;

    if (event->route_target == 0) {
        event_set_error(event, ERR_INVALID_ARGUMENT, event->current_prefix_idx);
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    if (event->route_target == event->pid) {
        event_set_error(event, ERR_ROUTE_SELF, event->current_prefix_idx);
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    process_t* target = process_find(event->route_target);
    if (!target) {
        event_set_error(event, ERR_PROCESS_NOT_FOUND, event->current_prefix_idx);
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    process_state_t target_state = process_get_state(target);
    if (target_state == PROC_CRASHED || target_state == PROC_DONE) {
        event_set_error(event, ERR_PROCESS_NOT_FOUND, event->current_prefix_idx);
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    uint64_t result_phys = target->result_page_phys;
    if (result_phys != 0) {
        result_page_t* rp = (result_page_t*)vmm_phys_to_virt(result_phys);
        if (rp) {
            uint32_t head;
            memcpy(&head, (const void*)&rp->ring.head, sizeof(uint32_t));
            uint32_t tail = rp->ring.tail;
            if (((tail + 1) % RESULT_RING_SIZE) == head) {
                event_set_error(event, ERR_ROUTE_TARGET_FULL, event->current_prefix_idx);
                event->state = EVENT_STATE_ERROR;
                return -1;
            }
        }
    }

    uint32_t original_sender = event->pid;

    extern EventRingBuffer* kernel_event_ring;
    Event clone;
    memcpy(&clone, event, sizeof(Event));
    clone.pid = event->route_target;
    clone.sender_pid = original_sender;
    clone.route_flags = ROUTE_SOURCE_PROCESS;
    clone.event_id = guide_alloc_event_id();
    clone.prefixes[0] = 0x0000;
    clone.current_prefix_idx = 0;
    clone.prefix_count = 1;
    clone.state = EVENT_STATE_PROCESSING;

    error_t push_err = event_ring_push(kernel_event_ring, &clone);
    if (IS_ERROR(push_err)) {
        event_ring_grow(kernel_event_ring);
        push_err = event_ring_push(kernel_event_ring, &clone);
        if (IS_ERROR(push_err)) {
            event_set_error(event, ERR_EVENT_RING_FULL, event->current_prefix_idx);
            event->state = EVENT_STATE_ERROR;
            return -1;
        }
    }

    event->sender_pid = original_sender;

    debug_printf("[ROUTE] PID %u -> PID %u (direct, clone-based)\n", original_sender, event->route_target);

    event->state = EVENT_STATE_PROCESSING;
    return 0;
}

int system_deck_route_tag(Event* event) {
    if (!event) return -1;

    if (event->route_tag[0] == '\0') {
        event_set_error(event, ERR_INVALID_ARGUMENT, event->current_prefix_idx);
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    uint32_t targets[MAX_ROUTE_TAG_TARGETS];
    uint32_t target_count = 0;

    extern process_t* process_get_first(void);
    process_t* proc = process_get_first();
    while (proc && target_count < MAX_ROUTE_TAG_TARGETS) {
        process_state_t pstate = process_get_state(proc);
        if ((pstate == PROC_WORKING || pstate == PROC_WAITING) &&
            proc->pid != event->pid) {
            if (tag_matches_process(proc, event->route_tag)) {
                targets[target_count++] = proc->pid;
            }
        }
        proc = proc->next;
    }

    if (target_count == 0) {
        event_set_error(event, ERR_ROUTE_NO_SUBSCRIBERS, event->current_prefix_idx);
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    uint32_t original_sender = event->pid;

    extern EventRingBuffer* kernel_event_ring;

    uint32_t delivered = 0;
    for (uint32_t i = 0; i < target_count; i++) {
        Event clone;
        memcpy(&clone, event, sizeof(Event));
        clone.pid = targets[i];
        clone.sender_pid = original_sender;
        clone.route_flags = ROUTE_SOURCE_PROCESS;
        clone.event_id = guide_alloc_event_id();

        clone.prefixes[0] = 0x0000;
        clone.current_prefix_idx = 0;
        clone.prefix_count = 1;
        clone.state = EVENT_STATE_PROCESSING;

        error_t push_err = event_ring_push(kernel_event_ring, &clone);
        if (IS_ERROR(push_err)) {
            event_ring_grow(kernel_event_ring);
            push_err = event_ring_push(kernel_event_ring, &clone);
            if (IS_ERROR(push_err)) {
                debug_printf("[ROUTE_TAG] WARNING: Failed to deliver to PID %u: ring full\n",
                             targets[i]);
                continue;
            }
        }
        delivered++;
    }

    if (delivered == 0) {
        debug_printf("[ROUTE_TAG] ERROR: All %u deliveries failed for '%s'\n",
                     target_count, event->route_tag);
        event_set_error(event, ERR_EVENT_RING_FULL, event->current_prefix_idx);
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    debug_printf("[ROUTE_TAG] PID %u -> %u/%u targets matching '%s'\n",
                 original_sender, delivered, target_count, event->route_tag);

    event->state = EVENT_STATE_PROCESSING;
    return 0;
}

int system_deck_listen(Event* event) {
    if (!event) return -1;

    uint8_t source_type = event->data[0];
    uint8_t flags = event->data[1];

    int result = listen_table_add(event->pid, source_type, flags);

    if (result < 0) {
        error_t err;
        if (result == -ERR_LISTEN_ALREADY) {
            err = ERR_LISTEN_ALREADY;
        } else if (result == -ERR_LISTEN_TABLE_FULL) {
            err = ERR_LISTEN_TABLE_FULL;
        } else {
            err = ERR_INTERNAL;
        }
        event_set_error(event, err, event->current_prefix_idx);
        event->state = EVENT_STATE_ERROR;
        memset(event->data, 0, EVENT_DATA_SIZE);
        *((uint32_t*)event->data) = (uint32_t)err;
        return -1;
    }

    event->state = EVENT_STATE_COMPLETED;
    memset(event->data, 0, EVENT_DATA_SIZE);
    *((uint32_t*)event->data) = 0;
    return 0;
}
