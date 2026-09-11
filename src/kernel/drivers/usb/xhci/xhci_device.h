#ifndef XHCI_DEVICE_H
#define XHCI_DEVICE_H

#include "ktypes.h"


typedef struct {
    uint32_t dwords[8];
} __attribute__((packed, aligned(32))) xhci_slot_context_t;

typedef struct {
    uint32_t dwords[8];
} __attribute__((packed, aligned(32))) xhci_endpoint_context_t;

typedef struct {
    xhci_slot_context_t slot;
    xhci_endpoint_context_t endpoints[31];
} __attribute__((packed, aligned(64))) xhci_device_context_t;

typedef struct {
    uint64_t device_context_ptrs[256];
} __attribute__((packed, aligned(64))) xhci_dcbaa_t;

typedef struct {
    uint32_t drop_context_flags;
    uint32_t add_context_flags;
    uint32_t reserved[5];
    uint32_t config_info;
} __attribute__((packed, aligned(32))) xhci_input_control_context_t;

_Static_assert(sizeof(xhci_slot_context_t) == 32, "Slot Context must be 32 bytes");
_Static_assert(sizeof(xhci_endpoint_context_t) == 32, "Endpoint Context must be 32 bytes");
_Static_assert(sizeof(xhci_input_control_context_t) == 32, "Input Control Context must be 32 bytes");
_Static_assert(sizeof(xhci_device_context_t) == 1024, "Device Context must be 1024 bytes (32-byte mode)");

struct xhci_device_slot;
void xhci_fill_slot_context(xhci_slot_context_t* slot_ctx,
                            const struct xhci_device_slot* slot);
void xhci_init_ep0_context(xhci_endpoint_context_t* ep0_ctx,
                           uint64_t dequeue_with_dcs, uint16_t max_packet);

#define XHCI_SLOT_STATE_DISABLED   0
#define XHCI_SLOT_STATE_DEFAULT    1
#define XHCI_SLOT_STATE_ADDRESSED  2
#define XHCI_SLOT_STATE_CONFIGURED 3

uint8_t     xhci_slot_context_state(const struct xhci_device_slot* slot);
uint8_t     xhci_slot_context_address(const struct xhci_device_slot* slot);
const char* xhci_slot_state_name(uint8_t state);

#endif