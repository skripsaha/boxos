#include "xhci_endpoint.h"
#include "xhci_enumeration.h"
#include "xhci_command.h"
#include "xhci_device.h"
#include "xhci_interrupt.h"
#include "xhci_trb.h"
#include "xhci_port.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "storage_completion.h"

/* Every transfer ring here is 64 TRBs — one page holds 256, and a ring that
 * fits in a page it does not share is a ring whose wrap behaviour is easy to
 * reason about. Bulk-Only Transport puts three transfers on the wire per
 * command, so this is twenty commands of headroom before a wrap. */
#define XHCI_EP_RING_TRBS 64

uint8_t xhci_ep_type_of(uint8_t attributes, uint8_t endpoint_addr)
{
    bool in = (endpoint_addr & 0x80) != 0;

    switch (attributes & 0x03) {
        case 0: return XHCI_EP_TYPE_INVALID;    /* control, and only EP0 here */
        case 1: return in ? XHCI_EP_TYPE_ISOCH_IN     : XHCI_EP_TYPE_ISOCH_OUT;
        case 2: return in ? XHCI_EP_TYPE_BULK_IN      : XHCI_EP_TYPE_BULK_OUT;
        default:return in ? XHCI_EP_TYPE_INTERRUPT_IN : XHCI_EP_TYPE_INTERRUPT_OUT;
    }
}

uint32_t xhci_input_ctx_pages(xhci_controller_t* ctrl)
{
    /* One Input Control Context, one Slot Context and 31 Endpoint Contexts. */
    return (uint32_t)vmm_size_to_pages((size_t)ctrl->context_size * 33u);
}

int xhci_ep_table_alloc(xhci_device_slot_t* slot)
{
    if (!slot) {
        return -1;
    }
    if (slot->endpoints) {
        return 0;
    }

    slot->endpoints = (xhci_endpoint_t*)kmalloc(sizeof(xhci_endpoint_t) *
                                                XHCI_DCI_COUNT);
    if (!slot->endpoints) {
        return -1;
    }
    memset(slot->endpoints, 0, sizeof(xhci_endpoint_t) * XHCI_DCI_COUNT);
    slot->max_dci = 1;              /* EP0 always */
    return 0;
}

void xhci_ep_table_free(xhci_device_slot_t* slot)
{
    if (!slot || !slot->endpoints) {
        return;
    }

    for (uint8_t dci = 2; dci <= XHCI_MAX_DCI; dci++) {
        xhci_endpoint_t* ep = &slot->endpoints[dci];
        if (ep->ring) {
            xhci_ring_destroy(ep->ring);
            if (ep->ring_page_phys) {
                pmm_free((void*)ep->ring_page_phys, 1);
            }
        }
        if (ep->buffer_phys) {
            pmm_free((void*)ep->buffer_phys, vmm_size_to_pages(ep->buffer_bytes));
        }
    }

    kfree(slot->endpoints);
    slot->endpoints = NULL;
    slot->max_dci = 0;
    slot->ep_pending_add = 0;
}

int xhci_ep_prepare(xhci_device_slot_t* slot, uint8_t dci, uint8_t type,
                    const usb_endpoint_info_t* info, uint32_t buffer_bytes)
{
    if (!slot || !slot->endpoints || dci < 2 || dci > XHCI_MAX_DCI || !info) {
        return -1;
    }
    if (type == XHCI_EP_TYPE_INVALID || info->max_packet == 0) {
        return -1;
    }

    xhci_endpoint_t* ep = &slot->endpoints[dci];
    if (ep->active) {
        return 0;                   /* already prepared */
    }

    void* ring_page = pmm_alloc_zero(1, PHYS_TAG_DMA32);
    if (!ring_page) {
        return -1;
    }

    xhci_ring_t* ring = (xhci_ring_t*)vmm_phys_to_virt((uintptr_t)ring_page);
    if (!ring || xhci_ring_init(ring, XHCI_EP_RING_TRBS, true) != 0) {
        pmm_free(ring_page, 1);
        return -1;
    }

    ep->ring               = ring;
    ep->ring_page_phys     = (uint64_t)ring_page;
    ep->max_packet         = info->max_packet;
    ep->addr               = info->addr;
    ep->type               = type;
    ep->interval           = info->interval;
    ep->max_burst          = info->max_burst;
    ep->mult               = info->mult;
    ep->bytes_per_interval = info->bytes_per_interval;
    ep->xfer_state         = XHCI_XFER_IDLE;

    if (buffer_bytes > 0) {
        size_t pages = vmm_size_to_pages(buffer_bytes);
        void* buf = pmm_alloc_zero(pages, PHYS_TAG_DMA32);
        if (!buf) {
            xhci_ring_destroy(ring);
            pmm_free(ring_page, 1);
            memset(ep, 0, sizeof(*ep));
            return -1;
        }
        ep->buffer_phys  = (uint64_t)buf;
        ep->buffer_virt  = vmm_phys_to_virt((uintptr_t)buf);
        ep->buffer_bytes = buffer_bytes;
    }

    ep->active = true;
    if (dci > slot->max_dci) {
        slot->max_dci = dci;
    }
    slot->ep_pending_add |= (1u << dci);
    return 0;
}

