#ifndef AHCI_ASYNC_H
#define AHCI_ASYNC_H

#include "ktypes.h"
#include "error.h"
#include "ahci.h"


typedef void (*ahci_async_cb_t)(uint8_t port, uint8_t slot,
                                error_t status, void *ctx);

error_t ahci_submit_read_async(uint8_t port, uint64_t lba,
                                uint16_t sector_count,
                                void *dma_buf_phys,
                                ahci_async_cb_t cb, void *ctx,
                                uint8_t *out_slot);

error_t ahci_submit_write_async(uint8_t port, uint64_t lba,
                                 uint16_t sector_count,
                                 void *dma_buf_phys,
                                 ahci_async_cb_t cb, void *ctx,
                                 uint8_t *out_slot);

#endif