#ifndef ATA_H
#define ATA_H

#include "ktypes.h"
#include "boxos_limits.h"


#define ATA_CHANNEL_COUNT       2
#define ATA_DRIVE_COUNT         4

#define ATA_CMD_READ_SECTORS        0x20
#define ATA_CMD_WRITE_SECTORS       0x30
#define ATA_CMD_READ_SECTORS_EXT    0x24
#define ATA_CMD_WRITE_SECTORS_EXT   0x34
#define ATA_CMD_READ_DMA            0xC8
#define ATA_CMD_WRITE_DMA           0xCA
#define ATA_CMD_READ_DMA_EXT        0x25
#define ATA_CMD_WRITE_DMA_EXT       0x35
#define ATA_CMD_IDENTIFY            0xEC
#define ATA_CMD_IDENTIFY_PACKET     0xA1
#define ATA_CMD_CACHE_FLUSH         0xE7
#define ATA_CMD_CACHE_FLUSH_EXT     0xEA
#define ATA_CMD_SET_FEATURES        0xEF

#define ATA_SF_SET_XFER_MODE        0x03

typedef enum {
    ATA_DMA_TYPE_NONE = 0,
    ATA_DMA_TYPE_MDMA = 1,
    ATA_DMA_TYPE_UDMA = 2,
} AtaDmaType;

#define ATA_SR_BSY              0x80
#define ATA_SR_DRDY             0x40
#define ATA_SR_DF               0x20
#define ATA_SR_DSC              0x10
#define ATA_SR_DRQ              0x08
#define ATA_SR_CORR             0x04
#define ATA_SR_IDX              0x02
#define ATA_SR_ERR              0x01

#define ATA_DRIVE_MASTER        0xA0
#define ATA_DRIVE_SLAVE         0xB0

#define ATA_SUCCESS                 0
#define ATA_ERR_INVALID_ARGS       -1
#define ATA_ERR_NO_DEVICE          -2
#define ATA_ERR_LBA_OUT_OF_BOUNDS  -3
#define ATA_ERR_TIMEOUT            -4
#define ATA_ERR_DRIVE_FAULT        -5
#define ATA_ERR_PROTECTED_SECTOR   -6
#define ATA_ERR_FLUSH_FAILED       -7
#define ATA_ERR_MAX_RETRIES        -8
#define ATA_ERR_NOT_SUPPORTED      -9

typedef struct {
    bool        present;
    bool        native_mode;
    uint16_t    cmd_base;
    uint16_t    ctrl_reg;
    uint16_t    bmide_base;
    uint8_t     irq_gsi;
} AtaChannel;

typedef struct {
    uint8_t  exists;
    uint8_t  is_atapi;
    uint8_t  channel;
    uint8_t  is_slave;
    uint8_t  lba48_supported;
    uint8_t  pio_mode;
    uint8_t  dma_type;
    uint8_t  dma_mode;
    uint32_t logical_sector_size;
    uint32_t physical_sector_size;
    uint64_t total_sectors;
    uint64_t size_mb;
    char     model[41];
    char     serial[21];
} ATADevice;

extern AtaChannel g_ata_channels[ATA_CHANNEL_COUNT];
extern ATADevice  g_ata_devices[ATA_DRIVE_COUNT];

#define ata_primary_master   (g_ata_devices[0])
#define ata_primary_slave    (g_ata_devices[1])

void ata_init(void);
int  ata_identify(uint8_t drive_idx, ATADevice* device);

int  ata_read_sectors(uint8_t drive_idx, uint64_t lba, uint16_t count, uint8_t* buffer);
int  ata_write_sectors(uint8_t drive_idx, uint64_t lba, uint16_t count, const uint8_t* buffer);
int  ata_flush_cache(uint8_t drive_idx);

uint32_t ata_max_run_sectors(uint8_t drive_idx);

int  ata_read_sectors_retry(uint8_t drive_idx, uint64_t lba, uint16_t count, uint8_t* buffer);
int  ata_write_sectors_retry(uint8_t drive_idx, uint64_t lba, uint16_t count, const uint8_t* buffer);

void ata_print_device_info(const ATADevice* device);

int  ata_program_lba(uint8_t drive_idx, uint64_t lba, uint16_t count,
                     uint8_t cmd28, uint8_t cmd48);
int  ata_set_xfer_mode(uint8_t drive_idx, uint8_t mode_byte);

void ata_channel_soft_reset(uint8_t channel);

#endif