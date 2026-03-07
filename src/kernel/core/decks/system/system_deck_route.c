#include "system_deck_route.h"
#include "guide.h"
#include "process.h"
#include "klib.h"
#include "vmm.h"
#include "error.h"
#include "listen_table.h"
#include "pocket.h"
#include "pocket_ring.h"
#include "ready_queue.h"
#include "result_ring.h"

#define MAX_ROUTE_TAG_TARGETS 16

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

static bool deliver_pocket_to_process(process_t* target, Pocket* pocket, uint32_t sender_pid) {
    if (!target || !pocket) {
        return false;
    }

    if (!target->result_ring_phys) {
        return false;
    }

    ResultRing* rring = (ResultRing*)vmm_phys_to_virt(target->result_ring_phys);
    if (!rring) {
        return false;
    }

    // Copy data from sender's heap to target's heap at the same virtual address
    if (pocket->data_length > 0 && pocket->data_addr != 0) {
        process_t* sender = process_find(pocket->pid);
        if (sender) {
            void* src = vmm_translate_user_addr(sender->cabin, pocket->data_addr, pocket->data_length);
            void* dst = vmm_translate_user_addr(target->cabin, pocket->data_addr, pocket->data_length);
            if (src && dst) {
                memcpy(dst, src, pocket->data_length);
            }
        }
    }

    // Push Result directly to target's ResultRing
    Result result;
    result.error_code = OK;
    result.data_length = pocket->data_length;
    result.data_addr = pocket->data_addr;
    result.sender_pid = sender_pid;
    result._reserved = 0;

    if (!result_ring_push(rring, &result)) {
        return false;
    }

    // Wake target if it's waiting for a message
    if (process_get_state(target) == PROC_WAITING) {
        process_set_state(target, PROC_WORKING);
    }

    return true;
}

int system_deck_route(Pocket* pocket) {
    if (!pocket) return -1;

    if (pocket->target_pid == 0) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    if (pocket->target_pid == pocket->pid) {
        pocket->error_code = ERR_ROUTE_SELF;
        return -1;
    }

    process_t* target = process_find(pocket->target_pid);
    if (!target) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    process_state_t target_state = process_get_state(target);
    if (target_state == PROC_CRASHED || target_state == PROC_DONE) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    uint32_t sender_pid = pocket->pid;

    if (!deliver_pocket_to_process(target, pocket, sender_pid)) {
        pocket->error_code = ERR_ROUTE_TARGET_FULL;
        return -1;
    }

    debug_printf("[ROUTE] PID %u -> PID %u (direct)\n", sender_pid, pocket->target_pid);

    // Clear target_pid so execution_deck sets sender_pid=0 in the originator's Result.
    // The routed pocket already has target_pid=sender_pid for the receiver.
    pocket->target_pid = 0;
    pocket->error_code = 0;
    return 0;
}

int system_deck_route_tag(Pocket* pocket) {
    if (!pocket) return -1;

    if (pocket->route_tag[0] == '\0') {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint32_t targets[MAX_ROUTE_TAG_TARGETS];
    uint32_t target_count = 0;

    process_t* proc = process_get_first();
    while (proc && target_count < MAX_ROUTE_TAG_TARGETS) {
        process_state_t pstate = process_get_state(proc);
        if ((pstate == PROC_WORKING || pstate == PROC_WAITING) &&
            proc->pid != pocket->pid) {
            if (tag_matches_process(proc, pocket->route_tag)) {
                targets[target_count++] = proc->pid;
            }
        }
        proc = proc->next;
    }

    if (target_count == 0) {
        pocket->error_code = ERR_ROUTE_NO_SUBSCRIBERS;
        return -1;
    }

    uint32_t sender_pid = pocket->pid;
    uint32_t delivered  = 0;

    for (uint32_t i = 0; i < target_count; i++) {
        process_t* t = process_find(targets[i]);
        if (!t) {
            continue;
        }
        if (deliver_pocket_to_process(t, pocket, sender_pid)) {
            delivered++;
        } else {
            debug_printf("[ROUTE_TAG] WARNING: Failed to deliver to PID %u: ring full\n",
                         targets[i]);
        }
    }

    if (delivered == 0) {
        debug_printf("[ROUTE_TAG] ERROR: All %u deliveries failed for '%s'\n",
                     target_count, pocket->route_tag);
        pocket->error_code = ERR_ROUTE_TARGET_FULL;
        return -1;
    }

    debug_printf("[ROUTE_TAG] PID %u -> %u/%u targets matching '%s'\n",
                 sender_pid, delivered, target_count, pocket->route_tag);

    pocket->target_pid = 0;
    pocket->error_code = 0;
    return 0;
}

int system_deck_listen(Pocket* pocket) {
    if (!pocket) return -1;

    process_t* proc = process_find(pocket->pid);
    if (!proc) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    void* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
    if (!data || pocket->data_length < 2) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint8_t* bytes      = (uint8_t*)data;
    uint8_t source_type = bytes[0];
    uint8_t flags       = bytes[1];

    int result = listen_table_add(pocket->pid, source_type, flags);

    if (result < 0) {
        error_t err;
        if (result == -ERR_LISTEN_ALREADY) {
            err = ERR_LISTEN_ALREADY;
        } else if (result == -ERR_LISTEN_TABLE_FULL) {
            err = ERR_LISTEN_TABLE_FULL;
        } else {
            err = ERR_INTERNAL;
        }
        pocket->error_code = (uint32_t)err;

        uint32_t err_out = (uint32_t)err;
        memcpy(data, &err_out, sizeof(uint32_t));
        return -1;
    }

    pocket->error_code = 0;
    uint32_t ok = 0;
    memcpy(data, &ok, sizeof(uint32_t));
    return 0;
}
