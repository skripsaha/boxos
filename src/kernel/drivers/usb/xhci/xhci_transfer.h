#ifndef XHCI_TRANSFER_H
#define XHCI_TRANSFER_H

#include "xhci.h"
#include "xhci_rings.h"
#include "usb_descriptors.h"

int xhci_alloc_ep0_ring(xhci_controller_t* ctrl, xhci_device_slot_t* slot);
void xhci_free_ep0_ring(xhci_device_slot_t* slot);

int xhci_post_evaluate_context_cmd(xhci_controller_t* ctrl, uint8_t slot_id);

int xhci_control_transfer(xhci_controller_t* ctrl,
                          xhci_device_slot_t* slot,
                          usb_setup_packet_t* setup,
                          uint64_t data_buffer_phys,
                          uint16_t data_length,
                          bool data_in);

int xhci_queue_interrupt_transfer(xhci_device_slot_t* slot);

void xhci_handle_transfer_event(xhci_controller_t* ctrl, xhci_trb_t* event);

uint64_t xhci_pack_setup_packet(usb_setup_packet_t* setup);
void xhci_enqueue_transfer_trb(xhci_ring_t* ring, xhci_trb_t* trb);

#endif
