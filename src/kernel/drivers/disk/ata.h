#ifndef ATA_H
#define ATA_H

#include "ktypes.h"
#include "boxos_limits.h"

// ATA I/O Ports (Primary Bus)
#define ATA_PRIMARY_DATA        0x1F0   // Data register (16-bit)
#define ATA_PRIMARY_ERROR       0x1F1   // Error register (read)
#define ATA_PRIMARY_FEATURES    0x1F1   // Features register (write)
#define ATA_PRIMARY_SECCOUNT    0x1F2   // Sector count
#define ATA_PRIMARY_LBA_LO      0x1F3   // LBA low byte
#define ATA_PRIMARY_LBA_MID     0x1F4   // LBA mid byte
#define ATA_PRIMARY_LBA_HI      0x1F5   // LBA high byte
#define ATA_PRIMARY_DRIVE       0x1F6   // Drive/Head register
#define ATA_PRIMARY_STATUS      0x1F7   // Status register (read)
#define ATA_PRIMARY_COMMAND     0x1F7   // Command register (write)
#define ATA_PRIMARY_CONTROL     0x3F6   // Device control register
#define ATA_PRIMARY_ALTSTATUS   0x3F6   // Alternate status (read)

// ATA Commands
#define ATA_CMD_READ_SECTORS    0x20    // Read sectors with retry
#define ATA_CMD_WRITE_SECTORS   0x30    // Write sectors with retry
#define ATA_CMD_IDENTIFY        0xEC    // Identify drive
#define ATA_CMD_CACHE_FLUSH     0xE7    // Flush write cache

// Status Register Bits
#define ATA_SR_BSY              0x80    // Busy
#define ATA_SR_DRDY             0x40    // Drive ready
#define ATA_SR_DF               0x20    // Drive write fault
#define ATA_SR_DSC              0x10    // Drive seek complete
#define ATA_SR_DRQ              0x08    // Data request ready
#define ATA_SR_CORR             0x04    // Corrected data
#define ATA_SR_IDX              0x02    // Index
#define ATA_SR_ERR              0x01    // Error

// Drive Selection
#define ATA_DRIVE_MASTER        0xA0    // Select master drive
#define ATA_DRIVE_SLAVE         0xB0    // Select slave drive

#define ATA_SECTOR_SIZE         BOXOS_ATA_SECTOR_SIZE

// Return Codes
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
    uint32_t total_sectors;
    uint64_t size_mb;
    char model[41];
    char serial[21];
} ATADevice;

void ata_init(void);
int ata_identify(uint8_t is_master, ATADevice* device);

// lba: Logical Block Address, count: sectors (1-256), buffer: must be >= count*512 bytes
int ata_read_sectors(uint8_t is_master, uint32_t lba, uint8_t count, uint8_t* buffer);
int ata_write_sectors(uint8_t is_master, uint32_t lba, uint8_t count, const uint8_t* buffer);
int ata_flush_cache(uint8_t is_master);
int ata_read_sectors_retry(uint8_t is_master, uint32_t lba, uint8_t count, uint8_t* buffer);
int ata_write_sectors_retry(uint8_t is_master, uint32_t lba, uint8_t count, const uint8_t* buffer);

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
