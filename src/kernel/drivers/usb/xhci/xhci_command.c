#include "xhci_command.h"
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

static int find_free_cmd_slot(void) {
    spin_lock(&pending_cmds_lock);
    for (int i = 0; i < XHCI_MAX_PENDING_CMDS; i++) {
        if (pending_cmds[i].state == CMD_STATE_IDLE) {
            spin_unlock(&pending_cmds_lock);
            return i;
        }
    }
    spin_unlock(&pending_cmds_lock);
    return -1;
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

static int post_command(xhci_controller_t* ctrl, xhci_trb_t* trb, uint8_t slot_id) {
    int cmd_idx = find_free_cmd_slot();
    if (cmd_idx < 0) {
        debug_printf("[xHCI CMD] No free command slots\n");
        return -1;
    }

    uint64_t trb_phys = xhci_ring_enqueue(&ctrl->command_ring, trb);
    if (trb_phys == 0) {
        debug_printf("[xHCI CMD] Command ring full\n");
        return -1;
    }

    pending_cmds[cmd_idx].trb_phys = trb_phys;
    pending_cmds[cmd_idx].timestamp_posted = rdtsc();
    pending_cmds[cmd_idx].sequence = ++cmd_sequence;
    pending_cmds[cmd_idx].slot_id = slot_id;
    pending_cmds[cmd_idx].state = CMD_STATE_POSTED;
    pending_cmds[cmd_idx].completion_code = 0;
    pending_cmds[cmd_idx].completion_param = 0;

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

void xhci_handle_command_completion(xhci_controller_t* ctrl, xhci_trb_t* event) {
    if (!ctrl || !event) {
        return;
    }

    uint64_t trb_phys = event->parameter;
    uint8_t completion_code = (event->status >> 24) & 0xFF;
    uint8_t slot_id = (event->control >> 24) & 0xFF;

    int cmd_idx = find_cmd_by_trb_phys(trb_phys);
    if (cmd_idx < 0) {
        debug_printf("[xHCI CMD] No matching pending command for TRB 0x%llx\n", trb_phys);
        return;
    }

    spin_lock(&pending_cmds_lock);
    pending_cmds[cmd_idx].completion_code = completion_code;
    pending_cmds[cmd_idx].slot_id = slot_id;
    pending_cmds[cmd_idx].completion_param = event->status;
    pending_cmds[cmd_idx].state = (completion_code == TRB_COMPLETION_SUCCESS)
                                   ? CMD_STATE_COMPLETED : CMD_STATE_ERROR;
    spin_unlock(&pending_cmds_lock);

    if (completion_code != TRB_COMPLETION_SUCCESS) {
        debug_printf("[xHCI CMD] Command failed: slot=%u code=%u\n", slot_id, completion_code);
    }

    xhci_enum_advance_state(ctrl, slot_id, completion_code);

    spin_lock(&pending_cmds_lock);
    pending_cmds[cmd_idx].state = CMD_STATE_IDLE;
    pending_cmds[cmd_idx].trb_phys = 0;
    spin_unlock(&pending_cmds_lock);
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
                debug_printf("[xHCI CMD] Command timeout: TRB=0x%llx slot=%u\n",
                             pending_cmds[i].trb_phys, pending_cmds[i].slot_id);

                uint8_t slot_id = pending_cmds[i].slot_id;
                pending_cmds[i].state = CMD_STATE_IDLE;
                pending_cmds[i].trb_phys = 0;

                if (slot_id > 0 && slot_id <= ctrl->max_slots) {
                    spin_unlock(&pending_cmds_lock);
                    xhci_device_slot_t* slot = xhci_get_device_slot(ctrl, slot_id);
                    if (slot && slot->state != ENUM_STATE_IDLE) {
                        xhci_device_slot_cleanup(ctrl, slot);
                    }
                    spin_lock(&pending_cmds_lock);
                }
            }
        }
    }
    spin_unlock(&pending_cmds_lock);
}
