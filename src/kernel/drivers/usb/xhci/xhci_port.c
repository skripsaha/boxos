#include "xhci_port.h"
#include "xhci.h"
#include "xhci_regs.h"
#include "klib.h"
#include "atomics.h"
#include "cpu_calibrate.h"

/*
 * PORTSC is a register where writing back what you read is destructive.
 *
 * Seven of its bits are write-one-to-clear changes: a read-modify-write that
 * does not mask them off first clears whichever happened to be set at the
 * moment of the read, silently discarding a connect, a reset completion or an
 * over-current report that nothing will ever mention again. Three more are
 * write-one-to-act: PED disables the port, PR and WPR start a reset, LWS
 * commits a link state. Writing those back unchanged does not preserve them —
 * it performs them.
 *
 * Every write in this file goes through portsc_write for that reason, and no
 * other function in the driver writes PORTSC at all.
 */
static uint32_t portsc_read(xhci_controller_t* ctrl, uint8_t port)
{
    return ctrl->ports[port - 1].portsc;
}

/* The part of the current PORTSC that is safe to write back: everything the
 * driver wants to preserve, with every write-one-to-act and write-one-to-clear
 * bit stripped out. Whatever the caller actually intends to do is then OR'd on
 * top, explicitly, one bit at a time. */
static uint32_t portsc_base(xhci_controller_t* ctrl, uint8_t port)
{
    return portsc_read(ctrl, port) & ~XHCI_PORTSC_RMW_CLEAR;
}

/* Write PORTSC. The value must already be a portsc_base with the intended
 * action bits OR'd in — this does not filter, because filtering here is what
 * removed the reset bit from the write whose entire purpose was to set it. */
static void portsc_write(xhci_controller_t* ctrl, uint8_t port, uint32_t value)
{
    ctrl->ports[port - 1].portsc = value;
}

static bool port_valid(xhci_controller_t* ctrl, uint8_t port)
{
    return ctrl && ctrl->initialized && ctrl->ports &&
           port != 0 && port <= ctrl->max_ports;
}

uint32_t xhci_get_port_status(xhci_controller_t* ctrl, uint8_t port) {
    if (!port_valid(ctrl, port)) {
        return 0;
    }
    return portsc_read(ctrl, port);
}

bool xhci_port_has_device(xhci_controller_t* ctrl, uint8_t port) {
    if (!port_valid(ctrl, port)) {
        return false;
    }
    return (portsc_read(ctrl, port) & XHCI_PORTSC_CCS) != 0;
}

uint8_t xhci_get_port_speed(xhci_controller_t* ctrl, uint8_t port) {
    if (!port_valid(ctrl, port)) {
        return 0;
    }
    return (uint8_t)XHCI_PORTSC_SPEED(portsc_read(ctrl, port));
}

void xhci_port_clear_change_bits(xhci_controller_t* ctrl, uint8_t port, uint32_t bits) {
    if (!port_valid(ctrl, port)) {
        return;
    }

    portsc_write(ctrl, port, portsc_base(ctrl, port) |
                             (bits & XHCI_PORTSC_W1C_MASK));
}

uint8_t xhci_port_protocol(xhci_controller_t* ctrl, uint8_t port)
{
    if (!ctrl || port == 0 || port >= XHCI_PORT_MAP_ENTRIES) {
        return 0;
    }
    return ctrl->port_major[port];
}

/*
 * Port power.
 *
 * When the controller reports Port Power Control in HCCPARAMS1, its root ports
 * come out of reset with the power off and stay that way until software says
 * otherwise. An unpowered port reports no device however much hardware is
 * plugged into it — so on such a controller, skipping this is not a
 * degradation, it is a machine with no USB at all. QEMU powers its ports
 * itself, which is the whole reason a driver that never wrote this bit looked
 * like it worked.
 *
 * USB 2.0 §7.2.4.1 gives a device up to 100 ms after power is applied before
 * it has to be ready to answer; the wait below is that debounce, paid once for
 * all ports rather than once per port.
 */
void xhci_power_ports(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->ports || !ctrl->cap_regs) {
        return;
    }

    if ((ctrl->cap_regs->hccparams1 & XHCI_HCC1_PPC) == 0) {
        /* Said out loud, not whispered into a debug build. On the machine
         * where this matters there is no debug build — there is a screen, and
         * a person reading it. Every fact this function establishes changes
         * what the next line of the boot means. */
        kprintf("[xHCI %s] powers its own ports\n", ctrl->name);
        return;
    }

    unsigned switched = 0;
    for (uint8_t port = 1; port <= ctrl->max_ports; port++) {
        if (portsc_read(ctrl, port) & XHCI_PORTSC_PP) {
            continue;                   /* already powered */
        }
        portsc_write(ctrl, port, portsc_base(ctrl, port) | XHCI_PORTSC_PP);
        switched++;
    }

    kprintf("[xHCI %s] powered %u root port(s) of %u\n",
            ctrl->name, switched, ctrl->max_ports);

    if (switched == 0) {
        return;                         /* firmware had already powered them */
    }

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(XHCI_PORT_POWER_SETTLE_MS);
    while ((int64_t)(rdtsc() - deadline) < 0) {
        cpu_pause();
    }
}

