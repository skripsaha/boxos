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
#include "baton.h"

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

    /* Asked before anything is put on the ring: the answer to this would be
     * drained by a pass that cannot run until this call returns, so posting
     * the transfer would leave it in flight with nobody able to collect it. */
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

    /*
     * Room for the WHOLE transfer, checked before any of it is written.
     *
     * The three stages have to arrive together or not at all: a Setup Stage
     * queued with nowhere to put the Status Stage that ends it is a device
     * asked a question that can never be finished, on a ring that now has a
     * partial transfer descriptor in it. Checking per TRB, which is all the
     * ring itself can do, would allow exactly that.
     */
    uint32_t needed = (data_length > 0) ? 3u : 2u;
    if (xhci_ring_space(ring) < needed) {
        kprintf("[xHCI %s] port %u: the control ring has no room for a %u-stage "
                "transfer — %u slot(s) free\n",
                ctrl->name, slot->port_num, needed, xhci_ring_space(ring));
        return -1;
    }

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
    return 0;
}

/*
 * Put the stages on the ring, then tell the controller they are there.
 *
 * The two halves are separate only so that one caller can leave the second
 * one out: the proof below posts a transfer and deliberately does not ring
 * for it, which is the only way to make a control transfer that no answer can
 * come back for on a machine whose emulated devices answer everything.
 */
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
/*
 * ── the one proof that a given-up control transfer is TAKEN BACK ───────────
 *
 * xhci_ep_wait gives up when the device has gone, when the pipe has broken, or
 * — last — when the time ran out. Under emulation none of those happen on the
 * control pipe: the device answers every question immediately, so the give-up
 * path is unreachable. Measured six ways before this was written: a zero
 * budget (the answer arrives inside the first drain), the yank scenario, the
 * throttled-medium scenario, the controller-recovery scenario, a hot-removed
 * hub, and an ordinary boot.
 *
 * So it is run deliberately, once, at a moment defined by a FACT — the first
 * mass-storage device this kernel configures — and not by a clock.
 *
 * The transfer is posted and the doorbell is NOT rung. That is a faithful
 * "no answer came": the stages really are on the ring, the controller really
 * will execute them the next time it is rung, and nothing about the wait is
 * pretended.
 *
 * ‼ AND THE PROOF IS THE SECOND TRANSFER, not the first. The abandoned one
 * asks for the DEVICE descriptor; the one after it asks for the
 * CONFIGURATION descriptor, into the same buffer. If the give-up left the
 * abandoned stages where they were, the controller executes them first when
 * the next doorbell rings — and its event is accepted as the second
 * transfer's answer, because a control transfer sets xfer_trb_phys to zero
 * and that turns the TRB match off. The buffer then holds a descriptor of
 * type 1 where the caller asked for type 2, and the caller cannot tell.
 */
