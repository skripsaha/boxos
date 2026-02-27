#ifndef XHCI_ENUMERATION_H
#define XHCI_ENUMERATION_H

#include "ktypes.h"
#include "xhci.h"
#include "usb_descriptors.h"
#include "xhci_hid.h"

typedef enum {
    ENUM_STATE_IDLE = 0,
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

struct xhci_device_slot {
    uint8_t slot_id;
    uint8_t port_num;
    uint8_t state;
    uint8_t speed;
    uint64_t timestamp_started;

    void* dev_ctx;
    uint64_t dev_ctx_phys;
    uint64_t input_ctx_phys;

    xhci_ring_t* ep0_ring;
    uint64_t ep0_ring_phys;

    void* descriptor_buffer_virt;
    uint64_t descriptor_buffer_phys;

    usb_device_desc_t device_desc;

    usb_keyboard_info_t keyboard_info;
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

void xhci_device_slot_cleanup(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

#endif
