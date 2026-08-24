#ifndef XHCI_ENUMERATION_H
#define XHCI_ENUMERATION_H

#include "ktypes.h"
#include "xhci.h"
#include "usb_descriptors.h"
#include "xhci_hid.h"

/* Enumeration is a conversation, and every step of it waits on something the
 * hardware will say when it is ready — a command completion, a transfer
 * event, a port-status change. There is no step here that waits by counting.
 * The state is what the slot is listening for next. */
typedef enum {
    ENUM_STATE_IDLE = 0,
    ENUM_STATE_CLAIMING,
    ENUM_STATE_WAIT_PORT_RESET,
    ENUM_STATE_WAIT_ENABLE_SLOT,
    ENUM_STATE_WAIT_ADDRESS_DEVICE,
    ENUM_STATE_WAIT_GET_DESC_HEADER,
    ENUM_STATE_WAIT_EVALUATE_CONTEXT,
    ENUM_STATE_WAIT_GET_DESCRIPTOR,
    ENUM_STATE_WAIT_GET_CONFIG_HEADER,
    ENUM_STATE_WAIT_GET_CONFIG_DESC,
    ENUM_STATE_WAIT_SET_CONFIGURATION,
    ENUM_STATE_WAIT_SET_PROTOCOL,
    ENUM_STATE_WAIT_SET_IDLE,
    ENUM_STATE_WAIT_EP0_RESET,
    ENUM_STATE_WAIT_EP0_DEQUEUE,
    ENUM_STATE_WAIT_CONFIGURE_ENDPOINT,
    ENUM_STATE_CONFIGURED
} xhci_enum_state_t;

/* There is no ENUM_STATE_ERROR. There used to be, and nothing could reach it:
 * every path that once set it now releases the slot instead, because a slot
 * parked in a state it can never leave holds a port hostage for the rest of
 * the boot. A failure here is a device you can unplug and plug back in. */

#define XHCI_MAX_DEVICE_SLOTS 64

struct xhci_device_slot {
    uint8_t slot_id;
    uint8_t port_num;
    uint8_t state;          /* xhci_enum_state_t */
    uint8_t speed;

    uint64_t timestamp_started;

    /* Device Context (allocated, pointed to by DCBAA) */
    void* dev_ctx;
    uint64_t dev_ctx_phys;

    /* Input Context (temporary, for commands) */
    uint64_t input_ctx_phys;

    /* EP0 Transfer Ring */
    xhci_ring_t* ep0_ring;
    uint64_t ep0_ring_phys;

    /* Descriptor buffer (one page, reused for all GET_DESCRIPTOR) */
    void* descriptor_buffer_virt;
    uint64_t descriptor_buffer_phys;

    /* Cached device descriptor */
    usb_device_desc_t device_desc;

    /* EP0 max packet size actually in the endpoint context. A full-speed
     * device names its own, and the number is not known until eight bytes of
     * its descriptor have been read using the wrong one. */
    uint16_t ep0_max_packet;

    /* How long the configuration descriptor said it was, and what the first
     * interface turned out to be. The class is what decides who drives this
     * device; a device nobody drives is still enumerated and left addressed
     * rather than abandoned half-configured. */
    uint16_t config_total_len;

    /* Where to carry on from once a stalled EP0 has been cleared. A device is
     * allowed to refuse an optional request, and refusing it halts the pipe;
     * this is the step that was being attempted when that happened. */
    uint8_t  stall_resume;
    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;

    /* HID keyboard info (filled during enumeration) */
    usb_keyboard_info_t keyboard_info;
    bool is_keyboard;

    /* Interrupt endpoint (for keyboard) */
    xhci_ring_t* interrupt_ring;
    uint64_t interrupt_ring_phys;
    void* interrupt_data_buffer_virt;
    uint64_t interrupt_data_buffer_phys;
    uint8_t keyboard_endpoint_dci;
};

void xhci_enumeration_init(void);
int xhci_enumerate_device(xhci_controller_t* ctrl, uint8_t port);
void xhci_enum_advance_state(xhci_controller_t* ctrl, uint8_t slot_id, uint8_t completion_code);

/* The port finished the reset enumeration asked for. Called from the
 * port-status change path, which is the only thing that knows when. */
void xhci_enum_port_reset_done(xhci_controller_t* ctrl, uint8_t port);

/* How long any single step of enumeration may go unanswered. Every step is one
 * round trip on the bus, so this is generous by three orders of magnitude —
 * it is here to name a failure, never to pace a success. */
#define XHCI_ENUM_TIMEOUT_MS 2000

/* Report and release any enumeration that stopped being answered. Driven from
 * the timer tick. */
void xhci_enum_watchdog(xhci_controller_t* ctrl);

/* True when a STALL at this point in enumeration is the device declining an
 * optional request rather than the conversation failing. */
bool xhci_enum_stall_is_tolerable(uint8_t state);

/* Clear a halted EP0 and resume enumeration from where it stalled. */
void xhci_enum_recover_ep0(xhci_controller_t* ctrl, xhci_device_slot_t* slot);
xhci_device_slot_t* xhci_get_device_slot(xhci_controller_t* ctrl, uint8_t slot_id);
xhci_device_slot_t* xhci_get_device_slot_by_port(uint8_t port);
void xhci_device_slot_cleanup(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

#endif
