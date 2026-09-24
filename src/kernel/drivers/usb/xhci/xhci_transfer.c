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
 * ── the step of "reset a pipe" that is addressed to the HUB ────────────────
 *
 * xHCI 1.2 §4.6.8, the Note at the end: "Software shall execute the following
 * sequence to 'reset a pipe' ... Also, if the device was behind a TT, the TT
 * buffer would also need to be cleared. • Reset Endpoint Command (TSP = '0').
 * • If the device was behind a TT and it is a Control or Bulk endpoint: •
 * Issue a ClearFeature(CLEAR_TT_BUFFER) request to the hub. • If not a Control
 * endpoint: • Issue a ClearFeature(ENDPOINT_HALT) request to device. • Issue a
 * Set TR Dequeue Pointer Command ..."
 *
 * What the hub is left holding is a buffer, and USB 2.0 §11.17.5 says what
 * becomes of it: the split transaction that failed leaves the buffer busy, and
 * the hub will not allow it to be reused for other bulk or control split
 * transactions. §11.17.4 gives a transaction translator at least two
 * non-periodic buffers, so nothing breaks at the first halt — it is one buffer
 * lost per unrepaired halt until the translator has none left and stops
 * carrying bulk and control traffic for everything below it.
 *
 * ‼ AND IT IS NOT A ClearFeature, WHATEVER THE xHCI SPECIFICATION CALLS IT.
 * USB 2.0 §11.24.2.3 and Table 11-18: bmRequestType 00100011B, bRequest
 * CLEAR_TT_BUFFER (8), wLength zero, no data stage, and the endpoint is named
 * in wValue rather than by a feature selector —
 *
 *   wValue  [3:0]   Endpoint Number
 *           [10:4]  Device Address
 *           [12:11] Endpoint Type, in the numbering of bmAttributes
 *           [15]    Direction, 1 = IN
 *   wIndex          the TT port — except that §11.24.2.3 adds "If the hub
 *                   provides only a single TT, then wIndex must be set to
 *                   one", so sending the real port to a single-TT hub whose
 *                   device hangs off port 2 or higher is a Request Error and
 *                   not a harmless overshoot. Linux writes the same
 *                   expression: tt->multi ? udev->ttport : 1.
 *
 * The device address is the CONTROLLER'S, not this driver's: the xHC assigns
 * it and writes it into the Output Slot Context, which is where it is read
 * from here.
 *
 * Reached only from the waitable side of the driver: this is a control
 * transfer to a THIRD device, and the event drain cannot make one.
 *
 * ‼ WHICH ENDPOINTS, AND WHY EP0 IS HERE AT ALL. §4.6.8 names "a Control or
 * Bulk endpoint", so an interrupt pipe is refused below — a periodic split is
 * re-offered in the next microframe and leaves nothing in the translator, and
 * Linux excludes it in the same condition (`!usb_pipeint(td->urb->pipe)` in
 * xhci_clear_hub_tt_buffer). The default control pipe is NOT excluded here,
 * only at the one call site where it must be: after a protocol stall, which is
 * the device ANSWERING, the split transaction that carried the refusal
 * completed and the hub released the buffer. Linux draws that line in one
 * condition, drivers/usb/host/xhci-ring.c, verbatim: "Hub TT buffer should
 * only be cleared for FS/LS devices behind HS hubs for functional stalls."
 * followed by
 *
 *     if (!(ep->ep_index == 0 && trb_comp_code == COMP_STALL_ERROR))
 *             xhci_clear_hub_tt_buffer(xhci, td, ep);
 *
 * — ep_index 0 being EP0. An EP0 halted by a babble or by a transaction error
 * the controller gave up on is the other half of that condition, and it leaves
 * a split transaction genuinely unfinished in the translator.
 */
#define HUB_REQ_CLEAR_TT_BUFFER   8
#define TT_BUFFER_EP_NUM_SHIFT    0
#define TT_BUFFER_DEV_ADDR_SHIFT  4
#define TT_BUFFER_EP_TYPE_SHIFT   11
#define TT_BUFFER_DIRECTION_IN    (1u << 15)
#define TT_BUFFER_EP_TYPE_CONTROL 0u      /* USB 2.0 Table 9-13 bmAttributes */
#define TT_BUFFER_EP_TYPE_BULK    2u

