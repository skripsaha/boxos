#include "xhci_command.h"
#include "xhci_interrupt.h"
#include "xhci_enumeration.h"
#include "xhci_device.h"
#include "xhci_port.h"
#include "xhci_regs.h"
#include "xhci_rings.h"
#include "klib.h"
#include "atomics.h"
#include "cpu_calibrate.h"

const char* xhci_completion_name(uint8_t code)
{
    switch (code) {
        case TRB_COMPLETION_SUCCESS:          return "Success";
        case TRB_COMPLETION_DATA_BUFFER_ERR:  return "Data Buffer Error";
        case TRB_COMPLETION_BABBLE:           return "Babble Detected";
        case TRB_COMPLETION_USB_TRANS_ERR:    return "USB Transaction Error";
        case TRB_COMPLETION_TRB_ERROR:        return "TRB Error";
        case TRB_COMPLETION_STALL:            return "Stall";
        case TRB_COMPLETION_RESOURCE_ERR:     return "Resource Error";
        case TRB_COMPLETION_BANDWIDTH_ERR:    return "Bandwidth Error";
        case TRB_COMPLETION_NO_SLOTS:         return "No Slots Available";
        case TRB_COMPLETION_SLOT_NOT_ENABLED: return "Slot Not Enabled";
        case TRB_COMPLETION_EP_NOT_ENABLED:   return "Endpoint Not Enabled";
        case TRB_COMPLETION_SHORT_PKT:        return "Short Packet";
        case TRB_COMPLETION_RING_UNDERRUN:    return "Ring Underrun";
        case TRB_COMPLETION_RING_OVERRUN:     return "Ring Overrun";
        case TRB_COMPLETION_PARAMETER_ERR:    return "Parameter Error";
        case TRB_COMPLETION_BANDWIDTH_OVER:   return "Bandwidth Overrun";
        case TRB_COMPLETION_CONTEXT_STATE:    return "Context State Error";
        case TRB_COMPLETION_NO_PING_RESPONSE: return "No Ping Response";
        case TRB_COMPLETION_EVENT_RING_FULL:  return "Event Ring Full";
        case TRB_COMPLETION_INCOMPATIBLE_DEV: return "Incompatible Device";
        case TRB_COMPLETION_MISSED_SERVICE:   return "Missed Service Error";
        case TRB_COMPLETION_CMD_RING_STOPPED: return "Command Ring Stopped";
        case TRB_COMPLETION_COMMAND_ABORTED:  return "Command Aborted";
        case TRB_COMPLETION_STOPPED:          return "Stopped";
        case TRB_COMPLETION_STOPPED_LENGTH:   return "Stopped - Length Invalid";
        case TRB_COMPLETION_SPLIT_TRANS_ERR:  return "Split Transaction Error";
        default:                              return "an unnamed code";
    }
}

const char* xhci_command_name(uint8_t trb_type)
{
    switch (trb_type) {
        case TRB_TYPE_ENABLE_SLOT:        return "Enable Slot";
        case TRB_TYPE_DISABLE_SLOT:       return "Disable Slot";
        case TRB_TYPE_ADDRESS_DEVICE:     return "Address Device";
        case TRB_TYPE_CONFIGURE_ENDPOINT: return "Configure Endpoint";
        case TRB_TYPE_EVALUATE_CONTEXT:   return "Evaluate Context";
        case TRB_TYPE_RESET_ENDPOINT:     return "Reset Endpoint";
        case TRB_TYPE_STOP_ENDPOINT:      return "Stop Endpoint";
        case TRB_TYPE_SET_TR_DEQUEUE:     return "Set TR Dequeue Pointer";
        case TRB_TYPE_NO_OP_CMD:          return "No Op";
        default:                          return "an unnamed command";
    }
}

void xhci_command_init(xhci_controller_t* ctrl)
{
    if (!ctrl) {
        return;
    }
    spinlock_init(&ctrl->pending_lock);
    memset(ctrl->pending_cmds, 0, sizeof(ctrl->pending_cmds));
}

