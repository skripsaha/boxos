#include "xhci.h"
#include "xhci_transfer.h"
#include "xhci_command.h"
#include "xhci_enumeration.h"
#include "xhci_device.h"
#include "xhci_trb.h"
#include "xhci_hid.h"
#include "xhci_endpoint.h"
#include "xhci_hub.h"
#include "xhci_interrupt.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "atomics.h"

static uint64_t pack_setup_packet(usb_setup_packet_t* setup) {
    uint64_t param = 0;
    param |= (uint64_t)setup->bmRequestType;
    param |= (uint64_t)setup->bRequest << 8;
    param |= (uint64_t)setup->wValue << 16;
    param |= (uint64_t)setup->wIndex << 32;
    param |= (uint64_t)setup->wLength << 48;
    return param;
}

int xhci_alloc_ep0_ring(xhci_controller_t* ctrl, xhci_device_slot_t* slot) {
    (void)ctrl;
    if (!slot) {
        return -1;
    }

    void* ring_phys = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    if (!ring_phys) {
        debug_printf("[xHCI TRANSFER] Failed to allocate EP0 ring structure\n");
        return -1;
    }

    xhci_ring_t* ring = (xhci_ring_t*)vmm_phys_to_virt((uintptr_t)ring_phys);
    if (!ring) {
        pmm_free(ring_phys, 1);
        return -1;
    }

    if (xhci_ring_init(ring, 32, true) != 0) {
        debug_printf("[xHCI TRANSFER] Failed to init EP0 ring\n");
        pmm_free(ring_phys, 1);
        return -1;
    }

    slot->ep0_ring = ring;
    slot->ep0_ring_phys = (uint64_t)ring_phys;

    void* desc_phys = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    if (!desc_phys) {
        xhci_ring_destroy(ring);
        pmm_free(ring_phys, 1);
        slot->ep0_ring = NULL;
        slot->ep0_ring_phys = 0;
        debug_printf("[xHCI TRANSFER] Failed to allocate descriptor buffer\n");
        return -1;
    }

    slot->descriptor_buffer_virt = vmm_phys_to_virt((uintptr_t)desc_phys);
    slot->descriptor_buffer_phys = (uint64_t)desc_phys;

    if (slot->endpoints) {
        slot->endpoints[1].ring       = ring;
        slot->endpoints[1].type       = XHCI_EP_TYPE_CONTROL;
        slot->endpoints[1].max_packet = slot->ep0_max_packet;
        slot->endpoints[1].active     = true;
        slot->endpoints[1].xfer_state = XHCI_XFER_IDLE;
    }

    return 0;
}

int xhci_control_transfer_sync(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                               usb_setup_packet_t* setup, uint64_t data_phys,
                               uint16_t data_len, bool data_in,
                               uint32_t timeout_ms)
{
    if (!ctrl || !slot || !slot->endpoints) {
        return -1;
    }

    if (xhci_drain_is_mine(ctrl)) {
        kprintf("[xHCI %s] port %u: a control transfer was waited for from "
                "inside the event drain — its answer cannot arrive until this "
                "returns\n", ctrl->name, slot->port_num);
        return -3;
    }

    xhci_endpoint_t* ep0 = &slot->endpoints[1];
    if (ep0->xfer_state == XHCI_XFER_IN_FLIGHT) {
        return -2;
    }

    ep0->xfer_trb_phys = 0;
    ep0->xfer_state    = XHCI_XFER_IN_FLIGHT;

    if (xhci_control_transfer(ctrl, slot, setup, data_phys, data_len, data_in) < 0) {
        ep0->xfer_state = XHCI_XFER_IDLE;
        return -1;
    }

    uint32_t residual = 0;
    int code = xhci_ep_wait(ctrl, slot, 1, timeout_ms, &residual);

    if (code == TRB_COMPLETION_STALL) {
        xhci_ep_recover(ctrl, slot, 1);
        xhci_command_wait_idle(ctrl, timeout_ms);
    }

    if (code == TRB_COMPLETION_SUCCESS && slot->ctl_received < slot->ctl_requested) {
        return TRB_COMPLETION_SHORT_PKT;
    }
    return code;
}