/*
 * The service interval, which is where the units stop agreeing with each other.
 *
 * A low or full speed endpoint states bInterval directly in frames of one
 * millisecond. The controller wants a power-of-two count of 125 microsecond
 * microframes, so the conversion is a base-two logarithm plus three — not an
 * addition, which is what it used to be. High and super speed endpoints already
 * state an exponent, and there the conversion is a subtraction.
 *
 * Getting this wrong fails quietly: the endpoint is serviced at the wrong rate,
 * and a keyboard polled every two seconds looks like a keyboard that drops
 * keystrokes rather than one that was configured wrong.
 */
static uint8_t ep_interval_for(uint8_t speed, uint8_t type, uint8_t bInterval)
{
    /* Bulk and control endpoints are not periodic; the field is reserved. */
    if (type == XHCI_EP_TYPE_BULK_IN || type == XHCI_EP_TYPE_BULK_OUT) {
        return 0;
    }

    if (speed == XHCI_PORT_SPEED_FULL || speed == XHCI_PORT_SPEED_LOW) {
        uint8_t frames = bInterval ? bInterval : 1;
        uint8_t log2 = 0;
        while ((1u << (log2 + 1)) <= frames && log2 < 10) {
            log2++;
        }
        return (uint8_t)(log2 + 3);
    }

    uint8_t exponent = bInterval ? bInterval : 1;
    if (exponent > 16) {
        exponent = 16;
    }
    return (uint8_t)(exponent - 1);
}

static bool ep_is_periodic(uint8_t type)
{
    return type == XHCI_EP_TYPE_INTERRUPT_IN ||
           type == XHCI_EP_TYPE_INTERRUPT_OUT ||
           type == XHCI_EP_TYPE_ISOCH_IN ||
           type == XHCI_EP_TYPE_ISOCH_OUT;
}

/*
 * How much the endpoint may move in one service interval.
 *
 * xHCI 1.2 Section 4.14.2 and Section 6.2.3.8. A SuperSpeed endpoint states
 * this itself, in the companion descriptor, and there is nothing to calculate.
 * Below SuperSpeed there is no such field, so it is Max Packet Size times the
 * number of transactions the endpoint asked for — the count the packet-size
 * field carries in bits 12:11 and that this driver used to discard.
 *
 * The consequence of getting it low is not slowness. It is what the controller
 * reserves bus time for, so an endpoint under-declared here is one whose
 * transfers the controller will decline to schedule in full.
 */
static uint32_t ep_max_esit_payload(const xhci_endpoint_t* ep)
{
    if (!ep_is_periodic(ep->type)) {
        return 0;                   /* the field is reserved for the rest */
    }
    if (ep->bytes_per_interval) {
        return ep->bytes_per_interval;
    }
    return (uint32_t)ep->max_packet *
           ((uint32_t)ep->max_burst + 1u) * ((uint32_t)ep->mult + 1u);
}

