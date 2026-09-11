#ifndef XHCI_HUB_H
#define XHCI_HUB_H

#include "ktypes.h"
#include "xhci.h"


int  xhci_hub_attach(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

void xhci_hub_release(xhci_controller_t* ctrl, xhci_device_slot_t* slot);

bool xhci_hub_slot_attached(xhci_device_slot_t* slot);

int  xhci_hub_service(xhci_controller_t* ctrl);

bool xhci_hub_work_pending(void);
void xhci_hub_service_if_pending(void);

void xhci_hub_note_change(xhci_device_slot_t* slot);

void xhci_hub_note_work(void);

#endif