void xhci_free_ep0_ring(xhci_device_slot_t* slot) {
    if (!slot) {
        return;
    }

    if (slot->ep0_ring) {
        xhci_ring_destroy(slot->ep0_ring);
        if (slot->ep0_ring_phys) {
            pmm_free((void*)slot->ep0_ring_phys, 1);
        }
        slot->ep0_ring = NULL;
        slot->ep0_ring_phys = 0;
    }

    if (slot->descriptor_buffer_phys) {
        pmm_free((void*)slot->descriptor_buffer_phys, 1);
        slot->descriptor_buffer_virt = NULL;
        slot->descriptor_buffer_phys = 0;
    }
}

static int control_transfer_post(xhci_controller_t* ctrl,
                                 xhci_device_slot_t* slot,
                                 usb_setup_packet_t* setup,
                                 uint64_t data_buffer_phys,
                                 uint16_t data_length,
                                 bool data_in) {
    if (!ctrl || !slot || !setup || !slot->ep0_ring) {
        return -1;
    }

    xhci_ring_t* ring = slot->ep0_ring;

    uint32_t needed = (data_length > 0) ? 3u : 2u;
    if (xhci_ring_space(ring) < needed) {
        kprintf("[xHCI %s] port %u: the control ring has no room for a %u-stage "
                "transfer — %u slot(s) free\n",
                ctrl->name, slot->port_num, needed, xhci_ring_space(ring));
        return -1;
    }

    slot->ctl_data_trb   = 0;
    slot->ctl_status_trb = 0;
    slot->ctl_requested  = data_length;
    slot->ctl_received   = data_length;

    uint32_t trt = 0;
    if (data_length > 0) {
        trt = data_in ? 3 : 2;
    }

    xhci_trb_t setup_trb = {0};
    setup_trb.parameter = pack_setup_packet(setup);
    setup_trb.status = 8;
    setup_trb.control = TRB_SET_TYPE(TRB_TYPE_SETUP_STAGE) | TRB_IDT | (trt << 16);
    if (xhci_ring_enqueue(ring, &setup_trb) == 0) {
        debug_printf("[xHCI TRANSFER] EP0 ring full posting Setup Stage\n");
        return -1;
    }

    if (data_length > 0) {
        xhci_trb_t data_trb = {0};
        data_trb.parameter = data_buffer_phys;
        data_trb.status = data_length;
        data_trb.control = TRB_SET_TYPE(TRB_TYPE_DATA_STAGE) | TRB_ISP |
                           (data_in ? (1u << 16) : 0);
        uint64_t data_phys = xhci_ring_enqueue(ring, &data_trb);
        if (data_phys == 0) {
            debug_printf("[xHCI TRANSFER] EP0 ring full posting Data Stage\n");
            return -1;
        }
        slot->ctl_data_trb = data_phys;
    }

    bool status_in = (data_length == 0) || !data_in;
    xhci_trb_t status_trb = {0};
    status_trb.control = TRB_SET_TYPE(TRB_TYPE_STATUS_STAGE) | TRB_IOC |
                         (status_in ? (1u << 16) : 0);
    uint64_t status_phys = xhci_ring_enqueue(ring, &status_trb);
    if (status_phys == 0) {
        debug_printf("[xHCI TRANSFER] EP0 ring full posting Status Stage\n");
        return -1;
    }
    slot->ctl_status_trb = status_phys;
    return 0;
}

