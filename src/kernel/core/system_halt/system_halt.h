#ifndef SYSTEM_HALT_H
#define SYSTEM_HALT_H

#include "ktypes.h"

// Unified system shutdown sequence.
// If `reboot` is true, reset CPU after cleanup. Otherwise, power off via ACPI.
// This function never returns.
void system_halt(bool reboot) __attribute__((noreturn));

// Stop every other core and wait for each one's own word that it has stopped
// (the IPI_PANIC roll call). A core that answered was not inside a stretch that
// kept interrupts off. That is not "it held no lock": ata_dma_sync
// (drivers/disk/ata_async.c) runs `sti` and waits for its DMA inside its
// caller's spin_lock sections — g_ledger_lock through tagfs_write_ledger, on a
// legacy-IDE seat with more than one core — so a core can answer holding one.
// A core that never answered is named, and its process locks are forced open.
// The caller has interrupts off and never returns to normal running:
// system_halt, and the SETTLECUT test kernel (tagfs/settle_cut). Harmless to
// call twice — cores already offline are not asked again.
void halt_all_ap_cores(void);

#endif // SYSTEM_HALT_H
