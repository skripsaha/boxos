#ifndef XHCI_INTERRUPT_H
#define XHCI_INTERRUPT_H

#include "ktypes.h"

void xhci_irq_handler(void);
void xhci_process_events(void);
void xhci_poll_events(void);

#endif
