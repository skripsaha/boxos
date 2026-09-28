#ifndef SYSTEM_HALT_H
#define SYSTEM_HALT_H

#include "ktypes.h"

// Unified system shutdown sequence.
// If `reboot` is true, reset CPU after cleanup. Otherwise, power off via ACPI.
// This function never returns.
void system_halt(bool reboot) __attribute__((noreturn));

// Stop every other core and wait for each one's own word that it has stopped
// (the IPI_PANIC roll call). A core that answered was not inside a stretch that
// kept interrupts off, and a spin_lock section that does not turn them back on
// itself is such a stretch to its end — so a core that answered holds none of
// those. A legacy-IDE DMA wait inside a caller's section no longer turns them
// on: it takes the drive's answer itself (BmideAnswerHere,
// drivers/disk/ata_async.c) instead of waiting for the BSP's interrupt. A core
// that never answered is named, and its process locks are forced open.
// The caller has interrupts off and never returns to normal running:
// system_halt, and the SETTLECUT test kernel (tagfs/settle_cut). Harmless to
// call twice — cores already offline are not asked again.
//
// `orderly` is a shutdown's roll call, and not a stop that stands for a power
// cut (SETTLECUT): it lets a core that holds a Scribe finish its pass and stop
// at the end of it (g_halt_orderly, below).
void halt_all_ap_cores(bool orderly);

// Raised by an orderly halt before its IPI goes out, and never lowered: the
// machine is going off. A core the IPI finds holding a Scribe (tagfs/scribe)
// notes that it owes the halt its stop and runs its pass to the end, so no
// pass is ever cut in the middle and none is ever taken over. A panic does
// not raise it and stops every core where it stands.
extern volatile bool g_halt_orderly;

// This core stops for good: interrupts off, its own word that it arrived
// (CoreDescriptor.halted) published for the roll call, and hlt. The end of
// the IPI_PANIC branch, and of the ScribeGive that lets go of the last
// Scribe of a core an orderly halt found in a pass.
void HaltFreezeHere(void) __attribute__((noreturn));

#endif // SYSTEM_HALT_H
