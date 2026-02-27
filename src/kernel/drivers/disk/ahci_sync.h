#ifndef AHCI_SYNC_H
#define AHCI_SYNC_H

#include "ktypes.h"

int ahci_read_sectors_sync(uint8_t port, uint32_t lba,
                           uint16_t sector_count, void* buffer);

int ahci_write_sectors_sync(uint8_t port, uint32_t lba,
                            uint16_t sector_count, const void* buffer);

int ahci_flush_cache_sync(uint8_t port);

#endif