static int cmd_index_for(const xhci_controller_t* ctrl, uint64_t trb_phys)
{
    uint64_t base = ctrl->command_ring.trbs_phys;
    if (base == 0 || trb_phys < base) {
        return -1;
    }
    uint64_t offset = trb_phys - base;
    if (offset % sizeof(xhci_trb_t)) {
        return -1;
    }
    uint64_t index = offset / sizeof(xhci_trb_t);
    if (index >= ctrl->command_ring.num_trbs) {
        return -1;
    }
    return (int)index;
}

static int post_command(xhci_controller_t* ctrl, xhci_trb_t* trb,
                        uint8_t slot_id, xhci_device_slot_t* owner)
{
    uint8_t trb_type = (uint8_t)TRB_GET_TYPE(trb->control);

    spin_lock(&ctrl->pending_lock);

    uint64_t trb_phys = xhci_ring_enqueue(&ctrl->command_ring, trb);
    if (trb_phys == 0) {
        spin_unlock(&ctrl->pending_lock);
        kprintf("[xHCI %s] the command ring is full — %s for slot %u was not "
                "sent\n", ctrl->name, xhci_command_name(trb_type), slot_id);
        return -1;
    }

    int index = cmd_index_for(ctrl, trb_phys);
    if (index < 0) {
        spin_unlock(&ctrl->pending_lock);
        kprintf("[xHCI %s] the command ring produced an address outside "
                "itself (0x%llx)\n", ctrl->name,
                (unsigned long long)trb_phys);
        return -1;
    }

    if (ctrl->pending_cmds[index].state == XHCI_CMD_POSTED) {
        spin_unlock(&ctrl->pending_lock);
        kprintf("[xHCI %s] the command ring wrapped onto %s, still unanswered "
                "— the controller has stopped taking commands\n", ctrl->name,
                xhci_command_name(ctrl->pending_cmds[index].trb_type));
        return -1;
    }

    if (ctrl->last_cmd_answer == 0) {
        ctrl->last_cmd_answer = rdtsc();
    }

    ctrl->pending_cmds[index].owner       = owner;
    ctrl->pending_cmds[index].owner_epoch = xhci_slot_epoch(owner);
    ctrl->pending_cmds[index].posted_at   = rdtsc();
    ctrl->pending_cmds[index].slot_id     = slot_id;
    ctrl->pending_cmds[index].trb_type    = trb_type;
    ctrl->pending_cmds[index].state       = XHCI_CMD_POSTED;

    spin_unlock(&ctrl->pending_lock);

    __sync_synchronize();
    ctrl->doorbells->doorbells[0].doorbell = 0;

    return 0;
}

int xhci_post_enable_slot_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                              uint8_t slot_type)
{
    if (!ctrl || !ctrl->running) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.control = TRB_SET_TYPE(TRB_TYPE_ENABLE_SLOT) |
                  (((uint32_t)slot_type & 0x1Fu) << 16);

    return post_command(ctrl, &trb, 0, owner);
}

int xhci_post_disable_slot_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                               uint8_t slot_id)
{
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.control = TRB_SET_TYPE(TRB_TYPE_DISABLE_SLOT) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id, owner);
}

int xhci_post_address_device_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                 uint8_t slot_id, uint64_t input_ctx_phys,
                                 bool block_set_address)
{
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.parameter = input_ctx_phys;
    trb.control = TRB_SET_TYPE(TRB_TYPE_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24);
    if (block_set_address) {
        trb.control |= TRB_BSR;
    }

    return post_command(ctrl, &trb, slot_id, owner);
}

int xhci_post_configure_endpoint_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                     uint8_t slot_id, uint64_t input_ctx_phys)
{
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.parameter = input_ctx_phys;
    trb.control = TRB_SET_TYPE(TRB_TYPE_CONFIGURE_ENDPOINT) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id, owner);
}

int xhci_post_reset_endpoint_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                 uint8_t slot_id, uint8_t dci)
{
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots ||
        dci == 0 || dci > 31) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.control = TRB_SET_TYPE(TRB_TYPE_RESET_ENDPOINT) |
                  ((uint32_t)dci << 16) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id, owner);
}

int xhci_post_stop_endpoint_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                uint8_t slot_id, uint8_t dci)
{
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots ||
        dci == 0 || dci > 31) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.control = TRB_SET_TYPE(TRB_TYPE_STOP_ENDPOINT) |
                  ((uint32_t)dci << 16) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id, owner);
}

