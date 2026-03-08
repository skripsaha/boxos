#include "system_deck_route.h"
#include "guide.h"
#include "process.h"
#include "klib.h"
#include "vmm.h"
#include "pmm.h"
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

// Allocate page(s) in target's buffer heap and copy data from sender's heap.
// Returns the virtual address in target's address space, or 0 on failure.
static uint64_t ipc_copy_to_heap(process_t* sender, process_t* target,
                                  uint64_t src_addr, uint32_t length) {
    if (!sender || !target || length == 0 || src_addr == 0) {
        return 0;
    }

    // Translate sender's virtual address to kernel-accessible pointer
    void* src = vmm_translate_user_addr(sender->cabin, src_addr, length);
    if (!src) {
        return 0;
    }

    // Allocate page(s) in target's buffer heap
    uint32_t pages_needed = (length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t target_vaddr = target->buf_heap_next;

    for (uint32_t i = 0; i < pages_needed; i++) {
        void* page = pmm_alloc(1);
        if (!page) {
            return 0;
        }

        uint64_t vaddr = target_vaddr + (i * PMM_PAGE_SIZE);
        vmm_map_result_t ret = vmm_map_page(target->cabin, vaddr, (uint64_t)page,
                                              VMM_FLAGS_USER_RW);
        if (!ret.success) {
            pmm_free(page, 1);
            return 0;
        }
    }

    target->buf_heap_next += pages_needed * PMM_PAGE_SIZE;

    // Copy data from sender to target via kernel mappings
    void* dst = vmm_translate_user_addr(target->cabin, target_vaddr, length);
    if (!dst) {
        return 0;
    }

    memcpy(dst, src, length);

    return target_vaddr;
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

    // Copy data from sender's heap to target's heap
    if (pocket->data_length > 0 && pocket->data_addr != 0) {
        process_t* sender = process_find(pocket->pid);
        if (!sender) {
            pocket->error_code = ERR_PROCESS_NOT_FOUND;
            return -1;
        }

        uint64_t new_addr = ipc_copy_to_heap(sender, target,
                                              pocket->data_addr, pocket->data_length);
        if (new_addr == 0) {
            pocket->error_code = ERR_NO_MEMORY;
            return -1;
        }

        // Update data_addr to point to the copy in target's heap.
        // Execution deck will put this address in Result.data_addr.
        pocket->data_addr = new_addr;
    }

    // Keep target_pid set — execution_deck will deliver Result to target's ResultRing.
    pocket->error_code = 0;

    debug_printf("[ROUTE] PID %u -> PID %u (heap copy)\n", pocket->pid, pocket->target_pid);

    return 0;
}

// Route tag broadcast: copies data to each matching target's heap and delivers Results directly.
// Cannot use execution_deck routing because it supports only one target_pid.
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

    process_t* sender = process_find(pocket->pid);
    if (!sender) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    uint32_t delivered = 0;

    for (uint32_t i = 0; i < target_count; i++) {
        process_t* t = process_find(targets[i]);
        if (!t) continue;

        if (!t->result_ring_phys) continue;

        ResultRing* rring = (ResultRing*)vmm_phys_to_virt(t->result_ring_phys);
        if (!rring) continue;

        // Copy data to target's heap
        uint64_t target_data_addr = 0;
        uint32_t target_data_length = 0;

        if (pocket->data_length > 0 && pocket->data_addr != 0) {
            target_data_addr = ipc_copy_to_heap(sender, t,
                                                 pocket->data_addr, pocket->data_length);
            if (target_data_addr != 0) {
                target_data_length = pocket->data_length;
            }
        }

        Result result;
        result.error_code = OK;
        result.data_length = target_data_length;
        result.data_addr = target_data_addr;
        result.sender_pid = pocket->pid;
        result._reserved = 0;

        if (result_ring_push(rring, &result)) {
            delivered++;
            if (process_get_state(t) == PROC_WAITING) {
                process_set_state(t, PROC_WORKING);
            }
        }
    }

    if (delivered == 0) {
        pocket->error_code = ERR_ROUTE_TARGET_FULL;
        return -1;
    }

    debug_printf("[ROUTE_TAG] PID %u -> %u/%u targets matching '%s'\n",
                 pocket->pid, delivered, target_count, pocket->route_tag);

    // Clear target_pid so execution_deck delivers confirmation to sender
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
