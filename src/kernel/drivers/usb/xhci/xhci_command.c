#include "xhci_command.h"
#include "xhci_interrupt.h"
#include "xhci_enumeration.h"
#include "xhci_regs.h"
#include "xhci_rings.h"
#include "klib.h"
#include "atomics.h"
#include "cpu_calibrate.h"

struct xhci_pending_cmd pending_cmds[XHCI_MAX_PENDING_CMDS];
spinlock_t pending_cmds_lock;
uint32_t cmd_sequence = 0;

void xhci_command_init(void) {
    spinlock_init(&pending_cmds_lock);
    memset(pending_cmds, 0, sizeof(pending_cmds));
    cmd_sequence = 0;
}

static int find_cmd_by_trb_phys(uint64_t trb_phys) {
    spin_lock(&pending_cmds_lock);
    for (int i = 0; i < XHCI_MAX_PENDING_CMDS; i++) {
        if (pending_cmds[i].state == CMD_STATE_POSTED &&
            pending_cmds[i].trb_phys == trb_phys) {
            spin_unlock(&pending_cmds_lock);
            return i;
        }
    }
    spin_unlock(&pending_cmds_lock);
    return -1;
}

/* TOCTOU fix: previously find_free_cmd_slot() released the lock between
 * locating an IDLE slot and the caller marking it POSTED, so two callers on
 * different cores could grab the same index. Now allocate, populate, and
 * mark POSTED as one critical section. cmd_sequence is also bumped under
 * the lock so two posters cannot get the same sequence number. */
static int post_command(xhci_controller_t* ctrl, xhci_trb_t* trb, uint8_t slot_id) {
    uint8_t trb_type = (uint8_t)TRB_GET_TYPE(trb->control);

    spin_lock(&pending_cmds_lock);

    int cmd_idx = -1;
    for (int i = 0; i < XHCI_MAX_PENDING_CMDS; i++) {
        if (pending_cmds[i].state == CMD_STATE_IDLE) {
            cmd_idx = i;
            break;
        }
    }
    if (cmd_idx < 0) {
        spin_unlock(&pending_cmds_lock);
        debug_printf("[xHCI CMD] No free command slots\n");
        return -1;
    }

    uint64_t trb_phys = xhci_ring_enqueue(&ctrl->command_ring, trb);
    if (trb_phys == 0) {
        spin_unlock(&pending_cmds_lock);
        debug_printf("[xHCI CMD] Command ring full\n");
        return -1;
    }

    pending_cmds[cmd_idx].trb_phys = trb_phys;
    pending_cmds[cmd_idx].timestamp_posted = rdtsc();
    pending_cmds[cmd_idx].sequence = ++cmd_sequence;
    pending_cmds[cmd_idx].slot_id = slot_id;
    pending_cmds[cmd_idx].trb_type = trb_type;
    pending_cmds[cmd_idx].state = CMD_STATE_POSTED;
    pending_cmds[cmd_idx].completion_code = 0;
    pending_cmds[cmd_idx].completion_param = 0;

    spin_unlock(&pending_cmds_lock);

    __sync_synchronize();
    ctrl->doorbells->doorbells[0].doorbell = 0;

    return 0;
}

int xhci_post_enable_slot_cmd(xhci_controller_t* ctrl) {
    if (!ctrl || !ctrl->running) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.control = TRB_SET_TYPE(TRB_TYPE_ENABLE_SLOT);

    return post_command(ctrl, &trb, 0);
}

int xhci_post_disable_slot_cmd(xhci_controller_t* ctrl, uint8_t slot_id) {
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.control = TRB_SET_TYPE(TRB_TYPE_DISABLE_SLOT) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id);
}

int xhci_post_address_device_cmd(xhci_controller_t* ctrl, uint8_t slot_id, uint64_t input_ctx_phys) {
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.parameter = input_ctx_phys;
    trb.control = TRB_SET_TYPE(TRB_TYPE_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id);
}

