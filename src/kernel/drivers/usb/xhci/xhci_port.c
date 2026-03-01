#include "xhci_port.h"
#include "xhci.h"
#include "xhci_regs.h"
#include "klib.h"
#include "atomics.h"
#include "cpu_calibrate.h"

uint32_t xhci_get_port_status(xhci_controller_t* ctrl, uint8_t port) {
    if (!ctrl || !ctrl->initialized || port == 0 || port > ctrl->max_ports) {
        return 0;
    }

    xhci_port_regs_t* port_regs = &ctrl->ports[port - 1];
    return port_regs->portsc;
}

bool xhci_port_has_device(xhci_controller_t* ctrl, uint8_t port) {
    if (!ctrl || !ctrl->initialized || port == 0 || port > ctrl->max_ports) {
        return false;
    }

    uint32_t portsc = xhci_get_port_status(ctrl, port);
    return (portsc & XHCI_PORTSC_CCS) != 0;
}

uint8_t xhci_get_port_speed(xhci_controller_t* ctrl, uint8_t port) {
    if (!ctrl || !ctrl->initialized || port == 0 || port > ctrl->max_ports) {
        return 0;
    }

    uint32_t portsc = xhci_get_port_status(ctrl, port);
    return (portsc & XHCI_PORTSC_SPEED_MASK) >> 10;
}

void xhci_port_clear_change_bits(xhci_controller_t* ctrl, uint8_t port, uint32_t bits) {
    if (!ctrl || !ctrl->initialized || port == 0 || port > ctrl->max_ports) {
        return;
    }

    xhci_port_regs_t* port_regs = &ctrl->ports[port - 1];

    uint32_t portsc = port_regs->portsc & ~XHCI_PORTSC_W1C_MASK;
    portsc |= (bits & XHCI_PORTSC_W1C_MASK);
    port_regs->portsc = portsc;
}

int xhci_reset_port(xhci_controller_t* ctrl, uint8_t port) {
    if (!ctrl || !ctrl->initialized || port == 0 || port > ctrl->max_ports) {
        return -1;
    }

    xhci_port_regs_t* port_regs = &ctrl->ports[port - 1];

    debug_printf("[xHCI Port] Resetting port %u...\n", port);

    uint32_t portsc = port_regs->portsc & ~XHCI_PORTSC_W1C_MASK;
    portsc |= XHCI_PORTSC_PR;
    port_regs->portsc = portsc;

    // Poll for reset completion using TSC (non-blocking spin, ~100ms timeout)
    uint64_t timeout_tsc = rdtsc() + cpu_ms_to_tsc(100);
    while (rdtsc() < timeout_tsc) {
        portsc = port_regs->portsc;
        if (!(portsc & XHCI_PORTSC_PR)) {
            break;
        }
        cpu_pause();
    }

    portsc = port_regs->portsc;
    if (portsc & XHCI_PORTSC_PR) {
        debug_printf("[xHCI Port] WARNING: Port %u reset timed out after 100ms\n", port);
        return -1;
    }

    if (portsc & XHCI_PORTSC_PRC) {
        xhci_port_clear_change_bits(ctrl, port, XHCI_PORTSC_PRC);
    }

    debug_printf("[xHCI Port] Port %u reset complete\n", port);
    return 0;
}

int xhci_enable_port(xhci_controller_t* ctrl, uint8_t port) {
    if (!ctrl || !ctrl->initialized || port == 0 || port > ctrl->max_ports) {
        return -1;
    }

    xhci_port_regs_t* port_regs = &ctrl->ports[port - 1];

    uint32_t portsc = port_regs->portsc & ~XHCI_PORTSC_W1C_MASK;
    portsc |= XHCI_PORTSC_PED;
    port_regs->portsc = portsc;

    debug_printf("[xHCI Port] Port %u enabled\n", port);
    return 0;
}

int xhci_disable_port(xhci_controller_t* ctrl, uint8_t port) {
    if (!ctrl || !ctrl->initialized || port == 0 || port > ctrl->max_ports) {
        return -1;
    }

    xhci_port_regs_t* port_regs = &ctrl->ports[port - 1];

    uint32_t portsc = port_regs->portsc & ~XHCI_PORTSC_W1C_MASK;
    portsc &= ~XHCI_PORTSC_PED;
    port_regs->portsc = portsc;

    debug_printf("[xHCI Port] Port %u disabled\n", port);
    return 0;
}
