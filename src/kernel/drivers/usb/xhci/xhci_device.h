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

/* Fill a Slot Context from what is known about the device: where it sits on
 * the bus, how fast it is, whether it is a hub, and which translator stands
 * between it and the controller. Takes the slot rather than a port number
 * because a device behind a hub is described by five of those things and not
 * by one. */
struct xhci_device_slot;
void xhci_fill_slot_context(xhci_slot_context_t* slot_ctx,
                            const struct xhci_device_slot* slot);
/* `dequeue_with_dcs` is the physical address the controller should pick the
 * control ring up at, with the Dequeue Cycle State in bit 0 — see the note on
 * the definition for why it is not simply the ring's base. */
void xhci_init_ep0_context(xhci_endpoint_context_t* ep0_ctx,
                           uint64_t dequeue_with_dcs, uint16_t max_packet);

/*
 * What the CONTROLLER thinks of a device, as opposed to what this driver
 * thinks.
 *
 * The Output Slot Context is written by the controller and never by software
 * (Section 4.5.3): its Slot State says whether the device is Disabled, in
 * Default, Addressed or Configured, and its USB Device Address says which
 * address the controller actually assigned. That is the one fact that tells an
 * Address Device which never happened apart from an Address Device which
 * happened and whose answer this driver lost — and they are opposite faults
 * that have looked identical in every report from a live board so far.
 *
 * Read at the moment of failure, so the report stands on its own instead of
 * depending on lines that have already scrolled off a screen.
 */
#define XHCI_SLOT_STATE_DISABLED   0
#define XHCI_SLOT_STATE_DEFAULT    1
#define XHCI_SLOT_STATE_ADDRESSED  2
#define XHCI_SLOT_STATE_CONFIGURED 3

uint8_t     xhci_slot_context_state(const struct xhci_device_slot* slot);
uint8_t     xhci_slot_context_address(const struct xhci_device_slot* slot);
const char* xhci_slot_state_name(uint8_t state);

#endif
