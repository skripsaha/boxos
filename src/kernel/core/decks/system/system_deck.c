#include "system_deck.h"
#include "system_deck_process.h"
#include "system_deck_context.h"
#include "system_deck_route.h"
#include "klib.h"
#include "process.h"
#include "scheduler.h"
#include "tagfs.h"
#include "atomics.h"

// Helper: Check if tag exists in snapshot buffer
static bool snapshot_has_tag(const char* snapshot, const char* tag) {
    if (!snapshot || !tag || tag[0] == '\0') {
        return false;
    }

    size_t tag_len = strlen(tag);
    const char* pos = snapshot;

    while (*pos) {
        const char* comma = strchr(pos, ',');
        size_t current_len = comma ? (size_t)(comma - pos) : strlen(pos);

        if (current_len == tag_len && strncmp(pos, tag, tag_len) == 0) {
            return true;
        }

        if (!comma) break;
        pos = comma + 1;
    }

    return false;
}

static int system_deck_yield(Event* event) {
    if (!event) {
        return -1;
    }

    process_t* proc = process_find(event->pid);
    if (!proc) {
        return -1;
    }
    process_ref_inc(proc);

    process_state_t state = process_get_state(proc);
    if (state != PROC_RUNNING) {
        process_ref_dec(proc);
        return -1;
    }

    scheduler_yield_cooperative();

    event->state = EVENT_STATE_COMPLETED;

    process_ref_dec(proc);
    return 0;
}

// --- Defragmentation ---

static int system_deck_defrag_file(Event* event) {
    if (!event) {
        return -1;
    }

    defrag_file_event_t* req = (defrag_file_event_t*)event->data;
    defrag_file_response_t* resp = (defrag_file_response_t*)event->data;

    uint32_t file_id = req->file_id;
    uint32_t target_block = req->target_block;

    memset(event->data, 0, EVENT_DATA_SIZE);

    int result = tagfs_defrag_file(file_id, target_block);

    if (result == 0) {
        resp->error_code = SYSTEM_ERR_SUCCESS;
        resp->fragmentation_score = tagfs_get_fragmentation_score();
        event->state = EVENT_STATE_COMPLETED;
        return 0;
    } else {
        resp->error_code = SYSTEM_ERR_INVALID_ARGS;
        resp->fragmentation_score = tagfs_get_fragmentation_score();
        event->state = EVENT_STATE_ERROR;
        return -1;
    }
}

static int system_deck_fragmentation_score(Event* event) {
    if (!event) {
        return -1;
    }

    fragmentation_score_response_t* resp = (fragmentation_score_response_t*)event->data;
    memset(event->data, 0, EVENT_DATA_SIZE);

    uint32_t score = tagfs_get_fragmentation_score();

    TagFSState* state = tagfs_get_state();
    uint32_t total_files = 0;
    uint32_t total_gaps = 0;

    if (state && state->initialized) {
        for (uint32_t i = 0; i < TAGFS_MAX_FILES; i++) {
            TagFSMetadata* meta = tagfs_get_metadata(i + 1);
            if (meta && (meta->flags & TAGFS_FILE_ACTIVE) && meta->block_count > 0) {
                total_files++;
            }
        }
    }

    resp->score = score;
    resp->total_files = total_files;
    resp->total_gaps = total_gaps;

    event->state = EVENT_STATE_COMPLETED;
    return 0;
}

// --- Overflow status ---

static int system_deck_get_overflow_status(Event* event) {
    if (!event) {
        return -1;
    }

    process_t* proc = process_find(event->pid);
    if (!proc) {
        event->state = EVENT_STATE_ERROR;
        memset(event->data, 0, EVENT_DATA_SIZE);
        *((uint32_t*)event->data) = 0xFFFFFFFF;
        return -1;
    }
    process_ref_inc(proc);

    uint32_t overflow_count = atomic_load_u32(&proc->result_overflow_count);
    uint8_t overflow_flag = atomic_load_u8(&proc->result_overflow_flag);

    bool clear_flag = (event->data[0] == 1);
    if (clear_flag) {
        atomic_store_u8(&proc->result_overflow_flag, 0);
    }

    memset(event->data, 0, EVENT_DATA_SIZE);
    *((uint32_t*)event->data) = overflow_count;
    *((uint8_t*)(event->data + 4)) = overflow_flag ? 1 : 0;

    process_ref_dec(proc);
    event->state = EVENT_STATE_COMPLETED;
    return 0;
}

// --- Main handler ---

