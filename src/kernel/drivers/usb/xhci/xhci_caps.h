#ifndef XHCI_CAPS_H
#define XHCI_CAPS_H

#include "ktypes.h"
#include "xhci.h"

/* The extended capability list is where a controller says the things that do
 * not fit in a fixed register: who owns it, and what each of its ports
 * actually is. Both answers are needed before the controller is touched. */

/* Take the controller away from the firmware.
 *
 * On a machine booted through BIOS/CSM the firmware owns the xHC and is
 * emulating any attached USB keyboard as a PS/2 one through SMM. Resetting the
 * controller without asking ends that emulation with the SMM handler still
 * believing it is in charge. This performs the handshake the specification
 * defines, silences the firmware's SMI sources, and only then hands the
 * controller back to the caller.
 *
 * Returns 0 when this kernel owns the controller — including the case where
 * the firmware never let go and ownership had to be taken. */
int xhci_claim_from_firmware(xhci_controller_t* ctrl);

/* Fill ctrl->port_major / ctrl->port_slot_type from every Supported Protocol
 * capability the controller publishes. Ports it does not describe stay zero,
 * and a zero is a port this driver refuses to enumerate rather than guess at. */
void xhci_map_port_protocols(xhci_controller_t* ctrl);

/*
 * Work out which pairs of root ports are one physical socket, and say how many
 * there are. Called by xhci_map_port_protocols once it knows what each port
 * is; separate only because it is a different question with a different answer
 * — see the note on ctrl->port_pair for why the answer is an assumption and
 * how the assumption is kept visible.
 */
void xhci_pair_port_halves(xhci_controller_t* ctrl);

#endif
