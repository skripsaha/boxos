#ifndef AHCI_SYNC_H
#define AHCI_SYNC_H

#include "ktypes.h"

int ahci_read_sectors_sync(uint8_t port, uint64_t lba,
                           uint16_t sector_count, void* buffer);

int ahci_write_sectors_sync(uint8_t port, uint64_t lba,
                            uint16_t sector_count, const void* buffer);

int ahci_flush_cache_sync(uint8_t port);

#endif

/*
 * The largest run of 512-byte sectors this port takes in one command.
 *
 * Two bounds, and the smaller wins. The command uses a SINGLE PRD entry whose
 * byte count is 22 bits, so 4 MiB is the hardware ceiling — and the sync path
 * bounces through a physically CONTIGUOUS DMA32 buffer it allocates per call,
 * which is the real limit long before that. The number below is what the buddy
 * allocator hands out without complaint; a bigger run would not be faster if
 * the allocation for it failed, and a failed allocation here is a failed read.
 */
uint32_t ahci_max_run_sectors(uint8_t port);
