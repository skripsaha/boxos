#include "xhci_device.h"
#include "klib.h"

void xhci_init_slot_context(xhci_slot_context_t* slot_ctx, uint8_t port, uint32_t speed) {
    if (!slot_ctx) {
        return;
    }

    memset(slot_ctx, 0, sizeof(xhci_slot_context_t));

    // NOTE: Context Entries field set by caller (varies by use case)
    slot_ctx->dwords[0] = (speed << 20);  // Speed only, no Context Entries
    slot_ctx->dwords[1] = port << 16;
}

void xhci_init_ep0_context(xhci_endpoint_context_t* ep0_ctx, uint64_t ring_phys, uint16_t max_packet) {
    if (!ep0_ctx) {
        return;
    }

    memset(ep0_ctx, 0, sizeof(xhci_endpoint_context_t));

    ep0_ctx->dwords[0] = (max_packet << 16);
    ep0_ctx->dwords[1] = (3 << 1) | (4 << 3);
    ep0_ctx->dwords[2] = (uint32_t)(ring_phys & 0xFFFFFFFF) | (1 << 0);
    ep0_ctx->dwords[3] = (uint32_t)(ring_phys >> 32);
}