static void ep_write_context(xhci_endpoint_t* ep, uint8_t speed,
                             xhci_endpoint_context_t* ctx)
{
    memset(ctx, 0, sizeof(*ctx));

    uint8_t  interval = ep_interval_for(speed, ep->type, ep->interval);
    uint32_t esit     = ep_max_esit_payload(ep);

    /* dword0: Mult [9:8], Interval [23:16], Max ESIT Payload Hi [31:24].
     *
     * Mult is SuperSpeed isochronous only; everything else leaves it zero, and
     * a non-zero Mult on an endpoint that is not entitled to one is a
     * Parameter Error rather than a fast endpoint. */
    ctx->dwords[0] = ((uint32_t)(ep->mult & 0x3) << 8) |
                     ((uint32_t)interval << 16) |
                     (((esit >> 16) & 0xFFu) << 24);

    /*
     * dword1: CErr = 3 [2:1], EP Type [5:3], Max Burst Size [15:8],
     *         Max Packet Size [31:16].
     *
     * Max Burst Size was left at zero for every endpoint of every device.
     * Zero means one packet per burst, which is correct for full and low speed
     * and is wrong everywhere else: a SuperSpeed flash drive that can move
     * sixteen packets back to back moves one, forever, and nothing anywhere
     * reports it — the transfers all succeed. A high-speed interrupt endpoint
     * asking for two or three transactions per microframe gets one, and drops
     * what it could not send.
     */
    ctx->dwords[1] = (3u << 1) | ((uint32_t)ep->type << 3) |
                     ((uint32_t)ep->max_burst << 8) |
                     ((uint32_t)ep->max_packet << 16);

    /* dword2-3: TR Dequeue Pointer, with the Dequeue Cycle State in bit 0. */
    uint64_t ring_addr = ep->ring->trbs_phys;
    ctx->dwords[2] = (uint32_t)(ring_addr & 0xFFFFFFF0u) | 1u;
    ctx->dwords[3] = (uint32_t)(ring_addr >> 32);

    /* dword4: Average TRB Length [15:0], Max ESIT Payload Lo [31:16]. */
    ctx->dwords[4] = (uint32_t)ep->max_packet | ((esit & 0xFFFFu) << 16);
}

int xhci_ep_configure(xhci_controller_t* ctrl, xhci_device_slot_t* slot)
{
    if (!ctrl || !slot || !slot->endpoints || slot->ep_pending_add == 0) {
        return -1;
    }

    uint32_t pages = xhci_input_ctx_pages(ctrl);
    void* input_phys = pmm_alloc_zero(pages, PHYS_TAG_DMA32);
    if (!input_phys) {
        return -1;
    }
    slot->input_ctx_phys = (uint64_t)input_phys;

    uint8_t* base = (uint8_t*)vmm_phys_to_virt((uintptr_t)input_phys);

    /* The slot context comes along with A0 set because the specification
     * requires it whenever endpoints are added: the Context Entries field it
     * carries is how the controller learns how far the device context now
     * reaches. */
    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)base;
    icc->add_context_flags = (1u << 0) | slot->ep_pending_add;

    xhci_slot_context_t* slot_ctx =
        (xhci_slot_context_t*)(base + ctrl->context_size);
    xhci_fill_slot_context(slot_ctx, slot);
    slot_ctx->dwords[0] = (slot_ctx->dwords[0] & ~(0x1Fu << 27)) |
                          ((uint32_t)slot->max_dci << 27);

    for (uint8_t dci = 2; dci <= slot->max_dci; dci++) {
        if (!(slot->ep_pending_add & (1u << dci))) {
            continue;
        }
        xhci_endpoint_context_t* ep_ctx =
            (xhci_endpoint_context_t*)(base + ctrl->context_size * (dci + 1));
        xhci_endpoint_t* ep = &slot->endpoints[dci];
        ep_write_context(ep, slot->speed, ep_ctx);

        /*
         * What the controller is being told, once per endpoint per device.
         *
         * The burst is the reason this line exists. It is invisible in every
         * other way: an endpoint configured for one packet per burst when it
         * could do sixteen completes every transfer it is given, reports no
         * error, and is simply slow for the life of the machine. On a board
         * that can only be read by photographing its screen, "burst 16" is the
         * difference between a flash drive that works and one that works
         * properly, and there is nowhere else to find it out.
         */
        kprintf("[xHCI] slot %u endpoint %u: %u byte packets, burst %u, "
                "interval %u\n",
                slot->slot_id, dci, ep->max_packet, ep->max_burst + 1,
                ep_interval_for(slot->speed, ep->type, ep->interval));
    }

    if (xhci_post_configure_endpoint_cmd(ctrl, slot, slot->slot_id,
                                         (uint64_t)input_phys) < 0) {
        pmm_free(input_phys, pages);
        slot->input_ctx_phys = 0;
        return -1;
    }
    return 0;
}

