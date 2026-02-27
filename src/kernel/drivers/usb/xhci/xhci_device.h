#ifndef XHCI_DEVICE_H
#define XHCI_DEVICE_H

#include "ktypes.h"

typedef struct {
    uint32_t dwords[8];
} __attribute__((packed, aligned(32))) xhci_slot_context_t;

typedef struct {
    uint32_t dwords[4];
} __attribute__((packed, aligned(16))) xhci_endpoint_context_t;

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
    uint32_t config_value;
    uint32_t interface_number;
    uint32_t alternate_setting;
    uint32_t reserved2;
} __attribute__((packed)) xhci_input_control_context_t;

typedef struct {
    xhci_input_control_context_t input_control;   // 44 bytes
    uint8_t padding[20];                           // Padding to 64-byte boundary
    xhci_device_context_t device_context;          // 64-byte aligned
} __attribute__((packed, aligned(64))) xhci_input_context_t;

_Static_assert(sizeof(xhci_input_control_context_t) == 44, "Must be 44 bytes");

void xhci_init_slot_context(xhci_slot_context_t* slot_ctx, uint8_t port, uint32_t speed);
void xhci_init_ep0_context(xhci_endpoint_context_t* ep0_ctx, uint64_t ring_phys, uint16_t max_packet);

#endif