int xhci_post_set_tr_dequeue_cmd(xhci_controller_t* ctrl, xhci_device_slot_t* owner,
                                 uint8_t slot_id, uint8_t dci,
                                 uint64_t dequeue_ptr_with_dcs)
{
    if (!ctrl || !ctrl->running || slot_id == 0 || slot_id > ctrl->max_slots ||
        dci == 0 || dci > 31) {
        return -1;
    }

    xhci_trb_t trb = {0};
    trb.parameter = dequeue_ptr_with_dcs;
    trb.control = TRB_SET_TYPE(TRB_TYPE_SET_TR_DEQUEUE) |
                  ((uint32_t)dci << 16) | ((uint32_t)slot_id << 24);

    return post_command(ctrl, &trb, slot_id, owner);
}

static bool cmd_is_enumeration_step(uint8_t trb_type,
                                    const xhci_device_slot_t* owner)
{
    if (!owner || __atomic_load_n(&owner->state, __ATOMIC_ACQUIRE) ==
                      ENUM_STATE_CONFIGURED) {
        return false;
    }

    switch (trb_type) {
        case TRB_TYPE_ENABLE_SLOT:
        case TRB_TYPE_ADDRESS_DEVICE:
        case TRB_TYPE_CONFIGURE_ENDPOINT:
        case TRB_TYPE_RESET_ENDPOINT:
        case TRB_TYPE_STOP_ENDPOINT:
        case TRB_TYPE_SET_TR_DEQUEUE:
            return true;
        default:
            return false;
    }
}

void xhci_handle_command_completion(xhci_controller_t* ctrl, xhci_trb_t* event)
{
    if (!ctrl || !event) {
        return;
    }

    uint64_t trb_phys        = event->parameter;
    uint8_t  completion_code = (event->status >> 24) & 0xFF;
    uint8_t  event_slot_id   = (event->control >> 24) & 0xFF;

    int index = cmd_index_for(ctrl, trb_phys);
    if (index < 0) {
        kprintf("[xHCI %s] a completion names TRB 0x%llx, which is not on this "
                "controller's command ring (slot %u, %s)\n",
                ctrl->name, (unsigned long long)trb_phys, event_slot_id,
                xhci_completion_name(completion_code));
        return;
    }

    xhci_ring_reclaim_to(&ctrl->command_ring, trb_phys);

    spin_lock(&ctrl->pending_lock);
    xhci_pending_cmd_t entry = ctrl->pending_cmds[index];
    if (entry.state != XHCI_CMD_POSTED) {
        spin_unlock(&ctrl->pending_lock);
        if (completion_code != TRB_COMPLETION_CMD_RING_STOPPED) {
            kprintf("[xHCI %s] a second answer arrived for a command already "
                    "settled (slot %u, %s)\n", ctrl->name, event_slot_id,
                    xhci_completion_name(completion_code));
        }
        return;
    }
    ctrl->pending_cmds[index].state = XHCI_CMD_FREE;
    ctrl->pending_cmds[index].owner = NULL;
    ctrl->last_cmd_answer = rdtsc();
    ctrl->cmd_nudges      = 0;
    spin_unlock(&ctrl->pending_lock);

    uint32_t took = (uint32_t)cpu_tsc_to_ms(rdtsc() - entry.posted_at);
    if (took >= XHCI_CMD_SLOW_MS) {
        kprintf("[xHCI %s] %s on slot %u took %u ms\n", ctrl->name,
                xhci_command_name(entry.trb_type), event_slot_id, took);
    }

    if (!xhci_slot_still_is(entry.owner, entry.owner_epoch)) {
        kprintf("[xHCI %s] %s answered for slot %u after the device had gone "
                "(%s)\n", ctrl->name, xhci_command_name(entry.trb_type),
                event_slot_id, xhci_completion_name(completion_code));
        return;
    }

    if (completion_code != TRB_COMPLETION_SUCCESS) {
        kprintf("[xHCI %s] %s on slot %u was refused: %s (code %u)\n",
                ctrl->name, xhci_command_name(entry.trb_type),
                entry.slot_id ? entry.slot_id : event_slot_id,
                xhci_completion_name(completion_code), completion_code);
    }

    if (cmd_is_enumeration_step(entry.trb_type, entry.owner)) {
        xhci_enum_advance_state(ctrl, entry.owner, event_slot_id,
                                completion_code);
    }
}