static int ep_submit(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                     uint8_t dci, uint64_t buffer_phys, uint32_t length,
                     StorageCompletion* done)
{
    if (!ctrl || !slot || !slot->endpoints || dci < 2 || dci > XHCI_MAX_DCI) {
        return -1;
    }

    xhci_endpoint_t* ep = &slot->endpoints[dci];
    if (!ep->active || !ep->ring) {
        return -1;
    }

    /* One transfer at a time on an endpoint. A caller that ignores this would
     * overwrite the record of a transfer still in flight and then wait on the
     * wrong answer. */
    if (ep->xfer_state == XHCI_XFER_IN_FLIGHT) {
        return -2;
    }

    xhci_trb_t trb = {0};
    trb.parameter = buffer_phys;
    trb.status    = length;
    /* Interrupt On Completion, and On Short Packet: a device answering with
     * less than was asked for is ordinary on bulk IN — it is how a SCSI reply
     * shorter than its buffer arrives — and without ISP that transfer would
     * finish with nothing to say it had. */
    trb.control   = TRB_SET_TYPE(TRB_TYPE_NORMAL) | TRB_IOC | TRB_ISP;

    /* Said before it is asked for. The controller may answer inside the
     * doorbell write, and the answer looks for somewhere to go the moment it
     * is drained — so where it goes is written down first. */
    ep->xfer_done     = done;
    ep->xfer_state    = XHCI_XFER_IN_FLIGHT;
    ep->xfer_code     = 0;
    ep->xfer_residual = 0;

    uint64_t trb_phys = xhci_ring_enqueue(ep->ring, &trb);
    if (trb_phys == 0) {
        ep->xfer_done  = NULL;
        ep->xfer_state = XHCI_XFER_IDLE;
        return -1;
    }
    ep->xfer_trb_phys = trb_phys;

    __sync_synchronize();
    ctrl->doorbells->doorbells[slot->slot_id].doorbell = dci;
    return 0;
}

int xhci_ep_submit(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                   uint8_t dci, uint64_t buffer_phys, uint32_t length)
{
    return ep_submit(ctrl, slot, dci, buffer_phys, length, NULL);
}

int xhci_ep_submit_async(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                         uint8_t dci, uint64_t buffer_phys, uint32_t length,
                         struct StorageCompletion* done)
{
    return ep_submit(ctrl, slot, dci, buffer_phys, length, done);
}

void xhci_ep_complete(xhci_device_slot_t* slot, uint8_t dci,
                      uint8_t completion_code, uint32_t residual,
                      uint64_t trb_phys)
{
    if (!slot || !slot->endpoints || dci < 1 || dci > XHCI_MAX_DCI) {
        return;
    }

    xhci_endpoint_t* ep = &slot->endpoints[dci];
    if (ep->xfer_state != XHCI_XFER_IN_FLIGHT) {
        return;                     /* nobody is waiting for this */
    }
    if (trb_phys != 0 && ep->xfer_trb_phys != 0 && trb_phys != ep->xfer_trb_phys) {
        return;                     /* a completion for some earlier transfer */
    }

    ep->xfer_code     = completion_code;
    ep->xfer_residual = residual;
    __sync_synchronize();
    ep->xfer_state    = XHCI_XFER_DONE;

    /*
     * And if nobody stayed to hear it, it is carried to where the answering
     * happens.
     *
     * This runs inside the event drain, which may be an interrupt handler and
     * is holding the ring lock either way — so nothing here may wait, and the
     * post does not: the node lives in the job, so there is no allocation to
     * fail and no free slot to be short of. The continuation runs later, on a
     * K-Core, where the next transfer may be sent.
     */
    struct StorageCompletion* done = ep->xfer_done;
    if (done) {
        ep->xfer_done = NULL;
        StorageCompletionPush(done);
    }
}

int xhci_ep_wait(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                 uint8_t dci, uint32_t timeout_ms, uint32_t* out_residual)
{
    if (!ctrl || !slot || !slot->endpoints || dci < 1 || dci > XHCI_MAX_DCI) {
        return -1;
    }

    xhci_endpoint_t* ep = &slot->endpoints[dci];
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);

    while (ep->xfer_state != XHCI_XFER_DONE) {
        /* Drain the ring rather than wait for someone else to. This is what
         * makes the same call work before interrupts are routed, with them
         * masked, and on a controller that has none — and it is safe to do
         * from here because the drain is under the same lock the interrupt
         * handler takes. */
        xhci_process_events();

        if (ep->xfer_state == XHCI_XFER_DONE) {
            break;
        }
        if ((int64_t)(rdtsc() - deadline) >= 0) {
            ep->xfer_state = XHCI_XFER_IDLE;
            return -1;
        }
        cpu_pause();
    }

    uint8_t code = ep->xfer_code;
    if (out_residual) {
        *out_residual = ep->xfer_residual;
    }
    ep->xfer_state = XHCI_XFER_IDLE;
    return code;
}

