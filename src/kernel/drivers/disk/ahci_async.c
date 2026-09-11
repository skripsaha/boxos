#include "ahci_async.h"
#include "ahci.h"
#include "klib.h"
#include "atomics.h"

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

    error_t err = ahci_build_io(port, (uint8_t)slot, lba,
                                sector_count, dma_buf_phys, false);
    if (err != OK) {
        ahci_free_slot(port, (uint8_t)slot);
        return err;
    }

    __atomic_fetch_add(&port_state->stats.cmd_count, 1, __ATOMIC_RELAXED);
    port_state->event_id[slot]   = 0;
    port_state->pid[slot]        = 0;

    port_state->cb[slot]     = cb;
    port_state->cb_ctx[slot] = ctx;
    ahci_arm_slot(port, (uint8_t)slot);

    *out_slot = (uint8_t)slot;
    return OK;
}

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

    port_state->cb[slot]     = cb;
    port_state->cb_ctx[slot] = ctx;
    ahci_arm_slot(port, (uint8_t)slot);

    *out_slot = (uint8_t)slot;
    return OK;
}