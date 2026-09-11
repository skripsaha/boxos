#ifndef XHCI_PORT_H
#define XHCI_PORT_H

#include "ktypes.h"
#include "xhci_regs.h"
#include "xhci.h"

#define XHCI_PORT_SPEED_FULL     1
#define XHCI_PORT_SPEED_LOW      2
#define XHCI_PORT_SPEED_HIGH     3
#define XHCI_PORT_SPEED_SUPER    4
#define XHCI_PORT_SPEED_SUPER_10 5

#define XHCI_PORT_POWER_SETTLE_MS 100

#define XHCI_PORT_SURVEY_MS 1200

#define XHCI_PORT_RESET_RECOVERY_MS 50

uint32_t xhci_get_port_status(xhci_controller_t* ctrl, uint8_t port);
bool     xhci_port_has_device(xhci_controller_t* ctrl, uint8_t port);
uint8_t  xhci_get_port_speed(xhci_controller_t* ctrl, uint8_t port);
void     xhci_port_clear_change_bits(xhci_controller_t* ctrl, uint8_t port, uint32_t bits);

uint8_t  xhci_port_protocol(xhci_controller_t* ctrl, uint8_t port);

uint8_t  xhci_port_other_half(xhci_controller_t* ctrl, uint8_t port);

bool     xhci_port_socket_is_empty(xhci_controller_t* ctrl, uint8_t port);

void     xhci_power_ports(xhci_controller_t* ctrl);

void     xhci_port_describe(xhci_controller_t* ctrl, uint8_t port);

#define XHCI_PORT_RESET_NONE 0
#define XHCI_PORT_RESET_HOT  1
#define XHCI_PORT_RESET_WARM 2
#define XHCI_PORT_RESET_HUB  3

const char* xhci_port_reset_kind_name(uint8_t kind);

int      xhci_port_begin_reset(xhci_controller_t* ctrl, uint8_t port,
                               uint8_t* out_kind);

int      xhci_port_warm_reset(xhci_controller_t* ctrl, uint8_t port);

bool     xhci_port_reset_finished(xhci_controller_t* ctrl, uint8_t port);

bool     xhci_port_says_gone(xhci_controller_t* ctrl, uint8_t port);

int      xhci_disable_port(xhci_controller_t* ctrl, uint8_t port);

#endif