bool xhci_ep_take_result(xhci_device_slot_t* slot, uint8_t dci,
                         uint8_t* out_code, uint32_t* out_residual)
{
    if (!slot || !slot->endpoints || dci < 1 || dci > XHCI_MAX_DCI) {
        return false;
    }

    xhci_endpoint_t* ep = &slot->endpoints[dci];
    if (ep->xfer_state != XHCI_XFER_DONE) {
        return false;
    }

    if (out_code)     *out_code     = ep->xfer_code;
    if (out_residual) *out_residual = ep->xfer_residual;
    ep->xfer_state = XHCI_XFER_IDLE;
    return true;
}

int xhci_ep_transfer(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                     uint8_t dci, uint64_t buffer_phys, uint32_t length,
                     uint32_t timeout_ms, uint32_t* out_transferred)
{
    /* Asked before the transfer is posted, not after: the drain that would
     * answer it is below this caller on the same stack, so posting first would
     * leave a transfer in flight that nothing can collect. */
    if (xhci_drain_is_mine(ctrl)) {
        kprintf("[xHCI %s] slot %u endpoint %u: a transfer was waited for from "
                "inside the event drain — its answer cannot arrive until this "
                "returns\n", ctrl->name, slot ? slot->slot_id : 0, dci);
        return -3;
    }

    int rc = xhci_ep_submit(ctrl, slot, dci, buffer_phys, length);
    if (rc != 0) {
        return rc;
    }

    uint32_t residual = 0;
    int code = xhci_ep_wait(ctrl, slot, dci, timeout_ms, &residual);
    if (code < 0) {
        return code;
    }

    /* The event reports what was NOT transferred. Everyone asking wants the
     * other number, so this is the one place that subtraction happens. */
    if (out_transferred) {
        *out_transferred = (residual <= length) ? (length - residual) : 0;
    }
    return code;
}

void xhci_ep_recover(xhci_controller_t* ctrl, xhci_device_slot_t* slot, uint8_t dci)
{
    if (!ctrl || !slot || !slot->endpoints || dci < 1 || dci > XHCI_MAX_DCI) {
        return;
    }

    xhci_ring_t* ring = (dci == 1) ? slot->ep0_ring : slot->endpoints[dci].ring;
    if (!ring) {
        return;
    }

    /*
     * Two commands are about to be posted, and a command for a device that has
     * gone is not free: the controller holds it, the ring is executed in
     * order, and the slot cannot be taken down while anything is outstanding
     * for it. Measured on a live board, where a departed device does not go
     * quiet but answers `Endpoint Not Enabled`: `Reset Endpoint on slot 16
     * took 3552 ms`, `Set TR Dequeue Pointer on slot 11 took 3567 ms`, over
     * and over.
     *
     * A halted endpoint on a place nobody is in has nothing to be cleared FOR:
     * the whole context is about to be handed back. Every enumeration state
     * still passes this — the question is only whether the place is occupied,
     * not whether the device has finished arriving.
     */
    if (!xhci_slot_is_live(slot)) {
        return;
    }

    kprintf("[xHCI] slot %u endpoint %u halted — clearing it\n",
            slot->slot_id, dci);

    if (xhci_post_reset_endpoint_cmd(ctrl, slot, slot->slot_id, dci) < 0) {
        return;
    }

    /* Resume where software stands. The reset leaves the controller's dequeue
     * pointer on the transfer that halted; the ring's enqueue position is
     * where the next one will be written. */
    uint64_t resume = ring->trbs_phys +
                      (uint64_t)ring->enqueue_idx * sizeof(xhci_trb_t);

    /* And the TRBs being stepped over give their slots back: nothing will ever
     * execute them, so nothing will ever answer for them. */
    xhci_ring_abandon(ring);

    xhci_post_set_tr_dequeue_cmd(ctrl, slot, slot->slot_id, dci,
                                 resume | (ring->cycle_state ? 1u : 0u));
}
