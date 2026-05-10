#ifndef AHCI_ASYNC_H
#define AHCI_ASYNC_H

#include "ktypes.h"
#include "error.h"
#include "ahci.h"

/*
 * AHCI async submission — non-blocking read/write.
 *
 * Stage 1 of the storage-async migration: ground-floor IRQ-driven
 * completion path. The submitter posts a request and returns
 * immediately; when the disk command retires the AHCI IRQ handler
 * invokes the registered per-slot callback (see ahci_port_t::cb).
 *
 * Sync callers (ahci_read_sectors_sync) keep working unchanged —
 * each slot is owned by exactly one flow, and sync flow leaves
 * cb[slot] NULL so the IRQ-side fan-out skips it.
 *
 * Stage 2 wires this into storage_ops's ObjRead so a userspace
 * fread parks (PROC_WAITING), the K-Core moves on, and the IRQ
 * does KResultPush on completion.
 */

/* Completion callback signature. Fires from IRQ context — keep it
 * short and non-blocking. `status` is OK or an ERR_* code. */
typedef void (*ahci_async_cb_t)(uint8_t port, uint8_t slot,
                                error_t status, void *ctx);

/*
 * Submit a non-blocking read. Reads `sector_count` 512-byte sectors
 * starting at `lba` into the DMA buffer at `dma_buf_phys` (must be
 * < 4 GiB unless the controller advertises s64a). On success
 * returns OK and writes the slot index into *out_slot; the supplied
 * callback runs from the AHCI IRQ when the command retires.
 *
 * Failure modes (no callback fires, slot freed):
 *   ERR_DEVICE_NOT_READY  — port inactive
 *   ERR_BUSY              — no free slot
 *   ERR_INVALID_ARGUMENT  — bad pointer / count == 0
 *   ERR_IO                — controller rejected the FIS build
 */
error_t ahci_submit_read_async(uint8_t port, uint64_t lba,
                                uint16_t sector_count,
                                void *dma_buf_phys,
                                ahci_async_cb_t cb, void *ctx,
                                uint8_t *out_slot);

/*
 * Submit a non-blocking write. Symmetric to read_async — the buffer at
 * `dma_buf_phys` is the source; caller has already staged bytes there.
 * Same failure modes and slot semantics. The callback fires from the
 * AHCI IRQ when the controller retires the slot.
 */
error_t ahci_submit_write_async(uint8_t port, uint64_t lba,
                                 uint16_t sector_count,
                                 void *dma_buf_phys,
                                 ahci_async_cb_t cb, void *ctx,
                                 uint8_t *out_slot);

#endif /* AHCI_ASYNC_H */
