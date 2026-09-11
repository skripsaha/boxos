#ifndef XHCI_CAPS_H
#define XHCI_CAPS_H

#include "ktypes.h"
#include "xhci.h"


int xhci_claim_from_firmware(xhci_controller_t* ctrl);

void xhci_map_port_protocols(xhci_controller_t* ctrl);

void xhci_pair_port_halves(xhci_controller_t* ctrl);

#endif