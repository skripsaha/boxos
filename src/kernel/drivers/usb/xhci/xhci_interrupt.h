#ifndef XHCI_INTERRUPT_H
#define XHCI_INTERRUPT_H

#include "ktypes.h"
#include "xhci.h"

void xhci_irq_handler(void);

/* The controller that signals on this vector. Each has one of its own. */
void xhci_irq_handler_vector(uint8_t vector);
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

/*
 * Hold the screen so the lines just printed can be read — or photographed.
 *
 * This driver is brought up on a machine whose only diagnostic is a monitor
 * and a phone camera, and the lines that decide what to do next go past faster
 * than a shutter. So the few places where the next line changes what happens
 * next stop for a moment afterwards.
 *
 * It does nothing when the caller is an interrupt handler or the timer tick.
 * That is the whole reason this exists rather than a bare kscreen_hold at each
 * site: a second spent not returning from IRQ0 is a second of the machine's
 * timekeeping thrown away, and the enumeration watchdog is reached from both
 * there and from ordinary context.
 */
#define XHCI_SCREEN_HOLD_MS 1000

/*
 * How many events are handled between publications of the dequeue pointer.
 *
 * The controller decides the ring is full by comparing its own enqueue
 * position against the pointer software has published (xHCI 1.2 Section 4.9.4),
 * so publishing only at the end of a drain means a burst longer than the ring
 * meets a controller that still believes nothing has been read. Small enough
 * that the ring never runs down, large enough that an ordinary drain of two or
 * three events costs no extra register write at all.
 */
#define XHCI_ERDP_BATCH 32

/*
 * True when THIS core is inside the event drain for this controller.
 *
 * Anything about to wait for an event has to ask. The drain that would deliver
 * that event is below the caller on the same stack and cannot run again until
 * the caller returns, so the wait can only ever end in its own timeout —
 * five seconds of a core spinning for an answer it has itself blocked. Asking
 * turns that into a line naming the caller.
 */
bool xhci_drain_is_mine(const xhci_controller_t* ctrl);
void xhci_hold_screen(void);

#endif