void xhci_clear_tt_buffer(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                          uint8_t dci, uint32_t timeout_ms)
{
    if (!ctrl || !slot || !slot->endpoints || dci < 1 || dci > XHCI_MAX_DCI) {
        return;
    }
    if (slot->tt_slot_id == 0) {
        return;                 /* nothing translating: no buffer to clear */
    }

    uint32_t ep_type;
    switch (slot->endpoints[dci].type) {
        case XHCI_EP_TYPE_CONTROL:
            ep_type = TT_BUFFER_EP_TYPE_CONTROL;
            break;
        case XHCI_EP_TYPE_BULK_IN:
        case XHCI_EP_TYPE_BULK_OUT:
            ep_type = TT_BUFFER_EP_TYPE_BULK;
            break;
        default:
            return;             /* periodic: the translator holds nothing */
    }

    xhci_device_slot_t* tt = xhci_get_device_slot_by_id(ctrl, slot->tt_slot_id);
    if (!tt || !xhci_slot_enter(tt)) {
        kprintf("[xHCI %s] slot %u: the hub translating for it is no longer "
                "there, so the transaction buffer it is holding cannot be "
                "cleared\n", ctrl->name, slot->slot_id);
        return;
    }

    uint8_t address = xhci_slot_context_address(slot);
    if (address == 0) {
        /* The controller has not said what address it gave this device, and a
         * request naming address zero would clear somebody else's buffer. */
        kprintf("[xHCI %s] slot %u: the controller gives it no address, so the "
                "transaction buffer its hub is holding cannot be named\n",
                ctrl->name, slot->slot_id);
        xhci_slot_leave(tt);
        return;
    }

    uint8_t  ep_addr = slot->endpoints[dci].addr;
    uint16_t value   = (uint16_t)(((uint16_t)(ep_addr & 0x0Fu)
                                        << TT_BUFFER_EP_NUM_SHIFT)
                                | ((uint16_t)(address & 0x7Fu)
                                        << TT_BUFFER_DEV_ADDR_SHIFT)
                                | ((uint16_t)ep_type
                                        << TT_BUFFER_EP_TYPE_SHIFT)
                                | ((ep_addr & 0x80u) ? TT_BUFFER_DIRECTION_IN
                                                     : 0u));

    usb_setup_packet_t setup = {
        .bmRequestType = 0x23,      /* host to device, class, other */
        .bRequest = HUB_REQ_CLEAR_TT_BUFFER,
        .wValue = value,
        .wIndex = tt->multi_tt ? slot->tt_port : 1,
        .wLength = 0
    };

    /* Two different failures, and saying the first as if it were the second
     * would be a false statement about the hardware. A negative answer means
     * the request never reached the wire — the hub's own EP0 was already
     * carrying a transfer, or the caller turned out to be inside the event
     * drain — and the hub has said nothing at all. Only a completion code
     * that is not Success is the hub refusing. */
    int rc = xhci_control_transfer_sync(ctrl, tt, &setup, 0, 0, false,
                                        timeout_ms);
    if (rc < 0) {
        kprintf("[xHCI %s] slot %u: the hub translating for it could not be "
                "asked to let go of the buffer for endpoint 0x%02x (%d), so "
                "that translator has one fewer to give out\n",
                ctrl->name, slot->slot_id, ep_addr, rc);
    } else if (rc != TRB_COMPLETION_SUCCESS) {
        kprintf("[xHCI %s] slot %u: the hub translating for it would not let "
                "go of the buffer for endpoint 0x%02x — that translator has "
                "one fewer to give out\n",
                ctrl->name, slot->slot_id, ep_addr);
    }

    xhci_slot_leave(tt);
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
     *
     * ‼ AND A REFUSAL IS NOT THE ONLY THING THAT HALTS IT. This asked about
     * Stall Error alone, and Stall Error is the one halting code that means
     * the device is WELL. Babble Detected (xHCI 1.2b §4.10.2.4 "halt the
     * endpoint") and a USB Transaction Error with the error count spent
     * (§4.10.2.7 "the TD is aborted, the endpoint is Halted") leave EP0 in
     * exactly the same state and were walked away from — so one disturbed
     * answer on a marginal cable cost the device every control transfer it
     * would ever be asked for, silently. Which codes those are is not listed
     * here: the controller keeps the endpoint's state in the Output Endpoint
     * Context and updates it as it goes, so it is ASKED.
     */
    if (code > 0 && code != TRB_COMPLETION_SUCCESS &&
        code != TRB_COMPLETION_SHORT_PKT &&
        xhci_ep_context_state(ctrl, slot, 1) == XHCI_EP_STATE_HALTED) {

        /* §4.6.8's hub-side step, for the halts that are NOT the device
         * answering — see xhci_clear_tt_buffer for why a protocol stall is
         * excluded and these are not. Issued before the ring is repositioned,
         * so the translator has the buffer back by the time the doorbell
         * rings. Costs nothing on a device that is not behind a translator. */
        if (code != TRB_COMPLETION_STALL) {
            xhci_clear_tt_buffer(ctrl, slot, 1, timeout_ms);
        }

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

    /*
     * ‼ WHAT THE CONTROLLER HAD DONE, AND WHEN WE ASKED — taken here, one
     * instruction before the doorbell, because this is the only moment at which
     * "it has not moved since we rang" can be said truthfully. See GateClock
     * (XHCI_GATE_CLOCK_MICROFRAMES) for what reads the pair.
     */
    slot->doorbell_dequeue = xhci_ep_context_dequeue(ctrl, slot, 1);
    slot->doorbell_mfindex = ctrl->runtime_regs
                           ? (ctrl->runtime_regs->mfindex & XHCI_MFINDEX_MASK)
                           : 0u;

#if CONFIG_XHCI_ENUM_SILENCE_PROOF
    /*
     * ── the one proof that a pipe which never spoke is answered by the port ──
     *
     * The eight seconds this driver spent on every boot of the owner's machines
     * happened on a road emulation never takes: under QEMU every control
     * request is answered immediately, so a device that goes SILENT at its
     * first descriptor request cannot be produced at all. The fix for it was
     * therefore unprovable on the stand — which is how it stayed broken while
     * eight green scenarios passed.
     *
     * So the silence is made, once, at a moment defined by a FACT rather than a
     * clock: the first device to reach the descriptor-header step. The stages
     * really are on the ring and the doorbell is simply not rung, which is a
     * faithful "the controller never started the TD" — the exact state both
     * boards reported, with the endpoint Running, the dequeue pointer on the
     * first TRB, and no event ever raised.
     *
     * The injection is spent after one use, so the retry that follows is a real
     * one: the port is reset, the device is asked again, and this time it
     * answers. A scenario can then check the whole shape — one give-up, no
     * ladder of four, and a device that comes up on the fresh port.
     */
    static bool g_enum_silence_spent = false;
    if (!g_enum_silence_spent &&
        slot->state == ENUM_STATE_WAIT_GET_DESC_HEADER) {
        g_enum_silence_spent = true;
        kprintf("[xHCI %s] port %u: ENUMSILENCE — the doorbell for the first "
                "descriptor request is deliberately not rung; this pipe will "
                "never speak\n", ctrl->name, slot->port_num);
        return 0;
    }
#endif

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
        /* Counted before anything decides what this event means, and counted
         * nowhere else. The ring's dequeue index would be the wrong measure —
         * xhci_ring_abandon sets it to the enqueue index on every repair — so
         * a report about what the controller has answered asks these counters
         * and the endpoint context, never the ring. */
        slot->ep0_events++;

        /*
         * ‼ AND WHETHER THE DEVICE HAD ANYTHING TO DO WITH IT.
         *
         * A Stop Endpoint posted by this driver's own repair completes the
         * transfer it took back, and the controller answers with Stopped (xHCI
         * 1.2 §4.6.9: the Stop Endpoint Command "shall" complete any TD in
         * progress, and §6.4.5 gives Stopped and Stopped - Length Invalid as
         * its completion codes). That answer says nothing whatever about the
         * device. Counting it as one made the failure report claim a pipe was
         * answering when it had never said a word — measured on both machines,
         * 0 then 1 then 3 across three rungs, with the device silent all the
         * way through.
         */
        if (code != TRB_COMPLETION_STOPPED &&
            code != TRB_COMPLETION_STOPPED_LENGTH) {
            slot->ep0_answers++;
        }

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
         * ‼ WHILE A STOP IS OUTSTANDING, NO TRANSFER EVENT IS AN ANSWER.
         *
         * Stop Endpoint reports the transfer it stopped on, and it reports it
         * as a Transfer Event — code Stopped, or Stopped with the length not
         * to be believed. That is the controller confirming it has let go, not
         * a device failing anything, and enumeration is being carried forward
         * by the COMMAND's completion rather than by this.
         *
         * ‼ AND THE OTHER CODES BELONG TO THE SAME MOMENT. This state is
         * entered by the watchdog for a step that went UNANSWERED, and the
         * commonest reason a step goes unanswered is that the pipe halted
         * while it was being answered. That late answer — Stall Error, Babble
         * Detected, USB Transaction Error — is drained after the Stop has been
         * posted and before its completion arrives, and it names a transfer
         * the controller has already been told to let go of. Neither
         * xhci_enum_stall_is_tolerable nor xhci_enum_step_can_be_asked_again
         * answers true for this state, and rightly so — sending
         * xhci_enum_recover_ep0 in with it would loop STOP → RESET → DEQUEUE →
         * STOP — so the block below read it as a step that failed and released
         * the slot. That is exactly the device the stop was issued to save,
         * and for one behind a hub it is gone for good: born_port is zero, so
         * there is no port retry, and hub_scan_ports has already marked its
         * port as answered.
         *
         * Nothing is lost by swallowing it. The halt itself is not missed: the
         * Stop Endpoint answers Context State Error for exactly this race
         * (§4.6.9, the Note before Table 4-2), and xhci_enum_advance_state
         * reads the EP0 context there and routes a Halted pipe to Reset
         * Endpoint. An answer that arrives complete is not lost either — the
         * step is simply asked again on a repositioned ring, which is what the
         * watchdog already wrote down when it entered this state.
         */
        if (slot->state == ENUM_STATE_WAIT_EP0_STOP) {
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
                /* ‼ WITH THE RESIDUAL, WHICH IS WHAT TELLS THE THREE BABBLES
                 * APART.
                 *
                 * xHCI 1.2 §6.4.2.1: the Transfer Event's length field is what
                 * was NOT transferred. A Babble that leaves the whole request
                 * undelivered is a device overrunning the TD it was given —
                 * the ordinary shape of a device that ignores wLength (§4.10.2.4)
                 * — and one that leaves part of it is the bus being disturbed
                 * mid-answer. The number was extracted six lines above this and
                 * thrown away, and without it the board's Babble could only be
                 * reasoned about, which is what three separate readings of the
                 * same log ended up doing. */
                kprintf("[xHCI %s] port %u: %s — %s, with %u of the %u byte(s) "
                        "asked for undelivered; clearing the control "
                        "pipe and asking again (attempt %u of %u)\n",
                        ctrl->name, slot->port_num,
                        xhci_enum_state_name(slot->state),
                        xhci_completion_name(code),
                        residual, slot->ctl_requested,
                        slot->step_retry, XHCI_STEP_RETRIES);
                xhci_enum_recover_ep0(ctrl, slot, slot->state, true);
                return;
            }

            kprintf("[xHCI %s] port %u: %s failed — %s (code %u), with %u of "
                    "the %u byte(s) asked for undelivered; releasing "
                    "the slot\n", ctrl->name, slot->port_num,
                    xhci_enum_state_name(slot->state),
                    xhci_completion_name(code), code,
                    residual, slot->ctl_requested);
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
