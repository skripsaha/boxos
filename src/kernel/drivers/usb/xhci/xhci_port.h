#ifndef XHCI_PORT_H
#define XHCI_PORT_H

#include "ktypes.h"
#include "xhci_regs.h"
#include "xhci.h"

#define XHCI_PORT_SPEED_FULL     1
#define XHCI_PORT_SPEED_LOW      2
#define XHCI_PORT_SPEED_HIGH     3
#define XHCI_PORT_SPEED_SUPER    4

uint32_t xhci_get_port_status(xhci_controller_t* ctrl, uint8_t port);
int xhci_reset_port(xhci_controller_t* ctrl, uint8_t port);
int xhci_enable_port(xhci_controller_t* ctrl, uint8_t port);
int xhci_disable_port(xhci_controller_t* ctrl, uint8_t port);
bool xhci_port_has_device(xhci_controller_t* ctrl, uint8_t port);
uint8_t xhci_get_port_speed(xhci_controller_t* ctrl, uint8_t port);
void xhci_port_clear_change_bits(xhci_controller_t* ctrl, uint8_t port, uint32_t bits);

#endif
