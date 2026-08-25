#ifndef XHCI_ENDPOINT_H
#define XHCI_ENDPOINT_H

#include "ktypes.h"
#include "xhci.h"
#include "xhci_rings.h"
#include "usb_descriptors.h"

/* Endpoint types as the controller numbers them (xHCI Table 6-9). The value
 * goes straight into the endpoint context, so these are the controller's
 * numbers and not a private encoding. */
#define XHCI_EP_TYPE_INVALID        0
#define XHCI_EP_TYPE_ISOCH_OUT      1
#define XHCI_EP_TYPE_BULK_OUT       2
#define XHCI_EP_TYPE_INTERRUPT_OUT  3
#define XHCI_EP_TYPE_CONTROL        4
#define XHCI_EP_TYPE_ISOCH_IN       5
#define XHCI_EP_TYPE_BULK_IN        6
#define XHCI_EP_TYPE_INTERRUPT_IN   7

/* A device context has room for 31 endpoints beyond the slot, and the Device
 * Context Index that names one is five bits wide. 32 entries is therefore the
 * whole of what the hardware can address, not a limit chosen here. */
#define XHCI_MAX_DCI 31
#define XHCI_DCI_COUNT (XHCI_MAX_DCI + 1)

/* Where a transfer stands. One transfer may be in flight per endpoint, which
 * is not a simplification but a description: Bulk-Only Transport is a strictly
 * serial conversation (command, then data, then status), and an interrupt
 * endpoint re-arms one report at a time. An endpoint that needs a deeper queue
 * needs a different structure, and saying so here is cheaper than pretending
 * this one has it. */
#define XHCI_XFER_IDLE      0
#define XHCI_XFER_IN_FLIGHT 1
#define XHCI_XFER_DONE      2

typedef struct xhci_endpoint {
    xhci_ring_t* ring;
    uint64_t     ring_page_phys;    /* the page the ring structure itself is in */

    /* Some endpoints own a buffer for their whole life — an interrupt endpoint
     * re-armed with the same report page. Bulk endpoints are handed one per
     * transfer by whoever asked for it, and leave these zero. */
    void*        buffer_virt;
    uint64_t     buffer_phys;
    uint32_t     buffer_bytes;

    uint16_t     max_packet;
    uint8_t      addr;              /* bEndpointAddress as the device stated it */
    uint8_t      type;              /* XHCI_EP_TYPE_* */
    uint8_t      interval;          /* bInterval as the device stated it */

    /* How many packets may go back to back, one less than the count, and for
     * a periodic endpoint how many bytes that comes to per service interval.
     * Both as the device stated them — from the SuperSpeed companion where
     * there is one, from the packet-size field where there is not. */
    uint8_t      max_burst;
    uint8_t      mult;
    uint16_t     bytes_per_interval;

    bool         active;

    /* The transfer currently in flight on this endpoint, and what became of
     * it. Written by the event handler, read by whoever is waiting. */
    volatile uint64_t xfer_trb_phys;
    volatile uint8_t  xfer_state;
    volatile uint8_t  xfer_code;
    volatile uint32_t xfer_residual;   /* bytes the device did NOT transfer */
} xhci_endpoint_t;

/* Device Context Index of an endpoint address: the endpoint number doubled,
 * plus one when the direction is IN. EP0 is DCI 1 and is bidirectional. */
static inline uint8_t xhci_dci_of(uint8_t endpoint_addr) {
    uint8_t num = endpoint_addr & 0x0F;
    if (num == 0) return 1;
    return (uint8_t)(num * 2 + ((endpoint_addr & 0x80) ? 1 : 0));
}

/* Endpoint type from a bmAttributes/direction pair, as the descriptor gives
 * them. Returns XHCI_EP_TYPE_INVALID for a control endpoint that is not EP0,
 * which this driver does not configure. */
uint8_t xhci_ep_type_of(uint8_t attributes, uint8_t endpoint_addr);

/* Give a slot its endpoint table. Called once, when the slot is enabled. */
int  xhci_ep_table_alloc(xhci_device_slot_t* slot);
void xhci_ep_table_free(xhci_device_slot_t* slot);

/* Prepare one endpoint: transfer ring, optional persistent buffer, and the
 * bookkeeping. Does not talk to the controller — xhci_ep_configure does that
 * for every prepared endpoint at once, because Configure Endpoint takes them
 * together and a command per endpoint would be three round trips where the
 * hardware asked for one. */
uint32_t xhci_input_ctx_pages(xhci_controller_t* ctrl);

int  xhci_ep_prepare(xhci_device_slot_t* slot, uint8_t dci, uint8_t type,
                     const usb_endpoint_info_t* info, uint32_t buffer_bytes);

/* Post Configure Endpoint for every prepared endpoint that the controller does
 * not yet know about. The slot advances on the command completion. */
int  xhci_ep_configure(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

/* Submit one transfer and return without waiting. */
int  xhci_ep_submit(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                    uint8_t dci, uint64_t buffer_phys, uint32_t length);

/* Wait for the transfer submitted above.
 *
 * The wait drains the event ring itself rather than trusting that an interrupt
 * will arrive to do it — the same code has to work when this is called with
 * interrupts disabled, on a controller with no usable interrupt at all, and
 * before any of the machinery that would otherwise deliver one exists. Draining
 * is safe from here because the event ring is under a lock the interrupt
 * handler takes too.
 *
 * Reports the RESIDUAL — what the device did not transfer — because that is
 * what the event carries. xhci_ep_transfer turns it into the number everyone
 * actually wants.
 *
 * Returns the completion code, or a negative value on timeout. */
int  xhci_ep_wait(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                  uint8_t dci, uint32_t timeout_ms, uint32_t* out_residual);

/* Submit and wait, which is what every caller that is not an interrupt
 * endpoint actually wants. */
int  xhci_ep_transfer(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                      uint8_t dci, uint64_t buffer_phys, uint32_t length,
                      uint32_t timeout_ms, uint32_t* out_transferred);

/* Clear a halted endpoint and put its dequeue pointer where software is. */
void xhci_ep_recover(xhci_controller_t* ctrl, xhci_device_slot_t* slot, uint8_t dci);

/* Record a completion against the endpoint it belongs to. Called from the
 * transfer-event handler. */
void xhci_ep_complete(xhci_device_slot_t* slot, uint8_t dci,
                      uint8_t completion_code, uint32_t residual,
                      uint64_t trb_phys);

#endif