int xhci_control_transfer(xhci_controller_t* ctrl,
                          xhci_device_slot_t* slot,
                          usb_setup_packet_t* setup,
                          uint64_t data_buffer_phys,
                          uint16_t data_length,
                          bool data_in) {
    if (control_transfer_post(ctrl, slot, setup, data_buffer_phys,
                              data_length, data_in) < 0) {
        return -1;
    }

    __sync_synchronize();
    ctrl->doorbells->doorbells[slot->slot_id].doorbell = 1;
    return 0;
}

#if CONFIG_XHCI_CTRL_GIVEUP_PROOF
void xhci_ctrl_giveup_proof(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    static bool already = false;

    if (already || !ctrl || !slot) {
        return;
    }
    already = true;

    if (!slot->endpoints || !slot->descriptor_buffer_phys ||
        !slot->descriptor_buffer_virt) {
        kprintf("[xHCI PROOF] slot %u has no control scratch buffer "
                "(phys=0x%llx) — the give-up proof did NOT run\n",
                slot->slot_id,
                (unsigned long long)slot->descriptor_buffer_phys);
        return;
    }

    xhci_endpoint_t* ep0 = &slot->endpoints[1];
    if (ep0->xfer_state == XHCI_XFER_IN_FLIGHT) {
        kprintf("[xHCI PROOF] EP0 was busy — the give-up proof did not run\n");
        return;
    }

    kprintf("[xHCI PROOF] a control transfer is posted and not rung for, so "
            "no answer can come\n");

    usb_setup_packet_t refused = {
        .bmRequestType = 0x80, .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = 0xFF00, .wIndex = 0, .wLength = 8
    };

    ep0->xfer_trb_phys = 0;
    ep0->xfer_state    = XHCI_XFER_IN_FLIGHT;
    if (control_transfer_post(ctrl, slot, &refused,
                              slot->descriptor_buffer_phys, 8, true) < 0) {
        ep0->xfer_state = XHCI_XFER_IDLE;
        kprintf("[xHCI PROOF] it would not post — the proof did not run\n");
        return;
    }

    int code = xhci_ep_wait(ctrl, slot, 1, XHCI_CTRL_GIVEUP_PROOF_MS, NULL);
    kprintf("[xHCI PROOF] the wait ended with %d\n", code);

    usb_setup_packet_t cfg = {
        .bmRequestType = 0x80, .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = 0x0200, .wIndex = 0, .wLength = 9
    };
    int rc = xhci_control_transfer_sync(ctrl, slot, &cfg,
                                        slot->descriptor_buffer_phys, 9, true,
                                        XHCI_CTRL_GIVEUP_PROOF_MS);

    kprintf("[xHCI PROOF] the next control transfer asked something the device "
            "answers and got %s\n",
            (rc == TRB_COMPLETION_STALL) ? "the REFUSAL meant for the "
                                           "abandoned one"
                                         : "its own answer");
}
#endif

