#include "xhci_interrupt.h"
#include "xhci.h"
#include "xhci_regs.h"
#include "xhci_rings.h"
#include "xhci_trb.h"
#include "xhci_port.h"
#include "xhci_command.h"
#include "xhci_transfer.h"
#include "xhci_enumeration.h"
#include "klib.h"

void xhci_process_events(void) {
    xhci_controller_t* ctrl = xhci_get_controller();
    if (!ctrl || !ctrl->running) {
        return;
    }

    xhci_ring_t* event_ring = &ctrl->event_ring;
    xhci_interrupter_regs_t* intr0 = &ctrl->runtime_regs->interrupters[0];

    while (1) {
        xhci_trb_t* trb = &event_ring->trbs[event_ring->dequeue_idx];

        uint8_t trb_cycle = trb->control & TRB_C;
        if (trb_cycle != event_ring->cycle_state) {
            break;
        }

        uint8_t trb_type = TRB_GET_TYPE(trb->control);

        switch (trb_type) {
            case TRB_TYPE_PORT_STATUS_CHANGE:
                /* Port change events are handled in xhci_irq_handler via PORTSC. */
                break;

            case TRB_TYPE_COMMAND_COMPLETION:
                xhci_handle_command_completion(ctrl, trb);
                break;

            case TRB_TYPE_TRANSFER_EVENT:
                xhci_handle_transfer_event(ctrl, trb);
                break;

            default:
                debug_printf("[xHCI] Unknown event TRB type: %u\n", trb_type);
                break;
        }

        event_ring->dequeue_idx++;
        if (event_ring->dequeue_idx >= event_ring->num_trbs) {
            event_ring->dequeue_idx = 0;
            event_ring->cycle_state ^= 1;
        }
    }

    /* Update ERDP and clear EHB (Event Handler Busy) by writing 1 to bit 3. */
    uint64_t new_erdp = event_ring->trbs_phys +
                        (event_ring->dequeue_idx * sizeof(xhci_trb_t));
    new_erdp |= XHCI_ERDP_EHB;
    intr0->erdp = new_erdp;
}

void xhci_poll_events(void) {
    xhci_controller_t* ctrl = xhci_get_controller();
    if (!ctrl || !ctrl->use_polling) {
        return;
    }
    xhci_process_events();
}

void xhci_irq_handler(void) {
    xhci_controller_t* ctrl = xhci_get_controller();
    if (!ctrl || !ctrl->running) {
        return;
    }

    uint32_t usbsts = ctrl->op_regs->usbsts;

    /* Clear W1C status bits immediately before processing. */
    ctrl->op_regs->usbsts = usbsts & (XHCI_STS_HSE | XHCI_STS_EINT |
                                      XHCI_STS_PCD);

    if (usbsts & XHCI_STS_HSE) {
        debug_printf("[xHCI IRQ] Host System Error\n");
        ctrl->error_state = true;
    }

    if (usbsts & XHCI_STS_PCD) {
        for (uint8_t port = 1; port <= ctrl->max_ports; port++) {
            uint32_t portsc = xhci_get_port_status(ctrl, port);
            uint32_t change_bits = portsc & XHCI_PORTSC_W1C_MASK;

            if (change_bits == 0) {
                continue;
            }

            xhci_port_clear_change_bits(ctrl, port, change_bits);

            if ((portsc & XHCI_PORTSC_CSC) && !(portsc & XHCI_PORTSC_CCS)) {
                debug_printf("[xHCI] Device disconnected on port %u\n", port);
                xhci_device_slot_t* slot = xhci_get_device_slot_by_port(port);
                if (slot) {
                    xhci_post_disable_slot_cmd(ctrl, slot->slot_id);
                    xhci_device_slot_cleanup(ctrl, slot);
                }
            } else if ((portsc & XHCI_PORTSC_CSC) && (portsc & XHCI_PORTSC_CCS)) {
                debug_printf("[xHCI] Device connected on port %u\n", port);
                xhci_enumerate_device(ctrl, port);
            }
        }
    }

    if (usbsts & XHCI_STS_EINT) {
        xhci_process_events();
    }

    /* Clear IMAN IP (Interrupt Pending) bit — write 1 to clear. */
    ctrl->runtime_regs->interrupters[0].iman |= XHCI_IMAN_IP;
}
