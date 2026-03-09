#ifndef XHCI_TRANSFER_H
#define XHCI_TRANSFER_H

#include "xhci.h"
#include "xhci_rings.h"
#include "usb_descriptors.h"

int xhci_alloc_ep0_ring(xhci_controller_t* ctrl, xhci_device_slot_t* slot);
void xhci_free_ep0_ring(xhci_device_slot_t* slot);

int xhci_control_transfer(xhci_controller_t* ctrl,
                          xhci_device_slot_t* slot,
                          usb_setup_packet_t* setup,
                          uint64_t data_buffer_phys,
                          uint16_t data_length,
                          bool data_in);

int xhci_queue_interrupt_transfer(xhci_device_slot_t* slot);
void xhci_handle_transfer_event(xhci_controller_t* ctrl, xhci_trb_t* event);

#endif