void xhci_ctrl_giveup_proof(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    static bool already = false;

    if (already || !ctrl || !slot) {
        return;
    }
    already = true;

    /* ‼ Never silent. A proof that declines without saying so reads exactly
     * like a proof that passed, and the first version of this one did that:
     * it returned on a missing scratch buffer and the scenario saw nothing at
     * all, which it would also have seen from a kernel where the give-up was
     * broken. */
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

    /*
     * ‼ THE ABANDONED REQUEST IS ONE THE DEVICE WILL REFUSE, and that is the
     * whole of what makes this provable rather than racy.
     *
     * The first version asked for the device descriptor and checked which
     * descriptor landed in the buffer. That check could not fail: left in
     * place, the controller runs BOTH transfers back to back the moment the
     * next doorbell rings, so the right bytes arrive in the buffer anyway,
     * just as the answer to the wrong question. Measured — it stayed green
     * under the mutation, which is how it was caught.
     *
     * A descriptor type of 0xFF does not exist, so the device STALLS it, and a
     * stall is carried in the EVENT rather than in the buffer. If the
     * abandoned transfer is still on the ring, that stall is what comes back
     * to the next caller — who asked a perfectly good question — and no timing
     * can turn it into a success.
     */
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

    /* Now a question the device is glad to answer, down the same pipe. */
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
#endif /* CONFIG_XHCI_CTRL_GIVEUP_PROOF */

/*
 * ── a halted report endpoint, repaired where waiting is allowed ────────────
 *
 * A doorbell cannot restart a pipe the device halted. xHCI 1.2b §4.7:
 * "The xHC shall ignore doorbell references to endpoints in the Halted or
 * Error state" — a `shall`, beside a `may` for the Disabled case one line
 * above it, so this is not a race that a fast enough driver wins. And every
 * completion code that reaches the failure arm below leaves the endpoint in
 * exactly that state: Stall Error (§4.10.2.1 "set the associated Endpoint
 * State (EP State) field to Halted"), Babble Detected (§4.10.2.4 "halt the
 * endpoint"), and a USB Transaction Error with the error count spent
 * (§4.10.2.7 "the TD is aborted, the endpoint is Halted").
 *
 * Getting it back is two halves with an order between them. §4.10.2.1:
 * "The Reset Endpoint Command for the endpoint shall complete successfully
 * and the halt condition on the USB device shall be successfully cleared
 * before attempting to restart the Transfer Ring by ringing its doorbell."
 * §4.10.2.1.1: the device-side half "shall be invoked after the pipe has been
 * transitioned to the Stopped state by a successful Reset Endpoint Command,
 * but before writing to the Doorbell register of the Endpoint".
 *
 * Neither half can be done from the event drain. Waiting for a command
 * completion is waiting for a pass that cannot run until this one returns,
 * and the device-side half is a control transfer, which is the one thing this
 * context is defined not to do. The drain used to post the two commands
 * without waiting and then ring anyway: the ring was left holding a report
 * the controller would never be asked for, and the only place that re-arms
 * the endpoint is the handler for an event that could now never come. One
 * stall and the keyboard was silent for the rest of the boot.
 *
 * So the drain knocks, and the repair happens on a K-Core — the one road out
 * of an interrupt this kernel has (baton.h). The knock carries no list
 * because it needs none: which endpoints want repairing is a fact the
 * CONTROLLER holds, and asking it is cheaper and truer than remembering.
 */
static void kbd_repair_knocked(void* ctx);
static Knock g_kbd_repair_knock = {
    .baton  = { .next = NULL, .run = kbd_repair_knocked, .ctx = NULL },
    .raised = 0,
};

/* USB 2.0 §9.2.6.4 gives a standard request with no data stage 50 ms. A
 * second is generous for the one below. */
#define XHCI_HALT_CLEAR_TIMEOUT_MS 1000

/*
 * The whole of §4.6.8 and §4.10.2.1.1 for one interrupt IN endpoint, in the
 * order the specification gives, and then the transfer that puts the pipe
 * back to work. Returns 0 when the endpoint is listening again.
 */
static int kbd_repair_one(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    uint8_t dci = slot->ep_interrupt_in;
    xhci_endpoint_t* ep = &slot->endpoints[dci];

    if (xhci_ep_context_state(ctrl, slot, dci) == XHCI_EP_STATE_HALTED) {
        /* The controller's half, and its completion WAITED FOR. Posting a
         * command is not the same as its having happened, and until it has,
         * the doorbell at the bottom of this function is dropped. */
        xhci_ep_recover(ctrl, slot, dci);
        xhci_command_wait_idle(ctrl, XHCI_HALT_CLEAR_TIMEOUT_MS);

        uint8_t state = xhci_ep_context_state(ctrl, slot, dci);
        if (state == XHCI_EP_STATE_HALTED) {
            kprintf("[xHCI %s] slot %u: the keyboard's report endpoint is "
                    "still %s after a Reset Endpoint — not ringing for it\n",
                    ctrl->name, slot->slot_id, xhci_ep_state_name(state));
            return -1;
        }

        /*
         * And the device's half, which nothing in this driver used to send to
         * anything but a disk. The controller agreeing that the pipe is
         * usable does not make the DEVICE agree, and §4.6.8 adds a second
         * consequence to leaving it out: the Reset Endpoint is posted with
         * Transfer State Preserve cleared, and "Undefined behavior may occur
         * if this command is executed with TSP = '0' and the associated
         * device endpoint is not successfully reset by system software. For
         * example, the Data Toggle may not be synchronized between the xHC
         * and a USB2 device."
         */
        usb_setup_packet_t setup = {
            .bmRequestType = 0x02,      /* host to device, endpoint */
            .bRequest = USB_REQ_CLEAR_FEATURE,
            .wValue = USB_FEATURE_ENDPOINT_HALT,
            .wIndex = ep->addr,
            .wLength = 0
        };
        if (xhci_control_transfer_sync(ctrl, slot, &setup, 0, 0, false,
                                       XHCI_HALT_CLEAR_TIMEOUT_MS)
            != TRB_COMPLETION_SUCCESS) {
            kprintf("[xHCI %s] slot %u: the keyboard would not clear the halt "
                    "on its own report endpoint — not ringing for it\n",
                    ctrl->name, slot->slot_id);
            return -1;
        }
    }

    ep->xfer_state = XHCI_XFER_IDLE;
    return xhci_ep_submit(ctrl, slot, dci, ep->buffer_phys, ep->max_packet);
}

/*
 * The visit the knock brings, on a K-Core.
 *
 * Every keyboard whose report endpoint has nothing outstanding is one that
 * stopped listening, because the drain re-arms the good path itself and
 * leaves exactly the endpoints it could not restart idle. A burst of errors
 * is one knock and one visit, and an error that lands after the door opens
 * brings the next one.
 */
static void kbd_repair_knocked(void* ctx)
{
    (void)ctx;
    KnockOpen(&g_kbd_repair_knock);

    for (uint8_t i = 0; i < xhci_controller_count(); i++) {
        xhci_controller_t* ctrl = xhci_controller_at(i);
        if (!ctrl) {
            continue;
        }
        for (uint8_t id = 1; id < 255; id++) {
            xhci_device_slot_t* slot = xhci_get_device_slot_by_id(ctrl, id);
            if (!slot || slot->driver != XHCI_DRIVER_KEYBOARD ||
                !slot->endpoints || !slot->ep_interrupt_in) {
                continue;
            }
            if (slot->endpoints[slot->ep_interrupt_in].xfer_state ==
                XHCI_XFER_IN_FLIGHT) {
                continue;           /* it is listening; nothing is wrong */
            }
            /* Held for the length of the repair: it is made of a command and
             * a control transfer, and the rings both of those walk belong to
             * a device that may be pulled out in the middle of them. */
            if (!xhci_slot_enter(slot)) {
                continue;
            }
            kbd_repair_one(ctrl, slot);
            xhci_slot_leave(slot);
        }
    }
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

    /* The slot that TRB occupied goes back to the ring, and so does every slot
     * queued ahead of it — a transfer ring is executed strictly in order, so an
     * answer for one TRB is an answer for all of them. */
    if (endpoint_id == 1) {
        xhci_ring_reclaim_to(slot->ep0_ring, trb_phys);
    } else if (slot->endpoints && endpoint_id <= XHCI_MAX_DCI) {
        xhci_ring_reclaim_to(slot->endpoints[endpoint_id].ring, trb_phys);
    }

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

        /*
         * ‼ THE ANSWER TO "STOP", WHICH IS NOT THE ANSWER TO THE TRANSFER.
         *
         * Stop Endpoint reports the transfer it stopped on, and it reports it
         * as a Transfer Event — code Stopped, or Stopped with the length not
         * to be believed. That is the controller confirming it has let go, not
         * a device failing anything, and enumeration is being carried forward
         * by the COMMAND's completion rather than by this.
         *
         * Without this line the block below reads it as a step that failed and
         * releases the slot — which is exactly the device the stop was issued
         * to save.
         */
        if (slot->state == ENUM_STATE_WAIT_EP0_STOP &&
            (code == TRB_COMPLETION_STOPPED ||
             code == TRB_COMPLETION_STOPPED_LENGTH)) {
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
         * new to say.
         *
         * ‼ AND THE SERVICE PASS IS ASKED FOR ON EVERY COMPLETION, NOT ONLY
         * ON A GOOD ONE.
         *
         * `ok` is Success and Short Packet. Every other code used to come
         * through here, set the endpoint idle and raise NOTHING — and the
         * only flag that makes the service pass look at a hub is the one
         * xhci_hub_note_change raises. So a single Stall Error, Babble
         * Detected or USB Transaction Error left that hub with no transfer
         * queued and nothing that would ever queue one: deaf to every plug
         * and unplug below it for the rest of its attachment, silently. USB
         * 2.0 §11.12.1 is why silence is total — the host learns of a change
         * only while an IN is outstanding on the status endpoint — and
         * §11.12.3's Figure 11-21 makes "Re-initialize Interrupt Pipe for
         * Status Change endpoint" a step of the loop rather than an option.
         *
         * The repair such a pipe needs is a command completion and a control
         * transfer (xHCI §4.10.2.1.1), so it belongs in the service pass and
         * not here. What belongs here is making sure somebody goes and looks.
         */
        xhci_endpoint_t* ep = &slot->endpoints[endpoint_id];
        ep->xfer_state = XHCI_XFER_IDLE;
        if (ok) {
            xhci_hub_note_change(slot);
        } else {
            kprintf("[xHCI %s] slot %u: a hub's status endpoint answered %s — "
                    "its pipe is being put back in order\n",
                    ctrl->name, slot_id, xhci_completion_name(code));
            xhci_hub_note_work();
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
        ep->xfer_state = XHCI_XFER_IDLE;

        if (ok) {
            /* The report, and the endpoint held open again. Nothing halted it
             * and nothing has to be cleared, so the doorbell is rung from
             * here — this is the hot path and the keystroke is waiting on it. */
            xhci_process_keyboard_report(
                (usb_boot_keyboard_report_t*)ep->buffer_virt);
            xhci_ep_submit(ctrl, slot, endpoint_id, ep->buffer_phys,
                           ep->max_packet);
            return;
        }

        /*
         * ‼ AND NOT RE-ARMED FROM HERE, WHICH IS THE WHOLE OF THE REPAIR.
         *
         * Transaction errors are what a marginal cable looks like, and the
         * controller has already retried them CErr times — at which point
         * xHCI §4.10.2.7 has it abort the TD and HALT the endpoint, exactly
         * as a stall does. Re-arming is still the right answer and going
         * quiet is still wrong, but a doorbell is not how a halted endpoint
         * is restarted: §4.7 says the xHC shall ignore one. The knock hands
         * the repair to a context that can do the two things it is made of —
         * wait for a Reset Endpoint to complete, and send the device the
         * ClearFeature(ENDPOINT_HALT) that §4.10.2.1.1 places between that
         * completion and the doorbell.
         */
        KnockOn(&g_kbd_repair_knock);
        return;
    }

    xhci_ep_complete(slot, endpoint_id, code, residual, trb_phys);
}