int xhci_command_wait_idle(xhci_controller_t* ctrl, uint32_t timeout_ms)
{
    if (!ctrl) {
        return -1;
    }

    if (xhci_drain_is_mine(ctrl)) {
        kprintf("[xHCI %s] the command ring was waited on from inside the "
                "event drain — its answers cannot arrive until this returns\n",
                ctrl->name);
        return -2;
    }

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);

    for (;;) {
        xhci_process_events();

        if (xhci_command_outstanding(ctrl) == 0) {
            return 0;
        }
        if ((int64_t)(rdtsc() - deadline) >= 0) {
            return -1;
        }
        cpu_pause();
    }
}

bool xhci_command_pending_for(xhci_controller_t* ctrl,
                              const xhci_device_slot_t* slot)
{
    if (!ctrl || !slot) {
        return false;
    }

    spin_lock(&ctrl->pending_lock);
    for (uint32_t i = 0; i < XHCI_CMD_RING_TRBS; i++) {
        if (ctrl->pending_cmds[i].state == XHCI_CMD_POSTED &&
            ctrl->pending_cmds[i].owner == slot) {
            spin_unlock(&ctrl->pending_lock);
            return true;
        }
    }
    spin_unlock(&ctrl->pending_lock);
    return false;
}

unsigned xhci_command_outstanding(xhci_controller_t* ctrl)
{
    if (!ctrl) {
        return 0;
    }

    unsigned count = 0;
    spin_lock(&ctrl->pending_lock);
    for (uint32_t i = 0; i < XHCI_CMD_RING_TRBS; i++) {
        if (ctrl->pending_cmds[i].state == XHCI_CMD_POSTED) {
            count++;
        }
    }
    spin_unlock(&ctrl->pending_lock);
    return count;
}

bool xhci_command_oldest_for(xhci_controller_t* ctrl,
                             const xhci_device_slot_t* slot,
                             uint8_t* out_trb_type, uint32_t* out_age_ms)
{
    if (!ctrl || !slot) {
        return false;
    }

    uint64_t now = rdtsc();
    uint64_t oldest = 0;
    uint8_t  type = 0;
    bool     found = false;

    spin_lock(&ctrl->pending_lock);
    for (uint32_t i = 0; i < XHCI_CMD_RING_TRBS; i++) {
        if (ctrl->pending_cmds[i].state != XHCI_CMD_POSTED ||
            ctrl->pending_cmds[i].owner != slot) {
            continue;
        }
        uint64_t age = now - ctrl->pending_cmds[i].posted_at;
        if (!found || age > oldest) {
            oldest = age;
            type   = ctrl->pending_cmds[i].trb_type;
            found  = true;
        }
    }
    spin_unlock(&ctrl->pending_lock);

    if (found) {
        if (out_trb_type) *out_trb_type = type;
        if (out_age_ms)   *out_age_ms   = (uint32_t)cpu_tsc_to_ms(oldest);
    }
    return found;
}

