#include "xhci_device.h"
#include "xhci_enumeration.h"
#include "klib.h"

void xhci_fill_slot_context(xhci_slot_context_t* slot_ctx,
                            const struct xhci_device_slot* slot) {
    if (!slot_ctx || !slot) return;
    memset(slot_ctx, 0, sizeof(xhci_slot_context_t));

    slot_ctx->dwords[0] = (1u << 27)
                        | ((uint32_t)slot->speed << 20)
                        | (slot->route_string & 0x000FFFFFu);

    if (slot->hub_ports > 0) {
        slot_ctx->dwords[0] |= (1u << 26);
        if (slot->multi_tt) {
            slot_ctx->dwords[0] |= (1u << 25);
        }
    }

    slot_ctx->dwords[1] = ((uint32_t)slot->port_num << 16)
                        | ((uint32_t)slot->hub_ports << 24);

    if (slot->tt_slot_id != 0) {
        slot_ctx->dwords[2] = (uint32_t)slot->tt_slot_id
                            | ((uint32_t)slot->tt_port << 8);
    }
    if (slot->hub_ports > 0) {
        slot_ctx->dwords[2] |= ((uint32_t)(slot->tt_think_time & 0x3) << 16);
    }
}

void xhci_init_ep0_context(xhci_endpoint_context_t* ep0_ctx,
                           uint64_t dequeue_with_dcs, uint16_t max_packet) {
    if (!ep0_ctx) return;
    memset(ep0_ctx, 0, sizeof(xhci_endpoint_context_t));
    ep0_ctx->dwords[1] = (3 << 1) | (4 << 3) | ((uint32_t)max_packet << 16);
    ep0_ctx->dwords[2] = (uint32_t)(dequeue_with_dcs & 0xFFFFFFF1u);
    ep0_ctx->dwords[3] = (uint32_t)(dequeue_with_dcs >> 32);
    ep0_ctx->dwords[4] = 8;
}

uint8_t xhci_slot_context_state(const struct xhci_device_slot* slot)
{
    if (!slot || !slot->dev_ctx) {
        return XHCI_SLOT_STATE_DISABLED;
    }
    const xhci_device_context_t* ctx = (const xhci_device_context_t*)slot->dev_ctx;
    return (uint8_t)((ctx->slot.dwords[3] >> 27) & 0x1F);
}

uint8_t xhci_slot_context_address(const struct xhci_device_slot* slot)
{
    if (!slot || !slot->dev_ctx) {
        return 0;
    }
    const xhci_device_context_t* ctx = (const xhci_device_context_t*)slot->dev_ctx;
    return (uint8_t)(ctx->slot.dwords[3] & 0xFF);
}

const char* xhci_slot_state_name(uint8_t state)
{
    switch (state) {
        case XHCI_SLOT_STATE_DISABLED:   return "Disabled/Enabled";
        case XHCI_SLOT_STATE_DEFAULT:    return "Default";
        case XHCI_SLOT_STATE_ADDRESSED:  return "Addressed";
        case XHCI_SLOT_STATE_CONFIGURED: return "Configured";
        default:                         return "a reserved value";
    }
}