int xhci_post_configure_endpoint_cmd(xhci_controller_t* ctrl, uint8_t slot_id, uint64_t input_ctx_phys) {
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.parameter = input_ctx_phys;
    trb.control = TRB_SET_TYPE(TRB_TYPE_CONFIGURE_ENDPOINT) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id);
}

int xhci_post_evaluate_context_cmd(xhci_controller_t* ctrl, uint8_t slot_id, uint64_t input_ctx_phys) {
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.parameter = input_ctx_phys;
    trb.control = TRB_SET_TYPE(TRB_TYPE_EVALUATE_CONTEXT) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id);
}

int xhci_post_reset_endpoint_cmd(xhci_controller_t* ctrl, uint8_t slot_id, uint8_t dci) {
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots ||
        dci == 0 || dci > 31) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.control = TRB_SET_TYPE(TRB_TYPE_RESET_ENDPOINT) |
                  ((uint32_t)dci << 16) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id);
}

int xhci_post_set_tr_dequeue_cmd(xhci_controller_t* ctrl, uint8_t slot_id,
                                 uint8_t dci, uint64_t dequeue_ptr_with_dcs) {
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots ||
        dci == 0 || dci > 31) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.parameter = dequeue_ptr_with_dcs;
    trb.control = TRB_SET_TYPE(TRB_TYPE_SET_TR_DEQUEUE) |
                  ((uint32_t)dci << 16) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id);
}

/* Which commands are steps of enumeration.
 *
 * Every command completion used to be handed to the enumeration state machine,
 * which was harmless only while enumeration was the sole thing issuing
 * commands. It is not any more: an endpoint reset on a device that is already
 * running would arrive as an unexplained completion for a slot in a settled
 * state, and a state machine driven by events it did not ask for is a state
 * machine that will eventually take the wrong branch. */
static bool cmd_is_enumeration_step(uint8_t trb_type) {
    switch (trb_type) {
        case TRB_TYPE_ENABLE_SLOT:
        case TRB_TYPE_ADDRESS_DEVICE:
        case TRB_TYPE_CONFIGURE_ENDPOINT:
        case TRB_TYPE_EVALUATE_CONTEXT:
        /* Clearing a stalled control pipe is part of enumeration when it is
         * enumeration that stalled. On a slot that has finished, the state
         * machine has no case for these and says so quietly. */
        case TRB_TYPE_RESET_ENDPOINT:
        case TRB_TYPE_SET_TR_DEQUEUE:
            return true;
        default:
            return false;
    }
}

void xhci_handle_command_completion(xhci_controller_t* ctrl, xhci_trb_t* event) {
    if (!ctrl || !event) {
        return;
    }

    uint64_t trb_phys = event->parameter;
    uint8_t completion_code = (event->status >> 24) & 0xFF;
    uint8_t slot_id = (event->control >> 24) & 0xFF;

    int cmd_idx = find_cmd_by_trb_phys(trb_phys);
    if (cmd_idx < 0) {
        /*
         * An answer to a question nobody remembers asking.
         *
         * Said out loud, not into a debug build: this is a completion that
         * advances nothing, so whatever was waiting on it waits until the
         * watchdog gives up — a device that never enumerates, with no line in
         * the log to say why. It has to be visible on a machine whose only
         * diagnostic is a screen.
         */
        kprintf("[xHCI] a command completion arrived for TRB 0x%llx, which is "
                "not one this driver is waiting on (slot %u, code %u)\n",
                (unsigned long long)trb_phys, slot_id, completion_code);
        return;
    }

    spin_lock(&pending_cmds_lock);
    pending_cmds[cmd_idx].completion_code = completion_code;
    pending_cmds[cmd_idx].slot_id = slot_id;
    pending_cmds[cmd_idx].completion_param = event->status;
    pending_cmds[cmd_idx].state = (completion_code == TRB_COMPLETION_SUCCESS)
                                   ? CMD_STATE_COMPLETED : CMD_STATE_ERROR;
    uint8_t trb_type = pending_cmds[cmd_idx].trb_type;
    spin_unlock(&pending_cmds_lock);

    if (completion_code != TRB_COMPLETION_SUCCESS) {
        kprintf("[xHCI] command %u on slot %u failed with completion code %u\n",
                trb_type, slot_id, completion_code);
    }

    if (cmd_is_enumeration_step(trb_type)) {
        xhci_enum_advance_state(ctrl, slot_id, completion_code);
    }

    spin_lock(&pending_cmds_lock);
    pending_cmds[cmd_idx].state = CMD_STATE_IDLE;
    pending_cmds[cmd_idx].trb_phys = 0;
    spin_unlock(&pending_cmds_lock);
}

