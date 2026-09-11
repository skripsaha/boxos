#ifndef XHCI_ENUMERATION_H
#define XHCI_ENUMERATION_H

#include "ktypes.h"
#include "xhci.h"
#include "usb_descriptors.h"

typedef enum {
    ENUM_STATE_IDLE = 0,
    ENUM_STATE_CLAIMING,

    ENUM_STATE_QUEUED,
    ENUM_STATE_WAIT_PORT_RESET,

    ENUM_STATE_WAIT_RESET_RECOVERY,

    ENUM_STATE_WAIT_ENABLE_SLOT,

    ENUM_STATE_WAIT_ADDRESS_DEVICE_BSR,

    ENUM_STATE_WAIT_GET_DESC_HEADER,

    ENUM_STATE_WAIT_ADDRESS_DEVICE,

    ENUM_STATE_WAIT_GET_DESCRIPTOR,
    ENUM_STATE_WAIT_GET_CONFIG_HEADER,
    ENUM_STATE_WAIT_GET_CONFIG_DESC,
    ENUM_STATE_WAIT_SET_CONFIGURATION,
    ENUM_STATE_WAIT_SET_PROTOCOL,
    ENUM_STATE_WAIT_SET_IDLE,
    ENUM_STATE_WAIT_EP0_RESET,

    ENUM_STATE_WAIT_EP0_STOP,

    ENUM_STATE_WAIT_EP0_DEQUEUE,
    ENUM_STATE_WAIT_CONFIGURE_ENDPOINT,
    ENUM_STATE_CONFIGURED,

    ENUM_STATE_RETIRING
} xhci_enum_state_t;


int xhci_slots_attach(xhci_controller_t* ctrl);

void xhci_slots_release(xhci_controller_t* ctrl);

struct xhci_device_slot {
    xhci_controller_t* ctrl;

    uint32_t epoch;

    uint8_t slot_id;
    uint8_t port_num;
    uint8_t state;
    uint8_t speed;

    uint64_t timestamp_started;

    uint8_t  watch_state;
    uint64_t watch_since;

    void* dev_ctx;
    uint64_t dev_ctx_phys;

    uint64_t input_ctx_phys;

    xhci_ring_t* ep0_ring;
    uint64_t ep0_ring_phys;

    void* descriptor_buffer_virt;
    uint64_t descriptor_buffer_phys;

    usb_device_desc_t device_desc;

    uint16_t ep0_max_packet;

    uint16_t config_total_len;

    uint8_t  step_resume;

    uint8_t  step_retry;

    bool     step_reissue;

    uint64_t ctl_data_trb;
    uint64_t ctl_status_trb;
    uint16_t ctl_requested;
    uint16_t ctl_received;

    uint8_t  reset_kind;
    uint32_t reset_took_ms;

    uint64_t recovery_due;

    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;

    uint32_t route_string;
    uint8_t  depth;
    uint8_t  parent_slot_id;
    uint8_t  parent_port;
    uint8_t  tt_slot_id;
    uint8_t  tt_port;

    uint8_t  hub_ports;
    uint8_t  tt_think_time;
    bool     multi_tt;

    uint8_t  config_value;
    uint8_t  interface_num;

    struct xhci_endpoint* endpoints;
    uint8_t  max_dci;
    uint32_t ep_pending_add;

    volatile uint32_t visitors;

    uint64_t retire_started;
    bool     retire_warned;

    uint8_t  born_port;
    bool     ever_configured;

    uint8_t  driver;
    uint8_t  ep_interrupt_in;
    uint8_t  ep_bulk_in;
    uint8_t  ep_bulk_out;
};

#define XHCI_DRIVER_NONE     0
#define XHCI_DRIVER_KEYBOARD 1
#define XHCI_DRIVER_STORAGE  2
#define XHCI_DRIVER_HUB      3

void xhci_enumeration_init(void);
int xhci_enumerate_device(xhci_controller_t* ctrl, uint8_t port);

int xhci_enumerate_behind_hub(xhci_controller_t* ctrl,
                              xhci_device_slot_t* hub,
                              uint8_t hub_port, uint8_t speed);

xhci_device_slot_t* xhci_get_device_slot_by_id(xhci_controller_t* ctrl, uint8_t slot_id);
void xhci_enum_advance_state(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                             uint8_t slot_id, uint8_t completion_code);

uint32_t xhci_slot_epoch(const xhci_device_slot_t* slot);
bool     xhci_slot_still_is(const xhci_device_slot_t* slot, uint32_t epoch);

bool     xhci_slot_is_live(const xhci_device_slot_t* slot);

void xhci_enum_port_reset_done(xhci_controller_t* ctrl, uint8_t port);

#define XHCI_ENUM_TIMEOUT_MS 2000

void xhci_enum_watchdog(xhci_controller_t* ctrl);

void xhci_enum_pump(xhci_controller_t* ctrl);

int xhci_enum_settle(xhci_controller_t* ctrl, uint32_t timeout_ms);

typedef void (*xhci_slot_visitor)(void* ctx, xhci_device_slot_t* slot);
void xhci_enum_for_each_configured(xhci_controller_t* ctrl,
                                   xhci_slot_visitor visit, void* ctx);

bool xhci_enum_stall_is_tolerable(uint8_t state);

bool xhci_enum_fault_is_retryable(uint8_t completion_code);

bool xhci_enum_step_can_be_asked_again(uint8_t state);

const char* xhci_enum_state_name(uint8_t state);

#define XHCI_STEP_RETRIES 3

#define XHCI_ENUM_ATTEMPTS 3

void xhci_enum_recover_ep0(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                           uint8_t resume_at, bool ask_again);
xhci_device_slot_t* xhci_get_device_slot(xhci_controller_t* ctrl, uint8_t slot_id);
xhci_device_slot_t* xhci_get_device_slot_by_port(xhci_controller_t* ctrl, uint8_t port);


bool xhci_slot_enter(xhci_device_slot_t* slot);
void xhci_slot_leave(xhci_device_slot_t* slot);

void xhci_slot_retire(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

int  xhci_slot_service(xhci_controller_t* ctrl);

bool xhci_slot_retire_pending(void);
void xhci_slot_service_if_pending(void);

#define XHCI_RETIRE_PATIENCE_MS 15000

#endif