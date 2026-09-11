#ifndef XHCI_TRANSFER_H
#define XHCI_TRANSFER_H

#include "ktypes.h"
#include "xhci_trb.h"
#include "xhci_rings.h"
#include "usb_descriptors.h"


#define XHCI_DESC_BUFFER_BYTES 4096

int xhci_alloc_ep0_ring(xhci_controller_t* ctrl, xhci_device_slot_t* slot);
void xhci_free_ep0_ring(xhci_device_slot_t* slot);

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