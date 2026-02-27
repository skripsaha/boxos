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

#define XHCI_TRB_CYCLE           (1 << 0)
#define XHCI_TRB_TYPE(ctrl)      TRB_GET_TYPE(ctrl)

static void xhci_generate_port_event(xhci_controller_t* ctrl, uint8_t port, uint32_t portsc) {
    (void)ctrl;
    debug_printf("[xHCI Event] Port %u changed: PORTSC=0x%08x\n", port, portsc);

    if (portsc & XHCI_PORTSC_CSC) {
        debug_printf("[xHCI Event]   CSC: Connect Status Change\n");
    }
    if (portsc & XHCI_PORTSC_PRC) {
        debug_printf("[xHCI Event]   PRC: Port Reset Change\n");
    }
    if (portsc & XHCI_PORTSC_PEC) {
        debug_printf("[xHCI Event]   PEC: Port Enable/Disable Change\n");
    }
    if (portsc & XHCI_PORTSC_OCC) {
        debug_printf("[xHCI Event]   OCC: Over-Current Change\n");
    }
}

void xhci_process_events(void) {
    xhci_controller_t* ctrl = xhci_get_controller();
    if (!ctrl || !ctrl->running) {
        return;
    }

    xhci_ring_t* event_ring = &ctrl->event_ring;
    uint32_t events_processed = 0;

    while (1) {
        xhci_trb_t* trb = &event_ring->trbs[event_ring->dequeue_idx];

        uint8_t trb_cycle = trb->control & XHCI_TRB_CYCLE;
        if (trb_cycle != event_ring->cycle_state) {
            if (events_processed == 0) {
                debug_printf("[xHCI EVENT] No events (idx=%u cycle_want=%u trb_cycle=%u ctrl=0x%08x)\n",
                             event_ring->dequeue_idx, event_ring->cycle_state, trb_cycle, trb->control);

                // DIAGNOSTIC: Force-read next 4 TRBs to check if events exist elsewhere
                for (uint32_t i = 1; i <= 4; i++) {
                    uint32_t test_idx = (event_ring->dequeue_idx + i) % event_ring->num_trbs;
                    xhci_trb_t *test_trb = &event_ring->trbs[test_idx];
                    uint32_t test_ctrl = test_trb->control;
                    uint8_t test_cycle = test_ctrl & 0x1;
                    uint8_t test_type = (test_ctrl >> 10) & 0x3F;

                    debug_printf("[xHCI EVENT] TRB[+%u]: cycle=%u type=%u ctrl=0x%08x\n",
                                 i, test_cycle, test_type, test_ctrl);
                }
            }
            break;
        }

        uint8_t trb_type = XHCI_TRB_TYPE(trb->control);
        events_processed++;

        switch (trb_type) {
            case TRB_TYPE_PORT_STATUS_CHANGE: {
                uint8_t port = (trb->parameter >> 24) & 0xFF;
                debug_printf("[xHCI] Port Status Change Event: port=%u\n", port);

                if (port > 0 && port <= ctrl->max_ports) {
                    uint32_t portsc = xhci_get_port_status(ctrl, port);
                    xhci_generate_port_event(ctrl, port, portsc);
                }
                break;
            }

            case TRB_TYPE_COMMAND_COMPLETION: {
                debug_printf("[xHCI] Command Completion Event\n");
                xhci_handle_command_completion(ctrl, trb);
                break;
            }

            case TRB_TYPE_TRANSFER_EVENT: {
                debug_printf("[xHCI] Transfer Event\n");
                xhci_handle_transfer_event(ctrl, trb);
                break;
            }

            default:
                debug_printf("[xHCI] Unknown Event TRB type: %u\n", trb_type);
                break;
        }

        event_ring->dequeue_idx++;
        if (event_ring->dequeue_idx >= event_ring->num_trbs) {
            event_ring->dequeue_idx = 0;
            event_ring->cycle_state ^= 1;
        }
    }

    xhci_interrupter_regs_t* intr0 = &ctrl->interrupters[0];
    uint64_t erdp = xhci_ring_get_phys_addr(event_ring);
    erdp += (event_ring->dequeue_idx * sizeof(xhci_trb_t));
    intr0->erdp = erdp;
}

void xhci_poll_events(void) {
    xhci_controller_t* ctrl = xhci_get_controller();

    if (!ctrl) {
        return;
    }

    if (!ctrl->use_polling) {
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

    if (usbsts & XHCI_STS_PCD) {
        debug_printf("[xHCI IRQ] Port Change Detect\n");

        for (uint8_t port = 1; port <= ctrl->max_ports; port++) {
            uint32_t portsc = xhci_get_port_status(ctrl, port);
            uint32_t change_bits = portsc & XHCI_PORTSC_W1C_MASK;

            if (change_bits != 0) {
                debug_printf("[xHCI IRQ] Port %u changed: PORTSC=0x%08x\n", port, portsc);
                xhci_generate_port_event(ctrl, port, portsc);
                xhci_port_clear_change_bits(ctrl, port, change_bits);

                if ((portsc & XHCI_PORTSC_CSC) && (portsc & XHCI_PORTSC_CCS)) {
                    debug_printf("[xHCI IRQ] Device connected on port %u, starting enumeration\n", port);
                    xhci_enumerate_device(ctrl, port);
                }
            }
        }

        ctrl->op_regs->usbsts = XHCI_STS_PCD;
    }

    if (usbsts & XHCI_STS_EINT) {
        debug_printf("[xHCI IRQ] Event Interrupt\n");
        xhci_process_events();
        ctrl->op_regs->usbsts = XHCI_STS_EINT;
    }

    if (usbsts & XHCI_STS_HSE) {
        debug_printf("[xHCI IRQ] Host System Error!\n");
        ctrl->error_state = true;
        ctrl->op_regs->usbsts = XHCI_STS_HSE;
    }

    ctrl->interrupters[0].iman = XHCI_IMAN_IP;
}