/* What a port has to say about itself, in the terms the register uses. */
void xhci_port_describe(xhci_controller_t* ctrl, uint8_t port)
{
    if (!ctrl || !ctrl->ports) return;

    uint32_t sc = xhci_get_port_status(ctrl, port);
    uint8_t  pls = (uint8_t)XHCI_PORTSC_PLS(sc);

    static const char* link[16] = {
        "U0", "U1", "U2", "U3", "Disabled", "RxDetect", "Inactive", "Polling",
        "Recovery", "Hot Reset", "Compliance", "Test", "?", "?", "?", "Resume"
    };

    kprintf("[xHCI %s] port %u: %s, %s, link %s, speed %u  (PORTSC 0x%x, USB %u)\n",
            ctrl->name, port,
            (sc & XHCI_PORTSC_PP)  ? "powered"   : "NOT powered",
            (sc & XHCI_PORTSC_CCS) ? "something attached" : "nothing attached",
            link[pls],
            (unsigned)XHCI_PORTSC_SPEED(sc),
            sc, ctrl->port_major[port]);
}

const char* xhci_port_reset_kind_name(uint8_t kind)
{
    switch (kind) {
        case XHCI_PORT_RESET_HOT:  return "hot reset";
        case XHCI_PORT_RESET_WARM: return "warm reset";
        case XHCI_PORT_RESET_HUB:  return "reset by its hub";
        default:                   return "NOT reset";
    }
}

/*
 * A link that cannot be recovered by asking politely.
 *
 * SS.Inactive is a SuperSpeed link that failed and cannot retrain itself, and
 * Compliance is a port that fell into the electrical test mode a partly
 * connected cable puts it in. Neither answers a hot reset, because a hot reset
 * is carried IN BAND over a link that is not working. A warm reset is signalled
 * out of band and is the only one that gets those ports back — which is exactly
 * the escalation every USB stack does, and why PORTSC has two reset bits.
 */
static bool port_link_needs_warm(uint32_t portsc)
{
    switch (XHCI_PORTSC_PLS(portsc)) {
        case XHCI_PLS_INACTIVE:
        case XHCI_PLS_COMPLIANCE:
        case XHCI_PLS_DISABLED:
            return true;
        default:
            return false;
    }
}

/*
 * Beginning a port reset, and then leaving.
 *
 * A port has to be driven through a reset before the device on it will answer
 * to anything. That reset takes tens of milliseconds, and the old code spent
 * them spinning — inside the interrupt handler, once per port, with every other
 * interrupt on the core waiting behind it. Nothing about the hardware requires
 * that: the controller raises a port-status change when the reset finishes and
 * sets PRC or WRC to say which one it was. So this asserts the reset and
 * returns, and xhci_port_reset_finished picks the story up when the hardware
 * says it is time.
 *
 * ‼ EVERY PORT IS RESET, INCLUDING A USB 3 PORT THAT ARRIVES ALREADY ENABLED.
 *
 * This driver used to leave those alone, reasoning that a SuperSpeed link
 * trains itself on connect and that resetting it drops a link that was working.
 * Both halves of that are true and the conclusion does not follow, because a
 * trained link says nothing whatever about the state of the DEVICE on it:
 *
 *   Address Device sends a USB SET_ADDRESS to the DEFAULT ADDRESS (xHCI 1.2
 *   Section 4.6.5), and a device answers the default address only while it is
 *   in the Default state, which it enters only through a reset (USB 2.0
 *   Section 9.1.1.3). A Host Controller Reset does not put it there — HCRST
 *   resets the controller, not the devices, and a SuperSpeed link that comes
 *   back up keeps the address it had.
 *
 * BoxOS boots from a flash drive through the firmware, so it reaches this
 * driver with the firmware having already addressed at least the boot device
 * and the keyboard. Sending SET_ADDRESS to address zero at a device that is
 * already addressed is sending it to an address nobody on the bus answers on:
 * the command goes out and is never completed, and because the command ring
 * executes strictly in order, everything queued behind it stops too. Nothing in
 * an emulator reproduces it — QEMU's devices come up unaddressed every time.
 *
 * Which reset depends on the link, not on the speed: a hot reset is signalled
 * in band and needs a link that works, so a port whose link has failed gets the
 * warm one. A USB 2 port has only the hot one and needs no choice.
 *
 * Returns 0 when a reset was started and its event will follow, negative on
 * error. There is no longer a "nothing to do" answer.
 */