void xhci_handle_transfer_event(xhci_controller_t* ctrl, xhci_trb_t* event) {
    if (!ctrl || !event) {
        return;
    }

    uint8_t  slot_id     = (event->control >> 24) & 0xFF;
    uint8_t  endpoint_id = (event->control >> 16) & 0x1F;
    uint8_t  code        = (event->status >> 24) & 0xFF;
    uint32_t residual    = event->status & 0x00FFFFFFu;
    uint64_t trb_phys    = event->parameter;

    xhci_device_slot_t* slot = xhci_get_device_slot(ctrl, slot_id);
    if (!slot) {
        kprintf("[xHCI %s] a transfer on slot %u endpoint %u was answered (%s) "
                "and there is no such device\n",
                ctrl->name, slot_id, endpoint_id, xhci_completion_name(code));
        return;
    }

    bool ok = (code == TRB_COMPLETION_SUCCESS || code == TRB_COMPLETION_SHORT_PKT);

    if (endpoint_id == 1) {
        xhci_ring_reclaim_to(slot->ep0_ring, trb_phys);
    } else if (slot->endpoints && endpoint_id <= XHCI_MAX_DCI) {
        xhci_ring_reclaim_to(slot->endpoints[endpoint_id].ring, trb_phys);
    }

    if (endpoint_id == 1) {
        if (code == TRB_COMPLETION_SHORT_PKT && trb_phys != 0 &&
            trb_phys == slot->ctl_data_trb) {
            slot->ctl_received = (residual <= slot->ctl_requested)
                               ? (uint16_t)(slot->ctl_requested - residual)
                               : 0;
            return;
        }


        if (slot->endpoints &&
            slot->endpoints[1].xfer_state == XHCI_XFER_IN_FLIGHT) {
            xhci_ep_complete(slot, 1, code, residual, 0);
            return;
        }

        if (slot->state == ENUM_STATE_WAIT_EP0_STOP &&
            (code == TRB_COMPLETION_STOPPED ||
             code == TRB_COMPLETION_STOPPED_LENGTH)) {
            return;
        }

        if (!ok) {
            if (xhci_enum_stall_is_tolerable(slot->state) &&
                (code == TRB_COMPLETION_STALL ||
                 slot->step_retry >= XHCI_STEP_RETRIES)) {
                kprintf("[xHCI %s] port %u: %s — %s; it is optional, carrying "
                        "on without it\n", ctrl->name, slot->port_num,
                        xhci_enum_state_name(slot->state),
                        xhci_completion_name(code));
                xhci_enum_recover_ep0(ctrl, slot, slot->state, false);
                return;
            }

            if (xhci_enum_fault_is_retryable(code) &&
                xhci_enum_step_can_be_asked_again(slot->state) &&
                slot->step_retry < XHCI_STEP_RETRIES) {
                slot->step_retry++;
                kprintf("[xHCI %s] port %u: %s — %s; clearing the control "
                        "pipe and asking again (attempt %u of %u)\n",
                        ctrl->name, slot->port_num,
                        xhci_enum_state_name(slot->state),
                        xhci_completion_name(code),
                        slot->step_retry, XHCI_STEP_RETRIES);
                xhci_enum_recover_ep0(ctrl, slot, slot->state, true);
                return;
            }

            kprintf("[xHCI %s] port %u: %s failed — %s (code %u); releasing "
                    "the slot\n", ctrl->name, slot->port_num,
                    xhci_enum_state_name(slot->state),
                    xhci_completion_name(code), code);
            xhci_slot_retire(ctrl, slot);
            return;
        }

        xhci_enum_advance_state(ctrl, slot, slot_id, TRB_COMPLETION_SUCCESS);
        return;
    }

    if (!slot->endpoints || endpoint_id > XHCI_MAX_DCI) {
        return;
    }

    if (slot->driver == XHCI_DRIVER_HUB &&
        endpoint_id == slot->ep_interrupt_in) {

        xhci_endpoint_t* ep = &slot->endpoints[endpoint_id];
        if (code == TRB_COMPLETION_STALL) {
            xhci_ep_recover(ctrl, slot, endpoint_id);
        }
        ep->xfer_state = XHCI_XFER_IDLE;
        if (ok) {
            xhci_hub_note_change(slot);
        }
        return;
    }

    if (slot->driver == XHCI_DRIVER_KEYBOARD &&
        endpoint_id == slot->ep_interrupt_in) {

        xhci_endpoint_t* ep = &slot->endpoints[endpoint_id];

        if (ok) {
            xhci_process_keyboard_report(
                (usb_boot_keyboard_report_t*)ep->buffer_virt);
        } else if (code == TRB_COMPLETION_STALL) {
            xhci_ep_recover(ctrl, slot, endpoint_id);
        } else {
            debug_printf("[xHCI] slot %u keyboard completion %u\n", slot_id, code);
        }

        ep->xfer_state = XHCI_XFER_IDLE;
        xhci_ep_submit(ctrl, slot, endpoint_id, ep->buffer_phys, ep->max_packet);
        return;
    }

    xhci_ep_complete(slot, endpoint_id, code, residual, trb_phys);
}