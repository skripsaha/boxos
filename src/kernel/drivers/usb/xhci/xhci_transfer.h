#ifndef XHCI_TRANSFER_H
#define XHCI_TRANSFER_H

#include "ktypes.h"
#include "xhci_trb.h"
#include "xhci_rings.h"
#include "usb_descriptors.h"

// Types are defined in xhci.h - include xhci.h before this header

/* The per-slot scratch page every descriptor is read into. Named here because
 * this is where it is allocated, and the enumeration state machine has to
 * refuse a configuration descriptor larger than it rather than overrun it. */
#define XHCI_DESC_BUFFER_BYTES 4096

int xhci_alloc_ep0_ring(xhci_controller_t* ctrl, xhci_device_slot_t* slot);
void xhci_free_ep0_ring(xhci_device_slot_t* slot);

/* Post a control transfer and wait for it. For class drivers, which run as
 * ordinary kernel code with a question to ask; enumeration uses the
 * asynchronous form above and is carried forward by the completion itself. */
int xhci_control_transfer_sync(xhci_controller_t* ctrl, xhci_device_slot_t* slot,
                               usb_setup_packet_t* setup, uint64_t data_phys,
                               uint16_t data_len, bool data_in,
                               uint32_t timeout_ms);

int xhci_control_transfer(xhci_controller_t* ctrl,
                          xhci_device_slot_t* slot,
                          usb_setup_packet_t* setup,
                          uint64_t data_buffer_phys,
                          uint16_t data_length,
                          bool data_in);

/*
 * The one deliberate exercise of the give-up path, which no emulated device
 * ever reaches on its own. Built only by `make CTRLGIVEUP=on`; off, it is not
 * a call at all — the same arrangement HardwareDeckUsbRecoverProof has.
 */
#define XHCI_CTRL_GIVEUP_PROOF_MS 200
#if CONFIG_XHCI_CTRL_GIVEUP_PROOF
void xhci_ctrl_giveup_proof(xhci_controller_t* ctrl, xhci_device_slot_t* slot);
#else
static inline void xhci_ctrl_giveup_proof(xhci_controller_t* ctrl,
                                          xhci_device_slot_t* slot)
{ (void)ctrl; (void)slot; }
#endif

void xhci_handle_transfer_event(xhci_controller_t* ctrl, xhci_trb_t* event);

#endif
