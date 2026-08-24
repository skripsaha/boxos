#ifndef XHCI_INTERRUPT_H
#define XHCI_INTERRUPT_H

#include "ktypes.h"
#include "xhci.h"

void xhci_irq_handler(void);
void xhci_process_events(void);
void xhci_poll_events(void);

/* Once per timer tick: poll for events when there is no interrupt, and let the
 * command and enumeration watchdogs report anything that stopped answering. */
void xhci_tick(void);

/* Announce a device, by name, at the moment the kernel first knows what it is.
 *
 * These are NOT the same events as "usb:connect" / "usb:disconnect". Those are
 * about a socket: something changed at port 5, and at that instant nobody knows
 * what. These are about a device: addressed, configured, and carrying its
 * vendor, product and class. A subscriber that wants to know what arrived had
 * to go and ask before — which is the polling Touch exists to abolish — and now
 * the answer comes with the event.
 *
 * "usb:left" fires only for a device that had arrived. A device unplugged
 * halfway through enumeration never had a name to announce, and a departure
 * with no arrival is a story with a hole in it. */
void xhci_touch_device_arrived(const xhci_device_slot_t* slot);
void xhci_touch_device_left(const xhci_device_slot_t* slot);

/* Resolve and cache Touch tag handles for "usb:connect" / "usb:disconnect".
 * Must be called from non-IRQ context AFTER TouchInit (TagFS registry up)
 * and BEFORE the xHCI controller starts generating port-change interrupts.
 * Re-callable; later calls overwrite cached handles atomically. */
void xhci_interrupt_touch_init(void);

#endif
