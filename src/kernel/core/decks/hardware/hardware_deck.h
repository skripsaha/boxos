#ifndef HARDWARE_DECK_H
#define HARDWARE_DECK_H

#include "ktypes.h"
#include "boxos_decks.h"
#include "op_registry.h"

#define HARDWARE_DECK_ID DECK_HARDWARE

/* The deck's opcodes live in boxos_decks.h, shared with boxlib. */

error_t HardwareDeckRegister(void);

/* The disks and their tables (hardware_disks.c). */
int HwDisksList(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx);
int HwDiskLay(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx);
int HwDiskAdd(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx);
int HwDiskRemove(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx);
int HwDiskMend(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx);
int HwDiskGrow(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx);
int HwDiskBoot(const ManifestOp* op, Crate* crates, uint16_t crate_count, const OpContext* ctx);

/*
 * Prove, on this machine, that asking the hardware deck to put a USB controller
 * back in service does it. Runs once, on the first pass through the idle or guide loop with a
 * controller in service and a volume mounted; that is a fact about the machine
 * and not a moment waited for.
 *
 * It costs the machine every USB device it has, once, so it exists only in a
 * build made with USBRECOVER=on. Off, it is not a call at all.
 */
#if CONFIG_USB_RECOVER_PROOF
void HardwareDeckUsbRecoverProof(void);
#else
static inline void HardwareDeckUsbRecoverProof(void) { }
#endif

#endif /* HARDWARE_DECK_H */
