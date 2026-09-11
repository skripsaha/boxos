#include "xhci_port.h"
#include "xhci.h"
#include "xhci_regs.h"
#include "klib.h"
#include "atomics.h"
#include "cpu_calibrate.h"

static uint32_t portsc_read(xhci_controller_t* ctrl, uint8_t port)
{
    return ctrl->ports[port - 1].portsc;
}

static uint32_t portsc_base(xhci_controller_t* ctrl, uint8_t port)
{
    return portsc_read(ctrl, port) & ~XHCI_PORTSC_RMW_CLEAR;
}

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

uint8_t xhci_port_other_half(xhci_controller_t* ctrl, uint8_t port)
{
    if (!ctrl || port == 0 || port >= XHCI_PORT_MAP_ENTRIES) {
        return 0;
    }
    return ctrl->port_pair[port];
}

bool xhci_port_socket_is_empty(xhci_controller_t* ctrl, uint8_t port)
{
    if (!port_valid(ctrl, port)) {
        return false;
    }
    if (portsc_read(ctrl, port) & XHCI_PORTSC_CCS) {
        return false;
    }

    uint8_t other = xhci_port_other_half(ctrl, port);
    if (other == 0 || !port_valid(ctrl, other)) {
        return true;
    }
    return (portsc_read(ctrl, other) & XHCI_PORTSC_CCS) == 0;
}

void xhci_power_ports(xhci_controller_t* ctrl)
{
    if (!ctrl || !ctrl->ports || !ctrl->cap_regs) {
        return;
    }

    if ((ctrl->cap_regs->hccparams1 & XHCI_HCC1_PPC) == 0) {
        kprintf("[xHCI %s] powers its own ports\n", ctrl->name);
        return;
    }

    unsigned switched = 0;
    for (unsigned pn = 1; pn <= ctrl->max_ports; pn++) {
        uint8_t port = (uint8_t)pn;
        if (portsc_read(ctrl, port) & XHCI_PORTSC_PP) {
            continue;
        }
        portsc_write(ctrl, port, portsc_base(ctrl, port) | XHCI_PORTSC_PP);
        switched++;
    }

    kprintf("[xHCI %s] powered %u root port(s) of %u\n",
            ctrl->name, switched, ctrl->max_ports);

    if (switched == 0) {
        return;
    }

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(XHCI_PORT_POWER_SETTLE_MS);
    while ((int64_t)(rdtsc() - deadline) < 0) {
        cpu_pause();
    }
}

void xhci_port_describe(xhci_controller_t* ctrl, uint8_t port)
{
    if (!ctrl || !ctrl->ports) return;

    uint32_t sc = xhci_get_port_status(ctrl, port);
    uint8_t  pls = (uint8_t)XHCI_PORTSC_PLS(sc);

    static const char* link[16] = {
        "U0", "U1", "U2", "U3", "Disabled", "RxDetect", "Inactive", "Polling",
        "Recovery", "Hot Reset", "Compliance", "Test", "?", "?", "?", "Resume"
    };

    uint8_t other = xhci_port_other_half(ctrl, port);
    char socket[32];
    socket[0] = '\0';
    if (other) {
        ksnprintf(socket, sizeof(socket), ", same socket as port %u", other);
    }

    kprintf("[xHCI %s] port %u: %s, %s, link %s, speed %u  (PORTSC 0x%x, USB %u%s)\n",
            ctrl->name, port,
            (sc & XHCI_PORTSC_PP)  ? "powered"   : "NOT powered",
            (sc & XHCI_PORTSC_CCS) ? "something attached" : "nothing attached",
            link[pls],
            (unsigned)XHCI_PORTSC_SPEED(sc),
            sc, ctrl->port_major[port], socket);
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
        return -2;
    }

    uint8_t major = xhci_port_protocol(ctrl, port);
    bool    warm  = (major >= 3) &&
                    (port_link_needs_warm(portsc) ||
                     !(portsc & XHCI_PORTSC_PED));

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

bool xhci_port_reset_finished(xhci_controller_t* ctrl, uint8_t port)
{
    if (!port_valid(ctrl, port)) {
        return false;
    }

    uint32_t portsc = portsc_read(ctrl, port);
    return (portsc & XHCI_PORTSC_CCS) && (portsc & XHCI_PORTSC_PED);
}

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

    portsc_write(ctrl, port, portsc_base(ctrl, port) | XHCI_PORTSC_PED);

    debug_printf("[xHCI Port] Port %u disabled\n", port);
    return 0;
}