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

    /* Build the NCQ READ command exactly like the sync path. */
    error_t err = ahci_build_ncq_read(port, (uint8_t)slot, lba,
                                       sector_count, dma_buf_phys);
    if (err != OK) {
        ahci_free_slot(port, (uint8_t)slot);
        return err;
    }

    __atomic_fetch_add(&port_state->stats.cmd_count, 1, __ATOMIC_RELAXED);
    port_state->event_id[slot]   = 0;  /* reserved for stage 2 (per-request id) */
    port_state->pid[slot]        = 0;  /* set by storage_ops in stage 2 */
    port_state->submit_tsc[slot] = rdtsc();

    /* Publish the callback BEFORE arming CI so the IRQ side never
     * observes a completed slot whose cb is still NULL. The order
     * matters: cb store must happen before CI write becomes visible
     * to the controller's completion path. The spinlock around CI
     * already gives us a release boundary. */
    port_state->cb[slot]     = cb;
    port_state->cb_ctx[slot] = ctx;

    volatile ahci_port_regs_t *regs = ahci_get_port_regs_pub(port);
    spin_lock(&port_state->lock);
    regs->sact |= (1U << slot);
    regs->ci   |= (1U << slot);
    /* Track issued slot for the IRQ handler's snapshot/diff path. */
    __sync_fetch_and_or(&port_state->ci_snapshot, (1U << slot));
    spin_unlock(&port_state->lock);

    *out_slot = (uint8_t)slot;
    return OK;
}

/*
 * AHCI async write submission. Mirror of ahci_submit_read_async; uses
 * ahci_build_ncq_write to build the FIS for ATA WRITE FPDMA QUEUED. The
 * caller pre-staged the bytes into dma_buf_phys; this routine never
 * touches the buffer's bytes — only addresses it via the PRDT.
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

    error_t err = ahci_build_ncq_write(port, (uint8_t)slot, lba,
                                        sector_count, dma_buf_phys);
    if (err != OK) {
        ahci_free_slot(port, (uint8_t)slot);
        return err;
    }

    __atomic_fetch_add(&port_state->stats.cmd_count, 1, __ATOMIC_RELAXED);
    port_state->event_id[slot]   = 0;
    port_state->pid[slot]        = 0;
    port_state->submit_tsc[slot] = rdtsc();

    /* Same publication ordering as the read path: cb installed BEFORE
     * the CI write that arms the controller. */
    port_state->cb[slot]     = cb;
    port_state->cb_ctx[slot] = ctx;

    volatile ahci_port_regs_t *regs = ahci_get_port_regs_pub(port);
    spin_lock(&port_state->lock);
    regs->sact |= (1U << slot);
    regs->ci   |= (1U << slot);
    __sync_fetch_and_or(&port_state->ci_snapshot, (1U << slot));
    spin_unlock(&port_state->lock);

    *out_slot = (uint8_t)slot;
    return OK;
}