int system_deck_handler(Event* event) {
    if (!event) {
        debug_printf("[SYSTEM_DECK] ERROR: NULL event\n");
        return -1;
    }

    uint8_t opcode = event_get_opcode(event, event->current_prefix_idx);

    switch (opcode) {
        case SYSTEM_OP_PROC_SPAWN:
            return system_deck_proc_spawn(event);

        case SYSTEM_OP_PROC_KILL:
            return system_deck_proc_kill(event);

        case SYSTEM_OP_PROC_INFO:
            return system_deck_proc_info(event);

        case SYSTEM_OP_PROC_EXEC:
            return system_deck_proc_exec(event);

        case SYSTEM_OP_CTX_USE:
            return system_deck_ctx_use(event);

        case SYSTEM_OP_BYPASS:
            debug_printf("[SYSTEM_DECK] BYPASS_ON (0x05) not implemented (security feature)\n");
            event->state = EVENT_STATE_ERROR;
            memset(event->data, 0, EVENT_DATA_SIZE);
            *((uint32_t*)event->data) = SYSTEM_ERR_NOT_IMPLEMENTED;
            return -1;

        case SYSTEM_OP_BUF_ALLOC:
            return system_deck_buf_alloc(event);

        case SYSTEM_OP_BUF_FREE:
            return system_deck_buf_free(event);

        case SYSTEM_OP_BUF_RESIZE:
            return system_deck_buf_resize(event);

        case SYSTEM_OP_TAG_ADD:
            return system_deck_tag_add(event);

        case SYSTEM_OP_TAG_REMOVE:
            return system_deck_tag_remove(event);

        case SYSTEM_OP_TAG_CHECK:
            return system_deck_tag_check(event);

        case SYSTEM_OP_DEFRAG_FILE:
            return system_deck_defrag_file(event);

        case SYSTEM_OP_FRAGMENTATION_SCORE:
            return system_deck_fragmentation_score(event);

        case SYSTEM_OP_OVERFLOW_STATUS:
            return system_deck_get_overflow_status(event);

        case SYSTEM_OP_ROUTE:
            return system_deck_route(event);

        case SYSTEM_OP_ROUTE_TAG:
            return system_deck_route_tag(event);

        case SYSTEM_OP_LISTEN:
            return system_deck_listen(event);

        case SYSTEM_OP_YIELD:
            return system_deck_yield(event);

        default:
            debug_printf("[SYSTEM_DECK] Unknown opcode 0x%02x\n", opcode);
            event->state = EVENT_STATE_ERROR;
            return -1;
    }
}

static bool system_deck_check_permission(process_t* proc, uint8_t opcode, uint32_t caller_pid) {
    if (!proc) {
        return false;
    }
    (void)caller_pid;

    // atomic snapshot of tags (64 bytes on stack)
    char tags_snapshot[PROCESS_TAG_SIZE];
    process_snapshot_tags(proc, tags_snapshot, sizeof(tags_snapshot));

    // Now all checks use snapshot, not live proc->tags
    switch (opcode) {
        case SYSTEM_OP_PROC_SPAWN:
            return snapshot_has_tag(tags_snapshot, "proc_spawn") ||
                   snapshot_has_tag(tags_snapshot, "utility") ||
                   snapshot_has_tag(tags_snapshot, "system");

        case SYSTEM_OP_PROC_EXEC:
            return snapshot_has_tag(tags_snapshot, "proc_spawn") ||
                   snapshot_has_tag(tags_snapshot, "utility") ||
                   snapshot_has_tag(tags_snapshot, "system");

        case SYSTEM_OP_PROC_KILL:
            return true;

        case SYSTEM_OP_PROC_INFO:
            return true;

        case SYSTEM_OP_BUF_ALLOC:
        case SYSTEM_OP_BUF_FREE:
        case SYSTEM_OP_BUF_RESIZE:
            return snapshot_has_tag(tags_snapshot, "app") ||
                   snapshot_has_tag(tags_snapshot, "utility") ||
                   snapshot_has_tag(tags_snapshot, "system");

        case SYSTEM_OP_TAG_ADD:
        case SYSTEM_OP_TAG_REMOVE:
            return snapshot_has_tag(tags_snapshot, "utility") ||
                   snapshot_has_tag(tags_snapshot, "system");

        case SYSTEM_OP_TAG_CHECK:
        case SYSTEM_OP_CTX_USE:
        case SYSTEM_OP_OVERFLOW_STATUS:
        case SYSTEM_OP_YIELD:
            return true;

        case SYSTEM_OP_DEFRAG_FILE:
        case SYSTEM_OP_FRAGMENTATION_SCORE:
            return snapshot_has_tag(tags_snapshot, "utility") ||
                   snapshot_has_tag(tags_snapshot, "system");

        case SYSTEM_OP_ROUTE:
        case SYSTEM_OP_ROUTE_TAG:
            return snapshot_has_tag(tags_snapshot, "app") ||
                   snapshot_has_tag(tags_snapshot, "utility") ||
                   snapshot_has_tag(tags_snapshot, "system");

        case SYSTEM_OP_LISTEN:
            return snapshot_has_tag(tags_snapshot, "app") ||
                   snapshot_has_tag(tags_snapshot, "utility") ||
                   snapshot_has_tag(tags_snapshot, "display") ||
                   snapshot_has_tag(tags_snapshot, "system");

        default:
            debug_printf("[SECURITY] DENY: Unknown System opcode 0x%02x\n", opcode);
            return false;
    }
}

