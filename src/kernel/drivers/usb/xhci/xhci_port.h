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

/* The other root port of the same physical socket, or zero when this port has
 * no other half. See ctrl->port_pair for what that answer is made of. */
uint8_t  xhci_port_other_half(xhci_controller_t* ctrl, uint8_t port);

/*
 * Is the SOCKET this port belongs to empty — as opposed to this port being.
 *
 * The one question a single port cannot answer. A USB 3 socket is two root
 * ports, and a device whose SuperSpeed link does not train vanishes from one
 * of them and appears on the other; on the port it left, that is bit-for-bit
 * what a hand pulling it out looks like. Asking both halves separates them,
 * and separates them with two register reads rather than a clock.
 *
 * A port with no other half answers for itself alone, which is correct: a
 * socket with one port IS that port.
 */
bool     xhci_port_socket_is_empty(xhci_controller_t* ctrl, uint8_t port);

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

/* Begin a port reset — every port, at every speed, because that is the only
 * thing that puts the device on it into the Default state where an Address
 * Device can reach it. `out_kind` receives which kind was applied; may be NULL.
 * Returns 0 when a reset is in flight, negative on error. */
int      xhci_port_begin_reset(xhci_controller_t* ctrl, uint8_t port,
                               uint8_t* out_kind);

/* Escalate to the out-of-band reset after a hot one left the port disabled. */
int      xhci_port_warm_reset(xhci_controller_t* ctrl, uint8_t port);

/* True when the port that just reported a reset came out of it usable. */
bool     xhci_port_reset_finished(xhci_controller_t* ctrl, uint8_t port);

/*
 * Has the device on this port gone, as the port itself reports it?
 *
 * The one question that replaces a deadline with a fact. Whoever is waiting
 * for an answer that has not come can ask this instead of counting: a port
 * with nothing on it is not a slow device, and the silence is not the
 * controller's fault. True in one direction only — see the comment on the
 * definition, and never read a false as "the device is fine".
 */
bool     xhci_port_says_gone(xhci_controller_t* ctrl, uint8_t port);

int      xhci_disable_port(xhci_controller_t* ctrl, uint8_t port);

#endif
