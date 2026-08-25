#include "xhci.h"
#include "xhci_transfer.h"
#include "xhci_command.h"
#include "xhci_enumeration.h"
#include "xhci_device.h"
#include "xhci_trb.h"
#include "xhci_hid.h"
#include "xhci_endpoint.h"
#include "xhci_hub.h"
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

    /* EP0 is entered in the endpoint table as well, sharing the ring rather
     * than owning it — the table's teardown starts at DCI 2 and will not
     * double free it. It is there so that a driver can wait on a control
     * transfer the same way it waits on a bulk one: enumeration is not the
     * only thing that ever needs to ask a device a question. */
    if (slot->endpoints) {
        slot->endpoints[1].ring       = ring;
        slot->endpoints[1].type       = XHCI_EP_TYPE_CONTROL;
        slot->endpoints[1].max_packet = slot->ep0_max_packet;
        slot->endpoints[1].active     = true;
        slot->endpoints[1].xfer_state = XHCI_XFER_IDLE;
    }

    return 0;
}

/*
 * A control transfer with somebody waiting for the answer.
 *
 * Enumeration never needs this: each of its steps is driven by the completion
 * of the last, and the state machine is the thing that carries it forward. A
 * class driver is in the opposite position — it is running as ordinary kernel
 * code with a question to ask and nothing to do until the device answers.
 *
 * The two are told apart at the event, by whether anything is waiting on EP0.
 */
