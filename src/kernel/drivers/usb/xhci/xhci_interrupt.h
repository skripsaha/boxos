#ifndef XHCI_INTERRUPT_H
#define XHCI_INTERRUPT_H

#include "ktypes.h"
#include "xhci.h"

void xhci_irq_handler(void);

void xhci_irq_handler_vector(uint8_t vector);
void xhci_process_events(void);
void xhci_poll_events(void);

void xhci_tick(void);

void xhci_touch_device_arrived(const xhci_device_slot_t* slot);
void xhci_touch_device_left(const xhci_device_slot_t* slot);

void xhci_interrupt_touch_init(void);


#define XHCI_ERDP_BATCH 32

bool xhci_drain_is_mine(const xhci_controller_t* ctrl);

#endif