void xhci_command_abort_if_wanted(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->op_regs) {
        return;
    }
    if (__atomic_exchange_n(&ctrl->cmd_abort_wanted, 0u, __ATOMIC_ACQ_REL) == 0) {
        return;
    }

    volatile uint32_t* crcr_lo = (volatile uint32_t*)&ctrl->op_regs->crcr;

    ctrl->op_regs->crcr = ctrl->op_regs->crcr | XHCI_CRCR_CA;

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(XHCI_CMD_ABORT_TIMEOUT_MS);
    bool stopped = false;
    while ((int64_t)(rdtsc() - deadline) < 0) {
        xhci_process_events();
        if ((*crcr_lo & XHCI_CRCR_CRR) == 0) {
            stopped = true;
            break;
        }
        cpu_pause();
    }

    if (!stopped) {
        kprintf("[xHCI %s] the command ring would not stop when asked to "
                "(CRCR=0x%08x USBSTS=0x%08x) — this controller is out of "
                "service\n", ctrl->name, *crcr_lo, ctrl->op_regs->usbsts);
        ctrl->running     = false;
        ctrl->error_state = true;
    }

    spin_lock(&ctrl->pending_lock);
    xhci_device_slot_t* orphans[XHCI_CMD_RING_TRBS];
    unsigned orphan_count = 0;
    for (uint32_t i = 0; i < XHCI_CMD_RING_TRBS; i++) {
        if (ctrl->pending_cmds[i].state != XHCI_CMD_POSTED) {
            continue;
        }
        xhci_device_slot_t* owner = ctrl->pending_cmds[i].owner;
        ctrl->pending_cmds[i].state = XHCI_CMD_FREE;
        ctrl->pending_cmds[i].owner = NULL;
        if (owner && orphan_count < XHCI_CMD_RING_TRBS) {
            bool already = false;
            for (unsigned k = 0; k < orphan_count; k++) {
                if (orphans[k] == owner) { already = true; break; }
            }
            if (!already) {
                orphans[orphan_count++] = owner;
            }
        }
    }

    ctrl->last_cmd_answer = 0;
    ctrl->cmd_nudges      = 0;

    ctrl->command_ring.enqueue_idx = 0;
    ctrl->command_ring.dequeue_idx = 0;
    ctrl->command_ring.cycle_state = 1;
    if (ctrl->command_ring.trbs) {
        memset(ctrl->command_ring.trbs, 0,
               (size_t)ctrl->command_ring.num_trbs * sizeof(xhci_trb_t));
        xhci_trb_t* link = &ctrl->command_ring.trbs[ctrl->command_ring.num_trbs - 1];
        link->parameter = ctrl->command_ring.trbs_phys;
        link->status    = 0;
        link->control   = TRB_SET_TYPE(TRB_TYPE_LINK) | TRB_TC;
    }
    spin_unlock(&ctrl->pending_lock);

    __sync_synchronize();
    ctrl->op_regs->crcr = ctrl->command_ring.trbs_phys | XHCI_CRCR_RCS;

    for (unsigned k = 0; k < orphan_count; k++) {
        xhci_device_slot_t* s = orphans[k];
        kprintf("[xHCI %s]   port %u (slot %u): controller says slot %s, "
                "address %u; PORTSC 0x%08x; the port was %s\n",
                ctrl->name, s->port_num, s->slot_id,
                xhci_slot_state_name(xhci_slot_context_state(s)),
                xhci_slot_context_address(s),
                xhci_get_port_status(ctrl, s->port_num),
                xhci_port_reset_kind_name(s->reset_kind));
    }

    for (unsigned k = 0; k < orphan_count; k++) {
        xhci_slot_retire(ctrl, orphans[k]);
    }

    kprintf("[xHCI %s] command ring restarted; %u device(s) released\n",
            ctrl->name, orphan_count);
}

