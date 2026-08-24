#include "xhci_device.h"
#include "xhci_enumeration.h"
#include "klib.h"

/*
 * Slot Context dword layout (xHCI 1.2 Section 6.2.2):
 *   dword0: Route String[19:0], Speed[23:20], MTT[25], Hub[26], Context Entries[31:27]
 *   dword1: Max Exit Latency[15:0], Root Hub Port Number[23:16], Num Ports[31:24]
 *   dword2: Parent Hub Slot ID[7:0], Parent Port Num[15:8], TTT[17:16], Interrupter Target[31:22]
 *   dword3: USB Device Address[7:0], Slot State[31:27]
 *   dword4-7: Reserved
 *
 * The route string is the part that is easy to get wrong and impossible to
 * notice: four bits per tier naming which hub port was taken, and a device
 * whose route is zero is a device the controller believes is plugged into the
 * root port directly. It will then address whatever actually is.
 */
void xhci_fill_slot_context(xhci_slot_context_t* slot_ctx,
                            const struct xhci_device_slot* slot) {
    if (!slot_ctx || !slot) return;
    memset(slot_ctx, 0, sizeof(xhci_slot_context_t));

    /* Context Entries starts at 1 (slot + EP0); whoever adds endpoints raises
     * it to the highest Device Context Index in use. */
    slot_ctx->dwords[0] = (1u << 27)
                        | ((uint32_t)slot->speed << 20)
                        | (slot->route_string & 0x000FFFFFu);

    if (slot->hub_ports > 0) {
        slot_ctx->dwords[0] |= (1u << 26);              /* this device is a hub */
        if (slot->multi_tt) {
            slot_ctx->dwords[0] |= (1u << 25);          /* one translator per port */
        }
    }

    slot_ctx->dwords[1] = ((uint32_t)slot->port_num << 16)
                        | ((uint32_t)slot->hub_ports << 24);

    /* The translator, when one stands in the way: which hub is doing it and on
     * which of its ports. Left zero for a device the controller can reach at
     * its own speed. */
    if (slot->tt_slot_id != 0) {
        slot_ctx->dwords[2] = (uint32_t)slot->tt_slot_id
                            | ((uint32_t)slot->tt_port << 8);
    }
    if (slot->hub_ports > 0) {
        slot_ctx->dwords[2] |= ((uint32_t)(slot->tt_think_time & 0x3) << 16);
    }
}

/*
 * Endpoint Context dword layout (xHCI 1.2 Section 6.2.3):
 *   dword0: EP State[2:0], Mult[9:8], MaxPStreams[14:10], Interval[23:16], MaxESITPayloadHi[31:24]
 *   dword1: CErr[2:1], EP Type[5:3], HID[7], Max Burst Size[15:8], Max Packet Size[31:16]
 *   dword2: TR Dequeue Pointer Low[31:4] | DCS[0]
 *   dword3: TR Dequeue Pointer High
 *   dword4: Average TRB Length[15:0], Max ESIT Payload Lo[31:16]
 *   dword5-7: Reserved
 */
void xhci_init_ep0_context(xhci_endpoint_context_t* ep0_ctx, uint64_t ring_phys, uint16_t max_packet) {
    if (!ep0_ctx) return;
    memset(ep0_ctx, 0, sizeof(xhci_endpoint_context_t));
    /* dword1: CErr=3 (3 retries), EP Type=4 (Control Bidirectional), Max Packet Size */
    ep0_ctx->dwords[1] = (3 << 1) | (4 << 3) | ((uint32_t)max_packet << 16);
    /* dword2: TR Dequeue Pointer Low | DCS=1 */
    ep0_ctx->dwords[2] = (uint32_t)(ring_phys & 0xFFFFFFF0) | 1;
    /* dword3: TR Dequeue Pointer High */
    ep0_ctx->dwords[3] = (uint32_t)(ring_phys >> 32);
    /* dword4: Average TRB Length = 8 (for control endpoint setup packets) */
    ep0_ctx->dwords[4] = 8;
}