int xhci_command_wait_idle(xhci_controller_t* ctrl, uint32_t timeout_ms)
{
    if (!ctrl) {
        return -1;
    }

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);

    for (;;) {
        xhci_process_events();

        bool busy = false;
        spin_lock(&pending_cmds_lock);
        for (int i = 0; i < XHCI_MAX_PENDING_CMDS; i++) {
            if (pending_cmds[i].state == CMD_STATE_POSTED) {
                busy = true;
                break;
            }
        }
        spin_unlock(&pending_cmds_lock);

        if (!busy) {
            return 0;
        }
        if ((int64_t)(rdtsc() - deadline) >= 0) {
            return -1;
        }
        cpu_pause();
    }
}

bool xhci_command_pending_for_slot(uint8_t slot_id)
{
    if (slot_id == 0) {
        return false;
    }

    spin_lock(&pending_cmds_lock);
    for (int i = 0; i < XHCI_MAX_PENDING_CMDS; i++) {
        if (pending_cmds[i].state == CMD_STATE_POSTED &&
            pending_cmds[i].slot_id == slot_id) {
            spin_unlock(&pending_cmds_lock);
            return true;
        }
    }
    spin_unlock(&pending_cmds_lock);
    return false;
}

void xhci_check_command_timeouts(xhci_controller_t* ctrl) {
    if (!ctrl) {
        return;
    }

    uint64_t now = rdtsc();
    uint64_t timeout_cycles = cpu_ms_to_tsc(XHCI_CMD_TIMEOUT_MS);

    spin_lock(&pending_cmds_lock);
    for (int i = 0; i < XHCI_MAX_PENDING_CMDS; i++) {
        if (pending_cmds[i].state == CMD_STATE_POSTED) {
            int64_t elapsed = (int64_t)(now - pending_cmds[i].timestamp_posted);
            if (elapsed > (int64_t)timeout_cycles) {
                /*
                 * A command the controller never answered.
                 *
                 * Silent until now, and the one fact that would have settled
                 * an argument that ran for six flashes: whether four devices
                 * stuck mid-enumeration were waiting on commands that had been
                 * reaped here, or on completions that arrived and went
                 * nowhere. Those are opposite faults and the log could not
                 * tell them apart.
                 */
                kprintf("[xHCI] command type %u on slot %u went unanswered for "
                        "%u ms — giving up on it\n",
                        pending_cmds[i].trb_type, pending_cmds[i].slot_id,
                        XHCI_CMD_TIMEOUT_MS);

                uint8_t slot_id = pending_cmds[i].slot_id;
                pending_cmds[i].state = CMD_STATE_IDLE;
                pending_cmds[i].trb_phys = 0;

                if (slot_id > 0 && slot_id <= ctrl->max_slots) {
                    spin_unlock(&pending_cmds_lock);
                    xhci_device_slot_t* slot = xhci_get_device_slot(ctrl, slot_id);
                    if (slot && slot->state != ENUM_STATE_IDLE) {
                        xhci_slot_retire(ctrl, slot);
                    }
                    spin_lock(&pending_cmds_lock);
                }
            }
        }
    }
    spin_unlock(&pending_cmds_lock);
}