void xhci_check_command_timeouts(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->running) {
        return;
    }

    uint64_t now    = rdtsc();
    uint64_t budget = cpu_ms_to_tsc(XHCI_CMD_TIMEOUT_MS);

    spin_lock(&ctrl->pending_lock);

    unsigned outstanding = 0;
    uint64_t oldest_at   = now;
    uint8_t  oldest_type = 0;
    uint8_t  oldest_slot = 0;

    xhci_device_slot_t* waiting[XHCI_CMD_RING_TRBS];
    unsigned waiting_count = 0;

    for (uint32_t i = 0; i < XHCI_CMD_RING_TRBS; i++) {
        if (ctrl->pending_cmds[i].state != XHCI_CMD_POSTED) {
            continue;
        }
        outstanding++;
        if ((int64_t)(ctrl->pending_cmds[i].posted_at - oldest_at) < 0) {
            oldest_at   = ctrl->pending_cmds[i].posted_at;
            oldest_type = ctrl->pending_cmds[i].trb_type;
            oldest_slot = ctrl->pending_cmds[i].slot_id;
        }

        xhci_device_slot_t* owner = ctrl->pending_cmds[i].owner;
        if (owner && waiting_count < XHCI_CMD_RING_TRBS) {
            bool already = false;
            for (unsigned k = 0; k < waiting_count; k++) {
                if (waiting[k] == owner) { already = true; break; }
            }
            if (!already) {
                waiting[waiting_count++] = owner;
            }
        }
    }

    if (outstanding == 0) {
        ctrl->last_cmd_answer = 0;
        spin_unlock(&ctrl->pending_lock);
        return;
    }

    bool stuck = (int64_t)(now - ctrl->last_cmd_answer) > (int64_t)budget;
    uint32_t silent = (uint32_t)cpu_tsc_to_ms(now - ctrl->last_cmd_answer);
    uint32_t oldest_ms = (uint32_t)cpu_tsc_to_ms(now - oldest_at);
    spin_unlock(&ctrl->pending_lock);

    if (!stuck) {
        return;
    }

    unsigned gone = 0;
    for (unsigned k = 0; k < waiting_count; k++) {
        xhci_device_slot_t* s = waiting[k];
        uint8_t port = s->port_num;

        if (!xhci_slot_still_is(s, xhci_slot_epoch(s))) {
            gone++;
            continue;
        }
        if (port != 0 && xhci_port_says_gone(ctrl, port)) {
            kprintf("[xHCI %s] port %u (slot %u): the port reports nothing "
                    "attached — the command it owes an answer for is owed by "
                    "a device that has left\n", ctrl->name, port, s->slot_id);
            xhci_slot_retire(ctrl, s);
            gone++;
        }
    }

    bool nobody_left_to_answer = (waiting_count > 0 && gone == waiting_count);

    if (nobody_left_to_answer && ctrl->cmd_nudges < XHCI_CMD_NUDGES) {
        kprintf("[xHCI %s] %u command(s) outstanding after %u ms, and every "
                "one of them is owed by a device that has left — not ringing "
                "the doorbell, there is nobody behind it\n",
                ctrl->name, outstanding, silent);
        ctrl->cmd_nudges = XHCI_CMD_NUDGES;
    }

    if (ctrl->cmd_nudges < XHCI_CMD_NUDGES) {
        ctrl->cmd_nudges++;
        xhci_ring_t* er = &ctrl->event_ring;
        uint32_t at_dequeue = er->trbs ? er->trbs[er->dequeue_idx].control : 0;

        kprintf("[xHCI %s] nothing answered for %u ms with %u command(s) "
                "outstanding (oldest %s on slot %u, %u ms) — ringing the "
                "doorbell again [%u/%u]\n",
                ctrl->name, silent, outstanding,
                xhci_command_name(oldest_type), oldest_slot, oldest_ms,
                ctrl->cmd_nudges, XHCI_CMD_NUDGES);

        kprintf("[xHCI %s]   CRCR=0x%08x USBSTS=0x%08x ERDP=0x%08x "
                "IMAN=0x%08x | command ring at %u | event ring at %u "
                "expecting cycle %u, TRB there 0x%08x | %u drain(s) skipped\n",
                ctrl->name,
                (uint32_t)ctrl->op_regs->crcr, ctrl->op_regs->usbsts,
                (uint32_t)ctrl->runtime_regs->interrupters[0].erdp,
                ctrl->runtime_regs->interrupters[0].iman,
                ctrl->command_ring.enqueue_idx,
                er->dequeue_idx, er->cycle_state, at_dequeue,
                __atomic_load_n(&ctrl->drain_skips, __ATOMIC_RELAXED));

        __sync_synchronize();
        ctrl->doorbells->doorbells[0].doorbell = 0;

        ctrl->last_cmd_answer = now;
        return;
    }

    if (nobody_left_to_answer) {
        kprintf("[xHCI %s] the command ring is being taken back — it is "
                "holding commands for devices that are not there any more\n",
                ctrl->name);
    } else {
        kprintf("[xHCI %s] still nothing after %u doorbell(s) — the command "
                "ring has stopped and is being taken back\n",
                ctrl->name, XHCI_CMD_NUDGES);
    }

    ctrl->cmd_nudges = 0;
    __atomic_store_n(&ctrl->cmd_abort_wanted, 1u, __ATOMIC_RELEASE);
}