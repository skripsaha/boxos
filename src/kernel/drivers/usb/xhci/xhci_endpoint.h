#ifndef XHCI_ENDPOINT_H
#define XHCI_ENDPOINT_H

#include "ktypes.h"
#include "xhci.h"
#include "xhci_rings.h"
#include "usb_descriptors.h"

struct Baton;

#define XHCI_EP_TYPE_INVALID        0
#define XHCI_EP_TYPE_ISOCH_OUT      1
#define XHCI_EP_TYPE_BULK_OUT       2
#define XHCI_EP_TYPE_INTERRUPT_OUT  3
#define XHCI_EP_TYPE_CONTROL        4
#define XHCI_EP_TYPE_ISOCH_IN       5
#define XHCI_EP_TYPE_BULK_IN        6
#define XHCI_EP_TYPE_INTERRUPT_IN   7

#define XHCI_MAX_DCI 31
#define XHCI_DCI_COUNT (XHCI_MAX_DCI + 1)

#define XHCI_XFER_IDLE      0
#define XHCI_XFER_IN_FLIGHT 1
#define XHCI_XFER_DONE      2

typedef struct xhci_endpoint {
    xhci_ring_t* ring;
    uint64_t     ring_page_phys;

    void*        buffer_virt;
    uint64_t     buffer_phys;
    uint32_t     buffer_bytes;

    uint16_t     max_packet;
    uint8_t      addr;
    uint8_t      type;
    uint8_t      interval;

    uint8_t      max_burst;
    uint8_t      mult;
    uint16_t     bytes_per_interval;

    bool         active;

    volatile uint64_t xfer_trb_phys;
    volatile uint8_t  xfer_state;
    volatile uint8_t  xfer_code;
    volatile uint32_t xfer_residual;

    struct Baton* xfer_done;
} xhci_endpoint_t;

static inline uint8_t xhci_dci_of(uint8_t endpoint_addr) {
    uint8_t num = endpoint_addr & 0x0F;
    if (num == 0) return 1;
    return (uint8_t)(num * 2 + ((endpoint_addr & 0x80) ? 1 : 0));
}

uint8_t xhci_ep_type_of(uint8_t attributes, uint8_t endpoint_addr);

int  xhci_ep_table_alloc(xhci_device_slot_t* slot);
void xhci_ep_table_free(xhci_device_slot_t* slot);

uint32_t xhci_input_ctx_pages(xhci_controller_t* ctrl);

void xhci_ep_context_self_test(void);

int  xhci_ep_prepare(xhci_device_slot_t* slot, uint8_t dci, uint8_t type,
                     const usb_endpoint_info_t* info, uint32_t buffer_bytes);

int  xhci_ep_configure(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

int  xhci_ep_submit(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                    uint8_t dci, uint64_t buffer_phys, uint32_t length);

int  xhci_ep_submit_async(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                          uint8_t dci, uint64_t buffer_phys, uint32_t length,
                          struct Baton* done);

int  xhci_ep_wait(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                  uint8_t dci, uint32_t timeout_ms, uint32_t* out_residual);

int  xhci_ep_transfer(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                      uint8_t dci, uint64_t buffer_phys, uint32_t length,
                      uint32_t timeout_ms, uint32_t* out_transferred);

bool xhci_ep_take_result(xhci_device_slot_t* slot, uint8_t dci,
                         uint8_t* out_code, uint32_t* out_residual);

#define XHCI_EP_STATE_DISABLED 0
#define XHCI_EP_STATE_RUNNING  1
#define XHCI_EP_STATE_HALTED   2
#define XHCI_EP_STATE_STOPPED  3
#define XHCI_EP_STATE_ERROR    4

uint8_t     xhci_ep_context_state(xhci_controller_t* ctrl,
                                  const xhci_device_slot_t* slot, uint8_t dci);
const char* xhci_ep_state_name(uint8_t state);

void xhci_ep_recover(xhci_controller_t* ctrl, xhci_device_slot_t* slot, uint8_t dci);

void xhci_ep_abandon(xhci_controller_t* ctrl, xhci_device_slot_t* slot, uint8_t dci);

void xhci_ep_complete(xhci_device_slot_t* slot, uint8_t dci,
                      uint8_t completion_code, uint32_t residual,
                      uint64_t trb_phys);

#endif