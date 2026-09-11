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
#include "baton.h"

#define XHCI_EP_RING_TRBS 64

uint8_t xhci_ep_type_of(uint8_t attributes, uint8_t endpoint_addr)
{
    bool in = (endpoint_addr & 0x80) != 0;

    switch (attributes & 0x03) {
        case 0: return XHCI_EP_TYPE_INVALID;
        case 1: return in ? XHCI_EP_TYPE_ISOCH_IN     : XHCI_EP_TYPE_ISOCH_OUT;
        case 2: return in ? XHCI_EP_TYPE_BULK_IN      : XHCI_EP_TYPE_BULK_OUT;
        default:return in ? XHCI_EP_TYPE_INTERRUPT_IN : XHCI_EP_TYPE_INTERRUPT_OUT;
    }
}

uint32_t xhci_input_ctx_pages(xhci_controller_t* ctrl)
{
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
    slot->max_dci = 1;
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
        return 0;
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

static uint8_t ep_interval_for(uint8_t speed, uint8_t type, uint8_t bInterval)
{
    if (type == XHCI_EP_TYPE_BULK_IN || type == XHCI_EP_TYPE_BULK_OUT) {
        return 0;
    }

    if (speed == XHCI_PORT_SPEED_FULL || speed == XHCI_PORT_SPEED_LOW) {
        if (type == XHCI_EP_TYPE_ISOCH_IN || type == XHCI_EP_TYPE_ISOCH_OUT) {
            uint8_t exponent = bInterval ? bInterval : 1;
            if (exponent > 16) {
                exponent = 16;
            }
            return (uint8_t)(exponent - 1 + 3);
        }

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

static uint32_t ep_max_esit_payload(const xhci_endpoint_t* ep)
{
    if (!ep_is_periodic(ep->type)) {
        return 0;
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

    ctx->dwords[0] = ((uint32_t)(ep->mult & 0x3) << 8) |
                     ((uint32_t)interval << 16) |
                     (((esit >> 16) & 0xFFu) << 24);

    uint32_t cerr = (ep->type == XHCI_EP_TYPE_ISOCH_IN ||
                     ep->type == XHCI_EP_TYPE_ISOCH_OUT) ? 0u : 3u;

    ctx->dwords[1] = (cerr << 1) | ((uint32_t)ep->type << 3) |
                     ((uint32_t)ep->max_burst << 8) |
                     ((uint32_t)ep->max_packet << 16);

    uint64_t ring_addr = ep->ring->trbs_phys;
    ctx->dwords[2] = (uint32_t)(ring_addr & 0xFFFFFFF0u) | 1u;
    ctx->dwords[3] = (uint32_t)(ring_addr >> 32);

    ctx->dwords[4] = (uint32_t)ep->max_packet | ((esit & 0xFFFFu) << 16);
}

void xhci_ep_context_self_test(void)
{
    static const struct {
        uint8_t speed, type, bInterval, want;
    } cases[] = {
        { XHCI_PORT_SPEED_FULL,  XHCI_EP_TYPE_INTERRUPT_IN,   1,  3 },
        { XHCI_PORT_SPEED_FULL,  XHCI_EP_TYPE_INTERRUPT_IN,   8,  6 },
        { XHCI_PORT_SPEED_FULL,  XHCI_EP_TYPE_INTERRUPT_IN, 255, 10 },
        { XHCI_PORT_SPEED_LOW,   XHCI_EP_TYPE_INTERRUPT_IN,  16,  7 },
        { XHCI_PORT_SPEED_FULL,  XHCI_EP_TYPE_ISOCH_IN,       1,  3 },
        { XHCI_PORT_SPEED_FULL,  XHCI_EP_TYPE_ISOCH_OUT,      4,  6 },
        { XHCI_PORT_SPEED_FULL,  XHCI_EP_TYPE_ISOCH_IN,      16, 18 },
        { XHCI_PORT_SPEED_HIGH,  XHCI_EP_TYPE_INTERRUPT_IN,   4,  3 },
        { XHCI_PORT_SPEED_HIGH,  XHCI_EP_TYPE_ISOCH_IN,       1,  0 },
        { XHCI_PORT_SPEED_SUPER, XHCI_EP_TYPE_ISOCH_OUT,     16, 15 },
        { XHCI_PORT_SPEED_SUPER, XHCI_EP_TYPE_BULK_IN,        0,  0 },
    };

    unsigned pass = 0, total = 0;

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        total++;
        uint8_t got = ep_interval_for(cases[i].speed, cases[i].type,
                                      cases[i].bInterval);
        if (got == cases[i].want) {
            pass++;
        } else {
            kprintf("[xHCI] endpoint-context self-test: speed %u type %u "
                    "bInterval %u gave interval %u, the specification says "
                    "%u\n", cases[i].speed, cases[i].type, cases[i].bInterval,
                    got, cases[i].want);
        }
    }

    static const struct { uint8_t type, want; } cerr_cases[] = {
        { XHCI_EP_TYPE_ISOCH_IN,     0 },
        { XHCI_EP_TYPE_ISOCH_OUT,    0 },
        { XHCI_EP_TYPE_BULK_IN,      3 },
        { XHCI_EP_TYPE_INTERRUPT_IN, 3 },
    };

    for (unsigned i = 0; i < sizeof(cerr_cases) / sizeof(cerr_cases[0]); i++) {
        total++;
        xhci_endpoint_t probe;
        xhci_endpoint_context_t ctx;
        xhci_ring_t fake_ring;
        memset(&fake_ring, 0, sizeof(fake_ring));
        memset(&probe, 0, sizeof(probe));
        memset(&ctx, 0, sizeof(ctx));
        probe.type       = cerr_cases[i].type;
        probe.max_packet = 64;
        probe.ring       = &fake_ring;

        ep_write_context(&probe, XHCI_PORT_SPEED_HIGH, &ctx);
        uint8_t got = (uint8_t)((ctx.dwords[1] >> 1) & 0x3u);
        if (got == cerr_cases[i].want) {
            pass++;
        } else {
            kprintf("[xHCI] endpoint-context self-test: type %u gave CErr %u, "
                    "the specification says %u\n",
                    cerr_cases[i].type, got, cerr_cases[i].want);
        }
    }

    kprintf("[xHCI] endpoint-context self-test %u/%u\n", pass, total);
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
                     Baton* done)
{
    if (!ctrl || !slot || !slot->endpoints || dci < 2 || dci > XHCI_MAX_DCI) {
        return -1;
    }

    xhci_endpoint_t* ep = &slot->endpoints[dci];
    if (!ep->active || !ep->ring) {
        return -1;
    }

    if (ep->xfer_state == XHCI_XFER_IN_FLIGHT) {
        return -2;
    }

    xhci_trb_t trb = {0};
    trb.parameter = buffer_phys;
    trb.status    = length;
    trb.control   = TRB_SET_TYPE(TRB_TYPE_NORMAL) | TRB_IOC | TRB_ISP;

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
                         struct Baton* done)
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
        return;
    }
    if (trb_phys != 0 && ep->xfer_trb_phys != 0 && trb_phys != ep->xfer_trb_phys) {
        return;
    }

    ep->xfer_code     = completion_code;
    ep->xfer_residual = residual;
    __sync_synchronize();
    ep->xfer_state    = XHCI_XFER_DONE;

    struct Baton* done = ep->xfer_done;
    if (done) {
        ep->xfer_done = NULL;
        BatonPass(done);
    }
}

static const char* ep_wait_is_hopeless(xhci_controller_t* ctrl,
                                       xhci_device_slot_t* slot, uint8_t dci)
{
    if (!xhci_slot_is_live(slot)) {
        return "the device has gone";
    }

    if (slot->port_num != 0 && xhci_port_says_gone(ctrl, slot->port_num)) {
        return "nothing is attached to its port any more";
    }

    uint8_t state = xhci_ep_context_state(ctrl, slot, dci);
    if (state == XHCI_EP_STATE_ERROR) {
        return "the pipe it is on has gone to Error";
    }
    if (state == XHCI_EP_STATE_DISABLED) {
        return "the pipe it is on has been taken away";
    }

    if (ctrl->error_state) {
        return "the controller has stopped, so nobody is left to answer";
    }
    return NULL;
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
        xhci_process_events();

        if (ep->xfer_state == XHCI_XFER_DONE) {
            break;
        }

        const char* hopeless = ep_wait_is_hopeless(ctrl, slot, dci);
        if (!hopeless && (int64_t)(rdtsc() - deadline) >= 0) {
            hopeless = "it has not answered in the time it was given";
        }

        if (hopeless) {
            kprintf("[xHCI] slot %u endpoint %u: %s — taking the transfer "
                    "back\n", slot->slot_id, dci, hopeless);
            xhci_ep_abandon(ctrl, slot, dci);
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

    if (out_transferred) {
        *out_transferred = (residual <= length) ? (length - residual) : 0;
    }
    return code;
}

uint8_t xhci_ep_context_state(xhci_controller_t* ctrl,
                              const xhci_device_slot_t* slot, uint8_t dci)
{
    if (!ctrl || !slot || !slot->dev_ctx || dci < 1 || dci > XHCI_MAX_DCI) {
        return XHCI_EP_STATE_DISABLED;
    }

    const uint8_t* ctx = (const uint8_t*)slot->dev_ctx;
    const xhci_endpoint_context_t* ep_ctx =
        (const xhci_endpoint_context_t*)(ctx + (size_t)ctrl->context_size * dci);

    return (uint8_t)(ep_ctx->dwords[0] & 0x7u);
}

const char* xhci_ep_state_name(uint8_t state)
{
    switch (state) {
        case XHCI_EP_STATE_DISABLED: return "Disabled";
        case XHCI_EP_STATE_RUNNING:  return "Running";
        case XHCI_EP_STATE_HALTED:   return "Halted";
        case XHCI_EP_STATE_STOPPED:  return "Stopped";
        case XHCI_EP_STATE_ERROR:    return "Error";
        default:                     return "a reserved value";
    }
}

static void ep_resume_where_software_stands(xhci_controller_t* ctrl,
                                            xhci_device_slot_t* slot,
                                            uint8_t dci, xhci_ring_t* ring)
{
    uint64_t resume = ring->trbs_phys +
                      (uint64_t)ring->enqueue_idx * sizeof(xhci_trb_t);

    xhci_ring_abandon(ring);

    xhci_post_set_tr_dequeue_cmd(ctrl, slot, slot->slot_id, dci,
                                 resume | (ring->cycle_state ? 1u : 0u));
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

    if (!xhci_slot_is_live(slot)) {
        return;
    }

    uint8_t state = xhci_ep_context_state(ctrl, slot, dci);
    if (state != XHCI_EP_STATE_HALTED) {
        debug_printf("[xHCI] slot %u endpoint %u is %s, not halted — nothing "
                     "to clear\n", slot->slot_id, dci,
                     xhci_ep_state_name(state));
        return;
    }

    kprintf("[xHCI] slot %u endpoint %u halted — clearing it\n",
            slot->slot_id, dci);

    if (xhci_post_reset_endpoint_cmd(ctrl, slot, slot->slot_id, dci) < 0) {
        return;
    }

    ep_resume_where_software_stands(ctrl, slot, dci, ring);
}

void xhci_ep_abandon(xhci_controller_t* ctrl, xhci_device_slot_t* slot, uint8_t dci)
{
    if (!ctrl || !slot || !slot->endpoints || dci < 1 || dci > XHCI_MAX_DCI) {
        return;
    }

    xhci_endpoint_t* ep = &slot->endpoints[dci];
    if (ep->xfer_state != XHCI_XFER_IN_FLIGHT) {
        return;
    }

    ep->xfer_done = NULL;

    xhci_ring_t* ring = (dci == 1) ? slot->ep0_ring : ep->ring;

    if (ring && xhci_slot_is_live(slot)) {
        uint8_t state = xhci_ep_context_state(ctrl, slot, dci);

        if (state == XHCI_EP_STATE_RUNNING) {
            kprintf("[xHCI] slot %u endpoint %u: the transfer on it is no "
                    "longer wanted — stopping it\n", slot->slot_id, dci);
            if (xhci_post_stop_endpoint_cmd(ctrl, slot, slot->slot_id, dci) == 0) {
                xhci_command_wait_idle(ctrl, XHCI_CMD_TIMEOUT_MS);
                ep_resume_where_software_stands(ctrl, slot, dci, ring);
                xhci_command_wait_idle(ctrl, XHCI_CMD_TIMEOUT_MS);
            }
        } else if (state == XHCI_EP_STATE_HALTED) {
            xhci_ep_recover(ctrl, slot, dci);
            xhci_command_wait_idle(ctrl, XHCI_CMD_TIMEOUT_MS);
        } else if (state == XHCI_EP_STATE_ERROR) {
            ep_resume_where_software_stands(ctrl, slot, dci, ring);
            xhci_command_wait_idle(ctrl, XHCI_CMD_TIMEOUT_MS);
        }
    }

    ep->xfer_trb_phys = 0;
    ep->xfer_code     = 0;
    ep->xfer_residual = 0;
    ep->xfer_state    = XHCI_XFER_IDLE;
}