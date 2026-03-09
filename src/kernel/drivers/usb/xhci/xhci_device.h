#ifndef XHCI_DEVICE_H
#define XHCI_DEVICE_H

#include "ktypes.h"

/* xHCI 1.2 spec: all contexts are 32 bytes (8 dwords) in 32-byte mode.
 * In 64-byte mode (CSZ=1 in HCCPARAMS1), each context is 64 bytes
 * with the extra 32 bytes reserved. We handle 64-byte mode by using
 * context_size from the controller at runtime for pointer arithmetic. */

/* Slot Context (Section 6.2.2) — 32 bytes */
typedef struct {
    uint32_t dwords[8];
} __attribute__((packed, aligned(32))) xhci_slot_context_t;

/* Endpoint Context (Section 6.2.3) — 32 bytes */
typedef struct {
    uint32_t dwords[8];
} __attribute__((packed, aligned(32))) xhci_endpoint_context_t;

/* Device Context (Section 6.2.1) — Slot + 31 Endpoints = 1024 bytes (32-byte mode) */
typedef struct {
    xhci_slot_context_t slot;
    xhci_endpoint_context_t endpoints[31];
} __attribute__((packed, aligned(64))) xhci_device_context_t;

/* Device Context Base Address Array — 256 entries (Section 6.1) */
typedef struct {
    uint64_t device_context_ptrs[256];
} __attribute__((packed, aligned(64))) xhci_dcbaa_t;

/* Input Control Context (Section 6.2.5.1) — 32 bytes */
typedef struct {
    uint32_t drop_context_flags;    /* dword 0 */
    uint32_t add_context_flags;     /* dword 1 */
    uint32_t reserved[5];           /* dwords 2-6 */
    uint32_t config_info;           /* dword 7: Configuration Value[7:0], Interface Number[15:8], Alternate Setting[23:16] */
} __attribute__((packed, aligned(32))) xhci_input_control_context_t;

_Static_assert(sizeof(xhci_slot_context_t) == 32, "Slot Context must be 32 bytes");
_Static_assert(sizeof(xhci_endpoint_context_t) == 32, "Endpoint Context must be 32 bytes");
_Static_assert(sizeof(xhci_input_control_context_t) == 32, "Input Control Context must be 32 bytes");
_Static_assert(sizeof(xhci_device_context_t) == 1024, "Device Context must be 1024 bytes (32-byte mode)");

void xhci_init_slot_context(xhci_slot_context_t* slot_ctx, uint8_t port, uint32_t speed);
void xhci_init_ep0_context(xhci_endpoint_context_t* ep0_ctx, uint64_t ring_phys, uint16_t max_packet);

#endif
