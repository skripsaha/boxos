#ifndef XHCI_ENUMERATION_H
#define XHCI_ENUMERATION_H

#include "ktypes.h"
#include "xhci.h"
#include "usb_descriptors.h"
#include "xhci_hid.h"

typedef enum {
    ENUM_STATE_IDLE = 0,
    ENUM_STATE_CLAIMING,
    ENUM_STATE_RESETTING_PORT,
    ENUM_STATE_WAIT_ENABLE_SLOT,
    ENUM_STATE_WAIT_ADDRESS_DEVICE,
    ENUM_STATE_WAIT_EVALUATE_CONTEXT,
    ENUM_STATE_WAIT_GET_DESCRIPTOR,
    ENUM_STATE_WAIT_GET_CONFIG_DESC,
    ENUM_STATE_WAIT_SET_CONFIGURATION,
    ENUM_STATE_WAIT_SET_PROTOCOL,
    ENUM_STATE_WAIT_SET_IDLE,
    ENUM_STATE_WAIT_CONFIGURE_ENDPOINT,
    ENUM_STATE_CONFIGURED,
    ENUM_STATE_ERROR = 255
} xhci_enum_state_t;

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
xhci_device_slot_t* xhci_get_device_slot(xhci_controller_t* ctrl, uint8_t slot_id);
xhci_device_slot_t* xhci_get_device_slot_by_port(uint8_t port);
void xhci_device_slot_cleanup(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

#endif