int xhci_control_transfer_sync(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                               usb_setup_packet_t* setup, uint64_t data_phys,
                               uint16_t data_len, bool data_in,
                               uint32_t timeout_ms)
{
    if (!ctrl || !slot || !slot->endpoints) {
        return -1;
    }

    xhci_endpoint_t* ep0 = &slot->endpoints[1];
    if (ep0->xfer_state == XHCI_XFER_IN_FLIGHT) {
        return -2;
    }

    /* Zero means "the next completion on this endpoint is mine". A control
     * transfer raises exactly one, from its Status Stage. */
    ep0->xfer_trb_phys = 0;
    ep0->xfer_state    = XHCI_XFER_IN_FLIGHT;

    if (xhci_control_transfer(ctrl, slot, setup, data_phys, data_len, data_in) < 0) {
        ep0->xfer_state = XHCI_XFER_IDLE;
        return -1;
    }

    uint32_t residual = 0;
    int code = xhci_ep_wait(ctrl, slot, 1, timeout_ms, &residual);

    /*
     * A device is allowed to say no, and the way it says no is to halt the pipe
     * the question came down.
     *
     * Enumeration knows this and clears the halt before its next step. Nothing
     * that called this did, so the first optional request any device refused
     * left EP0 halted and every request after it — from any driver, for the
     * rest of that device's life — failed against a pipe nobody had reopened.
     * The hub class asks a great many questions a hub is entitled to refuse.
     */
    if (code == TRB_COMPLETION_STALL) {
        xhci_ep_recover(ctrl, slot, 1);
        xhci_command_wait_idle(ctrl, timeout_ms);
    }

    /* A device that answered with less than was asked for says so, rather than
     * saying Success and leaving the caller to decide from the contents of a
     * buffer whether the contents are its own. Every caller here already
     * expects the answer; until the data stage carried Interrupt On Short
     * Packet there was nothing that could give it. */
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

/*
 * A control transfer is THREE transfer descriptors, not one.
 *
 * The Setup Stage, the optional Data Stage and the Status Stage are separate
 * TDs, and the Chain bit is what joins TRBs *within* one TD — a scatter-gather
 * data stage, say. The old code set Chain on the Setup and Data stages, which
 * welded all three into a single TD, and that is not a cosmetic difference:
 *
 *   when a device returns fewer bytes than were asked for, the controller
 *   finishes the current TD early and skips the rest of it.
 *
 * With all three welded together, "the rest of it" is the Status Stage — the
 * only TRB carrying Interrupt On Completion. The device answers correctly, the
 * controller does exactly what the specification says, and the driver waits
 * forever for an event that was skipped along with the stage that would have
 * raised it. Asking a device for more descriptor than it has is not an
 * unusual thing to do; it is how you find out how much it has.
 *
 * So: no Chain anywhere, and Interrupt On Completion on the Status Stage.
 *
 * ‼ Interrupt On Short Packet IS set on the Data Stage, and it used to not be.
 * The reasoning for leaving it off was that "every descriptor states its own
 * length" — which is true of a descriptor that arrived, and equally true of the
 * one still lying in the scratch page from the read before it. Without ISP a
 * data stage that comes up short raises no event at all: the only event is the
 * status stage's, and a status stage moves zero bytes, so its length field says
 * nothing about the data. The state machine therefore had NO WAY, by
 * construction, to tell a descriptor it had read from one it had not, and
 * parsed whatever was there. Nothing under emulation ever answers a descriptor
 * read short, which is why it looked correct for as long as it was only ever
 * run against one.
 *
 * The price is that a short control transfer now raises two events, so the
 * TRBs of both stages are written down and an answer says which one it is
 * answering.
 */
int xhci_control_transfer(xhci_controller_t* ctrl,
                          xhci_device_slot_t* slot,
                          usb_setup_packet_t* setup,
                          uint64_t data_buffer_phys,
                          uint16_t data_length,
                          bool data_in) {
    if (!ctrl || !slot || !setup || !slot->ep0_ring) {
        return -1;
    }

    xhci_ring_t* ring = slot->ep0_ring;

    /* What is being asked for, before it is asked. "Received" starts at the
     * full length because that is exactly what "no short packet was reported"
     * will mean when the status stage arrives on its own. */
    slot->ctl_data_trb   = 0;
    slot->ctl_status_trb = 0;
    slot->ctl_requested  = data_length;
    slot->ctl_received   = data_length;

    /* Transfer Type in the Setup Stage: 0 = no data, 2 = OUT data, 3 = IN. */
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

    /* The Status Stage runs opposite to the data: an IN data stage is
     * acknowledged with an OUT status, and a transfer with no data at all is
     * acknowledged IN. */
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

    __sync_synchronize();
    ctrl->doorbells->doorbells[slot->slot_id].doorbell = 1;

    return 0;
}

void xhci_handle_transfer_event(xhci_controller_t* ctrl, xhci_trb_t* event) {
    if (!ctrl || !event) {
        return;
    }

    uint8_t  slot_id     = (event->control >> 24) & 0xFF;
    uint8_t  endpoint_id = (event->control >> 16) & 0x1F;
    uint8_t  code        = (event->status >> 24) & 0xFF;
    /* The length field of a transfer event is what was NOT transferred. */
    uint32_t residual    = event->status & 0x00FFFFFFu;
    uint64_t trb_phys    = event->parameter;

    xhci_device_slot_t* slot = xhci_get_device_slot(ctrl, slot_id);
    if (!slot) {
        /*
         * An answer to a transfer nobody is left to hear about.
         *
         * Ordinary once — a device unplugged with a transfer in flight — and
         * a symptom when it is not: whatever posted that transfer is now
         * waiting for an event that has been delivered and thrown away. Said
         * out loud for the same reason as everywhere else in this driver:
         * on the machine where it matters there is no debug build, there is a
         * screen, and a line that was never printed cannot be read off it.
         */
        kprintf("[xHCI %s] a transfer on slot %u endpoint %u was answered (%s) "
                "and there is no such device\n",
                ctrl->name, slot_id, endpoint_id, xhci_completion_name(code));
        return;
    }

    bool ok = (code == TRB_COMPLETION_SUCCESS || code == TRB_COMPLETION_SHORT_PKT);

    /* EP0 is always DCI 1. Test it first, so that a device whose interrupt
     * endpoint somehow reported the same number cannot divert control
     * transfers into another path. */
    if (endpoint_id == 1) {
        /*
         * How much of the data stage actually arrived.
         *
         * A control transfer whose data stage comes up short raises this event
         * from the Data Stage TRB, and the status stage still raises its own
         * afterwards. So this one records the length and gets out of the way —
         * the transfer is not over, and completing anything on the strength of
         * it would end the wait one event early.
         */
        if (code == TRB_COMPLETION_SHORT_PKT && trb_phys != 0 &&
            trb_phys == slot->ctl_data_trb) {
            slot->ctl_received = (residual <= slot->ctl_requested)
                               ? (uint16_t)(slot->ctl_requested - residual)
                               : 0;
            return;
        }

        /* Anything else that names the data stage is that stage failing, and
         * the status stage will not run. It is the answer. */

        /* Somebody asked this question themselves and is waiting for it. */
        if (slot->endpoints &&
            slot->endpoints[1].xfer_state == XHCI_XFER_IN_FLIGHT) {
            xhci_ep_complete(slot, 1, code, residual, 0);
            return;
        }

        if (!ok) {
            /* An optional class request the device does not implement. Clear
             * the pipe and step over it — that is the answer.
             *
             * Also where a step this driver could do without ends up once it
             * has been asked for as many times as it is going to be: a
             * keyboard that will not answer Set Idle is still a keyboard, and
             * throwing it away over a request it never had to implement is
             * the fault this whole branch exists to avoid. */
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

            /* A step enumeration cannot do without, and a fault that leaves
             * the pipe halted rather than the device broken. Clear the pipe
             * and ask again: a device whose bus was disturbed while it was
             * answering fails once and answers the second time. */
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

        /* Every step of enumeration lives in one place. */
        xhci_enum_advance_state(ctrl, slot, slot_id, TRB_COMPLETION_SUCCESS);
        return;
    }

    if (!slot->endpoints || endpoint_id > XHCI_MAX_DCI) {
        return;
    }

    /* A hub reporting that something below it changed. What changed can only
     * be found out with control transfers, so this raises a flag and re-arms;
     * the finding out happens somewhere that is allowed to wait. */
    if (slot->driver == XHCI_DRIVER_HUB &&
        endpoint_id == slot->ep_interrupt_in) {

        /* Note it, and DO NOT re-arm here.
         *
         * A hub goes on reporting for as long as a port change is outstanding,
         * and clearing that change takes control transfers, which cannot
         * happen in this handler. Re-arming from here therefore asks the hub
         * to tell us again immediately — and it does, without pause, forever.
         * Measured: the core stopped reaching the idle loop altogether, which
         * is precisely where the change would have been dealt with. An
         * interrupt storm that starves the only context able to end it.
         *
         * The endpoint is re-armed by the service pass, once it has something
         * new to say. */
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

    /* The keyboard is the one endpoint nobody waits on: its reports arrive
     * unasked and the only right response is to take the report and hold the
     * endpoint open again. Everything else has a caller waiting, and the
     * completion is recorded for it. */
    if (slot->driver == XHCI_DRIVER_KEYBOARD &&
        endpoint_id == slot->ep_interrupt_in) {

        xhci_endpoint_t* ep = &slot->endpoints[endpoint_id];

        if (ok) {
            xhci_process_keyboard_report(
                (usb_boot_keyboard_report_t*)ep->buffer_virt);
        } else if (code == TRB_COMPLETION_STALL) {
            xhci_ep_recover(ctrl, slot, endpoint_id);
        } else {
            /* Transaction errors are what a marginal cable looks like, and the
             * controller has already retried them CErr times. Re-arming is the
             * right answer; going quiet is not. */
            debug_printf("[xHCI] slot %u keyboard completion %u\n", slot_id, code);
        }

        /* Re-arm after any recovery above, never before it: a Set TR Dequeue
         * names where the controller resumes, and the transfer queued here
         * lands on exactly that TRB. */
        ep->xfer_state = XHCI_XFER_IDLE;
        xhci_ep_submit(ctrl, slot, endpoint_id, ep->buffer_phys, ep->max_packet);
        return;
    }

    xhci_ep_complete(slot, endpoint_id, code, residual, trb_phys);
}
