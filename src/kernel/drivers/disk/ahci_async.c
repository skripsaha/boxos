#include "ahci_async.h"
#include "ahci.h"
#include "klib.h"
#include "atomics.h"

/*
 * AHCI async read submission. Mirrors the FIS-build / CI-write portion
 * of ahci_read_sectors_sync but does NOT poll for completion — the
 * caller registers a callback that fires from the IRQ handler when the
 * slot retires.
 */
error_t ahci_submit_read_async(uint8_t port, uint64_t lba,
                                uint16_t sector_count,
                                void *dma_buf_phys,
                                ahci_async_cb_t cb, void *ctx,
                                uint8_t *out_slot)
{
    if (!cb || !dma_buf_phys || sector_count == 0 || !out_slot)
        return ERR_INVALID_ARGUMENT;

    ahci_port_t *port_state = ahci_get_port_state(port);
    if (!port_state || !port_state->active)
        return ERR_DEVICE_NOT_READY;

    int slot = ahci_alloc_slot(port);
    if (slot < 0)
        return ERR_BUSY;

    /* Build the read command for this port's command class: FPDMA QUEUED on
     * NCQ ports, READ DMA EXT otherwise. The completion register the IRQ
     * watches (PxSACT vs PxCI) is selected the same way. */
    error_t err = ahci_build_io(port, (uint8_t)slot, lba,
                                sector_count, dma_buf_phys, false);
    if (err != OK) {
        ahci_free_slot(port, (uint8_t)slot);
        return err;
    }

    __atomic_fetch_add(&port_state->stats.cmd_count, 1, __ATOMIC_RELAXED);
    port_state->event_id[slot]   = 0;  /* reserved for stage 2 (per-request id) */
    port_state->pid[slot]        = 0;  /* set by storage_ops in stage 2 */
    port_state->submit_tsc[slot] = rdtsc();

    /* Install the callback BEFORE arming so the IRQ never observes a
     * completed slot with a NULL cb. ahci_arm_slot programs PxSACT/PxCI
     * (direct single-bit stores) and records the slot in issued_mask. */
    port_state->cb[slot]     = cb;
    port_state->cb_ctx[slot] = ctx;
    ahci_arm_slot(port, (uint8_t)slot);

    *out_slot = (uint8_t)slot;
    return OK;
}

/*
 * AHCI async write submission. Mirror of ahci_submit_read_async; uses
 * ahci_build_io to build either a WRITE FPDMA QUEUED (NCQ ports) or a
 * WRITE DMA EXT (non-NCQ ports) FIS. The caller pre-staged the bytes into
 * dma_buf_phys; this routine never touches the buffer's bytes — only
 * addresses it via the PRDT.
 *
 * Same lifetime / IRQ ordering rules as the read path. The IRQ handler's
 * fan-out (ahci.c:ahci_irq_handler) sees cb[slot] != NULL and invokes it
 * with status=OK or ERR_IO depending on TFES.
 */
error_t ahci_submit_write_async(uint8_t port, uint64_t lba,
                                 uint16_t sector_count,
                                 void *dma_buf_phys,
                                 ahci_async_cb_t cb, void *ctx,
                                 uint8_t *out_slot)
{
    if (!cb || !dma_buf_phys || sector_count == 0 || !out_slot)
        return ERR_INVALID_ARGUMENT;

    ahci_port_t *port_state = ahci_get_port_state(port);
    if (!port_state || !port_state->active)
        return ERR_DEVICE_NOT_READY;

    int slot = ahci_alloc_slot(port);
    if (slot < 0)
        return ERR_BUSY;

    error_t err = ahci_build_io(port, (uint8_t)slot, lba,
                                sector_count, dma_buf_phys, true);
    if (err != OK) {
        ahci_free_slot(port, (uint8_t)slot);
        return err;
    }

    __atomic_fetch_add(&port_state->stats.cmd_count, 1, __ATOMIC_RELAXED);
    port_state->event_id[slot]   = 0;
    port_state->pid[slot]        = 0;
    port_state->submit_tsc[slot] = rdtsc();

    /* Same ordering as the read path: cb installed before arming. */
    port_state->cb[slot]     = cb;
    port_state->cb_ctx[slot] = ctx;
    ahci_arm_slot(port, (uint8_t)slot);

    *out_slot = (uint8_t)slot;
    return OK;
}
