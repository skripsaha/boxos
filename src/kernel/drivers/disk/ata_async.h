#ifndef ATA_ASYNC_H
#define ATA_ASYNC_H

#include "ata.h"
#include "error.h"


#define ATA_ASYNC_MAX_SECTORS 8

void ata_async_init(void);

void ata_async_negotiate_dma_mode(uint8_t drive_idx, const uint16_t *id);

bool ata_async_usable(uint8_t drive_idx);

int ata_dma_sync(uint8_t drive_idx, uint64_t lba, uint16_t count,
                 bool is_write, void *buf);

void bmide_watchdog_scan(void);

error_t bmide_watchdog_selftest(void);

error_t bmide_wedge_selftest(void);

uint64_t ata_async_cmds_submitted(uint8_t channel);
uint64_t ata_async_cmds_completed(uint8_t channel);
uint64_t ata_async_cmds_failed(uint8_t channel);
uint64_t ata_async_spurious_irqs(uint8_t channel);
uint32_t ata_async_queue_depth(uint8_t channel);

#endif