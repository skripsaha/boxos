#ifndef XHCI_PORT_H
#define XHCI_PORT_H

#include "ktypes.h"
#include "xhci_regs.h"
#include "xhci.h"

/* Speeds as the controller reports them in PORTSC (Table 6-14). */
#define XHCI_PORT_SPEED_FULL     1
#define XHCI_PORT_SPEED_LOW      2
#define XHCI_PORT_SPEED_HIGH     3
#define XHCI_PORT_SPEED_SUPER    4
#define XHCI_PORT_SPEED_SUPER_10 5

/* USB 2.0 §7.2.4.1: a device may take this long after power is applied
 * before it must answer. */
#define XHCI_PORT_POWER_SETTLE_MS 100

/* The outside edge of the boot survey, reached only by a port that never
 * settles. The survey itself ends as soon as two passes running find nothing
 * new, so a machine with nothing plugged in pays the debounce above and no
 * more. This exists so that a port wedged in a link state it cannot leave
 * costs a bounded amount of boot rather than all of it. */
#define XHCI_PORT_SURVEY_MS 1200

/* TRSTRCY — what a device is owed between the end of its port reset and the
 * first thing the host says to it (USB 2.0 §7.1.7.5). Ten milliseconds, spent
 * by the device coming up in the Default state. */
#define XHCI_PORT_RESET_RECOVERY_MS 10

uint32_t xhci_get_port_status(xhci_controller_t* ctrl, uint8_t port);
bool     xhci_port_has_device(xhci_controller_t* ctrl, uint8_t port);
uint8_t  xhci_get_port_speed(xhci_controller_t* ctrl, uint8_t port);
void     xhci_port_clear_change_bits(xhci_controller_t* ctrl, uint8_t port, uint32_t bits);

/* USB major revision of a root port, from the Supported Protocol capability.
 * Zero when the controller never described the port. */
uint8_t  xhci_port_protocol(xhci_controller_t* ctrl, uint8_t port);

/* Switch on every root port that is not powered, then wait out the debounce.
 * Called once, after the controller is running. */
void     xhci_power_ports(xhci_controller_t* ctrl);

/* Say what one root port currently reports, in full. The machine this has to
 * work on has no debugger and no serial cable — it has a screen. */
void     xhci_port_describe(xhci_controller_t* ctrl, uint8_t port);

/* Start a port reset and return without waiting. Returns 1 when the port is
 * already usable and no reset was needed, 0 when a reset is now in flight and
 * a port-status change will follow, negative on error. */
/*
 * What a port had done to it before the device on it was spoken to.
 *
 * Kept and printed because a device answers the default address only while it
 * is in the Default state, and it enters the Default state only through a
 * reset (USB 2.0 Section 9.1.1). So "was this port reset, and how" is the
 * first question a silent Address Device has to be checked against — and a
 * machine that boots from a USB stick reaches this driver with firmware
 * having already addressed at least two devices on the bus.
 */
#define XHCI_PORT_RESET_NONE 0      /* arrived enabled and was left alone */
#define XHCI_PORT_RESET_HOT  1      /* PORTSC.PR */
#define XHCI_PORT_RESET_WARM 2      /* PORTSC.WPR, USB 3 only */
#define XHCI_PORT_RESET_HUB  3      /* a hub reset it, not this driver */

const char* xhci_port_reset_kind_name(uint8_t kind);

/* Begin a port reset. `out_kind` receives which kind was applied, or
 * XHCI_PORT_RESET_NONE for a port left alone; may be NULL. */
int      xhci_port_begin_reset(xhci_controller_t* ctrl, uint8_t port,
                               uint8_t* out_kind);

/* True when the port that just reported a reset came out of it usable. */
bool     xhci_port_reset_finished(xhci_controller_t* ctrl, uint8_t port);

int      xhci_disable_port(xhci_controller_t* ctrl, uint8_t port);

#endif
