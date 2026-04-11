#ifndef ATA_DMA_H
#define ATA_DMA_H

#include "ktypes.h"
#include "async_io.h"
#include "kernel_config.h"

// PIIX4 IDE (emulated by QEMU) does not support NCQ: single BMCR, BMSR, and PRD table pointer.
// Submitting two DMA commands simultaneously causes data loss. If DMA is busy, fallback to PIO.

#define ATA_DMA_MAX_REQUESTS    16
#define ATA_DMA_PRD_MAX_ENTRIES 8
#define ATA_DMA_TIMEOUT_MS      5000
#define ATA_DMA_BUFFER_SIZE     (128 * 1024)
#define ATA_DMA_MAX_RETRIES     CONFIG_ATA_MAX_RETRIES

#define ATA_DMA_CMD_START       0x01
#define ATA_DMA_CMD_STOP        0x00
#define ATA_DMA_CMD_READ        0x08

#define ATA_DMA_STATUS_ACTIVE   0x01
#define ATA_DMA_STATUS_ERROR    0x02
#define ATA_DMA_STATUS_IRQ      0x04

#define ATA_CMD_READ_DMA        0xC8
#define ATA_CMD_WRITE_DMA       0xCA
#define ATA_CMD_READ_DMA_EXT    0x25    // 48-bit LBA
#define ATA_CMD_WRITE_DMA_EXT   0x35    // 48-bit LBA

#define ATA_PRD_EOT             0x8000

typedef struct __attribute__((packed)) {
    uint32_t phys_addr;
    uint16_t byte_count;
    uint16_t flags;
} ata_prd_entry_t;

STATIC_ASSERT(sizeof(ata_prd_entry_t) == 8, "PRD entry must be 8 bytes");

typedef enum {
    ATA_DMA_STATUS_FREE = 0,
    ATA_DMA_STATUS_PENDING,
    ATA_DMA_STATUS_IN_PROGRESS,
    ATA_DMA_STATUS_COMPLETED,
    ATA_DMA_STATUS_ERROR_TIMEOUT,
    ATA_DMA_STATUS_ERROR_DMA,
    ATA_DMA_STATUS_ERROR_ATA
} ata_dma_status_t;

typedef struct {
    uint32_t event_id;
    uint32_t pid;
    uint64_t lba;               // 48-bit LBA support
    uint16_t sector_count;
    uint8_t is_master;
    uint8_t is_write;
    uint8_t* buffer_virt;
    uintptr_t buffer_phys;
    uint32_t buffer_size;
    ata_dma_status_t status;
    uint64_t start_time;
    uint8_t retry_count;
    uint8_t reserved[3];

    uint32_t file_id;
    uint64_t write_offset;
    uint64_t original_file_size;
    uint64_t data_addr;          // user heap address for result data
} ata_dma_request_t;

typedef struct {
    bool available;
    uint16_t bus_master_base;
    uint8_t* prd_table_virt;
    uintptr_t prd_table_phys;

    // PIIX4 limitation: only 1 active request at a time
    ata_dma_request_t active_request;
    uint8_t active_request_idx;  // 0 = active, 0xFF = idle

    atomic_u32_t total_requests;
    atomic_u32_t completed_requests;
    atomic_u32_t failed_requests;
    atomic_u32_t timeout_count;

    spinlock_t lock;
} ata_dma_state_t;

int ata_dma_init(void);
bool ata_dma_available(void);

int ata_dma_read_async(uint8_t is_master, uint32_t lba, uint16_t sector_count,
                       uint8_t* buffer_virt, uint32_t event_id, uint32_t pid);

int ata_dma_write_async(uint8_t is_master, uint32_t lba, uint16_t sector_count,
                        const uint8_t* buffer_virt, uint32_t event_id, uint32_t pid);

void ata_dma_irq_handler(void);
void ata_dma_check_timeouts(void);

int ata_read_sectors_dma(uint8_t is_master, uint32_t lba, uint8_t count, uint8_t* buffer);
int ata_write_sectors_dma(uint8_t is_master, uint32_t lba, uint8_t count, const uint8_t* buffer);

typedef struct {
    bool dma_available;
    uint16_t bus_master_base;
    uint32_t total_requests;
    uint32_t completed_requests;
    uint32_t failed_requests;
    uint32_t timeout_count;
    uint8_t active_requests;
} ata_dma_stats_t;

void ata_dma_get_stats(ata_dma_stats_t *stats);
void ata_dma_print_stats(void);

bool ata_dma_is_idle(void);
int ata_dma_start_async_transfer(async_io_request_t* req);

#endif