bool system_security_gate(uint32_t pid, uint8_t deck_id, uint8_t opcode) {
    process_t* proc = process_find(pid);
    if (!proc) {
        debug_printf("[SECURITY] DENY: Invalid PID %u\n", pid);
        return false;
    }

    // atomic snapshot of tags
    char tags_snapshot[PROCESS_TAG_SIZE];
    process_snapshot_tags(proc, tags_snapshot, sizeof(tags_snapshot));

    switch (deck_id) {
        case 0x01:  // Operations Deck
            return true;

        case 0x02: {  // Storage Deck
            bool is_write_op = (opcode == 0x02 || opcode == 0x03 || opcode == 0x06 ||
                               opcode == 0x07 || opcode == 0x08 || opcode == 0x09 ||
                               opcode == 0x10 || opcode == 0x11);

            bool is_read_op = (opcode == 0x01 || opcode == 0x05 || opcode == 0x0A);

            if (is_write_op) {
                return snapshot_has_tag(tags_snapshot, "utility") ||
                       snapshot_has_tag(tags_snapshot, "storage") ||
                       snapshot_has_tag(tags_snapshot, "system");
            }

            if (is_read_op) {
                return snapshot_has_tag(tags_snapshot, "app") ||
                       snapshot_has_tag(tags_snapshot, "utility") ||
                       snapshot_has_tag(tags_snapshot, "storage") ||
                       snapshot_has_tag(tags_snapshot, "system");
            }

            return snapshot_has_tag(tags_snapshot, "system");
        }

        case 0x03: {  // Hardware Deck
            if (opcode == 0x10 || opcode == 0x11 || opcode == 0x12 ||
                opcode == 0x15 || opcode == 0x16 || opcode == 0x17 ||
                opcode == 0x7B) {
                return true;
            }

            if (opcode == 0x34 || opcode == 0x35 || opcode == 0x53 ||
                opcode == 0x75 || opcode == 0x78) {
                return snapshot_has_tag(tags_snapshot, "hw_access") ||
                       snapshot_has_tag(tags_snapshot, "hardware") ||
                       snapshot_has_tag(tags_snapshot, "system");
            }

            if (opcode >= 0x60 && opcode <= 0x62) {
                return snapshot_has_tag(tags_snapshot, "hw_keyboard") ||
                       snapshot_has_tag(tags_snapshot, "system");
            }

            if ((opcode >= 0x70 && opcode <= 0x74) ||
                opcode == 0x76 || opcode == 0x77 ||
                opcode == 0x79 || opcode == 0x7A) {
                return snapshot_has_tag(tags_snapshot, "hw_vga") ||
                       snapshot_has_tag(tags_snapshot, "system");
            }

            if (opcode == 0x52) {
                return snapshot_has_tag(tags_snapshot, "storage") ||
                       snapshot_has_tag(tags_snapshot, "system");
            }

            if ((opcode >= 0x20 && opcode <= 0x25) ||
                (opcode >= 0x32 && opcode <= 0x36) ||
                opcode == 0x40) {
                return snapshot_has_tag(tags_snapshot, "hw_access") ||
                       snapshot_has_tag(tags_snapshot, "hardware") ||
                       snapshot_has_tag(tags_snapshot, "system");
            }

            if (opcode == 0x80 || opcode == 0x81) {
                return snapshot_has_tag(tags_snapshot, "hw_power") ||
                       snapshot_has_tag(tags_snapshot, "utility") ||
                       snapshot_has_tag(tags_snapshot, "system");
            }

            return snapshot_has_tag(tags_snapshot, "utility") ||
                   snapshot_has_tag(tags_snapshot, "system");
        }

        case 0x04:  // Network Deck
            return snapshot_has_tag(tags_snapshot, "network") ||
                   snapshot_has_tag(tags_snapshot, "net_access") ||
                   snapshot_has_tag(tags_snapshot, "system");

        case SYSTEM_DECK_ID:
            return system_deck_check_permission(proc, opcode, pid);

        default:
            debug_printf("[SECURITY] DENY: Unknown deck 0x%02x\n", deck_id);
            return false;
    }
}
