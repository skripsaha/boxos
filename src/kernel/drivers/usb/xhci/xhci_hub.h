#ifndef XHCI_HUB_H
#define XHCI_HUB_H

#include "ktypes.h"
#include "xhci.h"

/*
 * USB hubs.
 *
 * Without them the only devices this kernel can see are the ones plugged
 * straight into the machine — and a keyboard plugged into a monitor, a dock or
 * a hub on the desk is plugged into none of them. It is not an exotic case; it
 * is where most keyboards actually are.
 *
 * A hub is an ordinary USB device that happens to have ports. Software powers
 * them, asks what is on each, resets the ones with something attached, and
 * hands what it finds to enumeration — with the path down to it, because the
 * controller reaches a device below a hub by being told which port was taken
 * at each tier.
 *
 * Everything here needs control transfers, so nothing here may run from the
 * interrupt handler. The hub's own status-change endpoint does report from
 * there, and all it does is raise a flag; the work happens in xhci_hub_service,
 * which is called from contexts that are allowed to wait.
 */

/* Claim a hub the enumerator has configured. Powers its ports, waits out the
 * power-on delay, and enumerates whatever is already attached. */
int  xhci_hub_attach(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

/* Let go of a hub that has been unplugged, along with everything below it. */
void xhci_hub_release(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

/* True when this hub has already been claimed. */
bool xhci_hub_slot_attached(xhci_device_slot_t* slot);

/* Attach any hub that is waiting for it, and act on any port change a hub has
 * reported. Must be called from a context that can wait — never from an
 * interrupt handler. Returns the number of things it did, so a caller wanting
 * the bus to settle can keep calling until it returns zero. */
int  xhci_hub_service(xhci_controller_t* ctrl);

/* Cheap enough to call from the idle loop: returns immediately when no hub has
 * anything outstanding. */
bool xhci_hub_work_pending(void);
void xhci_hub_service_if_pending(void);

/* A hub's status-change endpoint has reported. Called from the interrupt
 * handler, and does nothing but remember that it happened. */
void xhci_hub_note_change(xhci_device_slot_t* slot);

/* A hub has been bound but not yet spoken to. Raised from the event handler
 * for the same reason: attaching one is made of control transfers. */
void xhci_hub_note_work(void);

#endif