int xhci_port_begin_reset(xhci_controller_t* ctrl, uint8_t port,
                          uint8_t* out_kind)
{
    if (out_kind) {
        *out_kind = XHCI_PORT_RESET_NONE;
    }
    if (!port_valid(ctrl, port)) {
        return -1;
    }

    uint32_t portsc = portsc_read(ctrl, port);
    if (!(portsc & XHCI_PORTSC_CCS)) {
        return -2;                      /* nothing connected any more */
    }

    uint8_t major = xhci_port_protocol(ctrl, port);
    bool    warm  = (major >= 3) &&
                    (port_link_needs_warm(portsc) ||
                     !(portsc & XHCI_PORTSC_PED));

    /*
     * Said out loud, once per device, not into a debug build. On a machine
     * whose only diagnostic is a photograph of the screen, this line is what an
     * Address Device that was never answered has to be read against.
     */
    kprintf("[xHCI %s] port %u (USB %u): %s (PORTSC 0x%08x, link %u)\n",
            ctrl->name, port, major,
            warm ? "warm reset" : "hot reset",
            portsc, XHCI_PORTSC_PLS(portsc));

    if (out_kind) {
        *out_kind = warm ? XHCI_PORT_RESET_WARM : XHCI_PORT_RESET_HOT;
    }
    portsc_write(ctrl, port, portsc_base(ctrl, port) |
                             (warm ? XHCI_PORTSC_WPR : XHCI_PORTSC_PR));
    return 0;
}

/*
 * The escalation: a hot reset that ended with the port still not enabled.
 *
 * On a SuperSpeed port that means the link did not come back, and the only
 * thing left is the out-of-band reset. Doing it here rather than giving the
 * device up is what turns "a stick that needs its socket wiggled" into a stick
 * that comes up on the second try.
 */
int xhci_port_warm_reset(xhci_controller_t* ctrl, uint8_t port)
{
    if (!port_valid(ctrl, port)) {
        return -1;
    }

    uint32_t portsc = portsc_read(ctrl, port);
    if (!(portsc & XHCI_PORTSC_CCS)) {
        return -2;
    }

    kprintf("[xHCI %s] port %u: the hot reset left it disabled — warm reset "
            "(PORTSC 0x%08x, link %u)\n",
            ctrl->name, port, portsc, XHCI_PORTSC_PLS(portsc));

    portsc_write(ctrl, port, portsc_base(ctrl, port) | XHCI_PORTSC_WPR);
    return 0;
}

/*
 * The reset the hardware just told us about: did it leave a usable port?
 *
 * PRC only says the reset ended. Whether it succeeded is PED — a port that
 * reset and did not enable has a device that failed to answer, and enumerating
 * into that produces a slot the controller will refuse to address.
 */
bool xhci_port_reset_finished(xhci_controller_t* ctrl, uint8_t port)
{
    if (!port_valid(ctrl, port)) {
        return false;
    }

    uint32_t portsc = portsc_read(ctrl, port);
    return (portsc & XHCI_PORTSC_CCS) && (portsc & XHCI_PORTSC_PED);
}

/*
 * The port's own answer to "is there any point waiting for this device?"
 *
 * Asked instead of a clock, and trusted in ONE direction only. CCS clear is
 * the controller saying nothing is attached — a device that is not there will
 * not answer an Address Device this year or next, and that is a fact rather
 * than an expired budget. The other direction is deliberately not offered:
 * a device reached through a hub carries the ROOT port in slot->port_num
 * (xhci_enumeration.c, xhci_enumerate_behind_hub), so a healthy answer here
 * says the branch is alive and says NOTHING about the device on the end of it.
 *
 * PED is left out on purpose. It is zero for the whole of a port reset, which
 * is a device coming up rather than a device gone, and a predicate that cannot
 * tell those apart is one that retires devices for being born.
 *
 * A port this controller does not have is not an answer, so it is not "gone".
 */
bool xhci_port_says_gone(xhci_controller_t* ctrl, uint8_t port)
{
    if (!port_valid(ctrl, port)) {
        return false;
    }
    return (portsc_read(ctrl, port) & XHCI_PORTSC_CCS) == 0;
}

int xhci_disable_port(xhci_controller_t* ctrl, uint8_t port) {
    if (!port_valid(ctrl, port)) {
        return -1;
    }

    /* PED is write-one-to-DISABLE, not a normal control bit: writing zero to
     * it does nothing at all. The old code cleared it in a read-modify-write
     * and reported success, which meant this function had never once disabled
     * a port. */
    portsc_write(ctrl, port, portsc_base(ctrl, port) | XHCI_PORTSC_PED);

    debug_printf("[xHCI Port] Port %u disabled\n", port);
    return 0;
}
