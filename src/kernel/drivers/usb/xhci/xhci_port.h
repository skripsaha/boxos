#ifndef XHCI_PORT_H
#define XHCI_PORT_H

#include "ktypes.h"
#include "xhci.h"

#define XHCI_PORTSC_CCS          (1 << 0)
#define XHCI_PORTSC_PED          (1 << 1)
#define XHCI_PORTSC_OCA          (1 << 3)
#define XHCI_PORTSC_PR           (1 << 4)
#define XHCI_PORTSC_PLS_MASK     (0xF << 5)
#define XHCI_PORTSC_PP           (1 << 9)
#define XHCI_PORTSC_SPEED_MASK   (0xF << 10)
#define XHCI_PORTSC_LWS          (1 << 16)
#define XHCI_PORTSC_CSC          (1 << 17)
#define XHCI_PORTSC_PEC          (1 << 18)
#define XHCI_PORTSC_WRC          (1 << 19)
#define XHCI_PORTSC_OCC          (1 << 20)
#define XHCI_PORTSC_PRC          (1 << 21)
#define XHCI_PORTSC_PLC          (1 << 22)
#define XHCI_PORTSC_CEC          (1 << 23)

#define XHCI_PORTSC_W1C_MASK     (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | \
                                  XHCI_PORTSC_WRC | XHCI_PORTSC_OCC | \
                                  XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | \
                                  XHCI_PORTSC_CEC)

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
