#ifndef ATA_H
#define ATA_H

#include "ktypes.h"
#include "boxos_limits.h"

#define ATA_PRIMARY_DATA        0x1F0   // Data register (16-bit)
#define ATA_PRIMARY_ERROR       0x1F1   // Error register (read)
#define ATA_PRIMARY_FEATURES    0x1F1   // Features register (write)
#define ATA_PRIMARY_SECCOUNT    0x1F2
#define ATA_PRIMARY_LBA_LO      0x1F3
#define ATA_PRIMARY_LBA_MID     0x1F4
#define ATA_PRIMARY_LBA_HI      0x1F5
#define ATA_PRIMARY_DRIVE       0x1F6   // Drive/Head register
#define ATA_PRIMARY_STATUS      0x1F7   // Status register (read)
#define ATA_PRIMARY_COMMAND     0x1F7   // Command register (write)
#define ATA_PRIMARY_CONTROL     0x3F6
#define ATA_PRIMARY_ALTSTATUS   0x3F6   // Alternate status (read, no interrupt clear)

#define ATA_CMD_READ_SECTORS        0x20
#define ATA_CMD_WRITE_SECTORS       0x30
#define ATA_CMD_READ_SECTORS_EXT    0x24    // 48-bit LBA
#define ATA_CMD_WRITE_SECTORS_EXT   0x34    // 48-bit LBA
#define ATA_CMD_IDENTIFY            0xEC
#define ATA_CMD_CACHE_FLUSH         0xE7
#define ATA_CMD_CACHE_FLUSH_EXT     0xEA    // 48-bit LBA

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

typedef struct {
    uint8_t exists;
    uint8_t is_master;
    uint8_t lba48_supported;    // 1 if 48-bit LBA EXT commands available
    uint8_t reserved;
    uint64_t total_sectors;     // 48-bit sector count (up to 128 PB)
    uint64_t size_mb;
    char model[41];
    char serial[21];
} ATADevice;

void ata_init(void);
int ata_identify(uint8_t is_master, ATADevice* device);

// lba: Logical Block Address (28-bit or 48-bit), count: sectors (1-256)
int ata_read_sectors(uint8_t is_master, uint64_t lba, uint16_t count, uint8_t* buffer);
int ata_write_sectors(uint8_t is_master, uint64_t lba, uint16_t count, const uint8_t* buffer);
int ata_flush_cache(uint8_t is_master);
int ata_read_sectors_retry(uint8_t is_master, uint64_t lba, uint16_t count, uint8_t* buffer);
int ata_write_sectors_retry(uint8_t is_master, uint64_t lba, uint16_t count, const uint8_t* buffer);

// High-level block I/O for TagFS (4KB blocks = 8 sectors)
int ata_read_block(uint32_t block_num, uint8_t* buffer);
int ata_write_block(uint32_t block_num, const uint8_t* buffer);
int ata_read_blocks(uint32_t start_block, uint32_t count, uint8_t* buffer);
int ata_write_blocks(uint32_t start_block, uint32_t count, const uint8_t* buffer);

int ata_wait_ready(void);
int ata_wait_drq(void);
uint8_t ata_read_status(void);
void ata_print_device_info(const ATADevice* device);

extern ATADevice ata_primary_master;
extern ATADevice ata_primary_slave;

#endif // ATA_H
