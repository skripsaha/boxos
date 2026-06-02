#ifndef XHCI_INTERRUPT_H
#define XHCI_INTERRUPT_H

#include "ktypes.h"

void xhci_irq_handler(void);
void xhci_process_events(void);
void xhci_poll_events(void);

/* Resolve and cache Touch tag handles for "usb:connect" / "usb:disconnect".
 * Must be called from non-IRQ context AFTER TouchInit (TagFS registry up)
 * and BEFORE the xHCI controller starts generating port-change interrupts.
 * Re-callable; later calls overwrite cached handles atomically. */
void xhci_interrupt_touch_init(void);

#endif
