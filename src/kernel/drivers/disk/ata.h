#ifndef ATA_H
#define ATA_H

#include "ktypes.h"
#include "boxos_limits.h"

// ============================================================================
// ATA/IDE Driver - Primary Interface для работы с жестким диском
// ============================================================================
//
// Поддержка PIO Mode (Programmed I/O) для чтения/записи секторов
// LBA28 адресация (до 128GB дисков)
//
// ============================================================================

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

// Constants
#define ATA_SECTOR_SIZE         BOXOS_ATA_SECTOR_SIZE     // Bytes per sector

// ============================================================================
// Return Codes
// ============================================================================

#define ATA_SUCCESS                 0
#define ATA_ERR_INVALID_ARGS       -1   // NULL pointer or count=0
#define ATA_ERR_NO_DEVICE          -2   // Drive does not exist
#define ATA_ERR_LBA_OUT_OF_BOUNDS  -3   // LBA >= total_sectors
#define ATA_ERR_TIMEOUT            -4   // BSY or DRQ timeout
#define ATA_ERR_DRIVE_FAULT        -5   // DF bit set (hardware failure)
#define ATA_ERR_PROTECTED_SECTOR   -6   // Write to bootloader area blocked
#define ATA_ERR_FLUSH_FAILED       -7   // Cache flush timeout
#define ATA_ERR_MAX_RETRIES        -8   // All retry attempts failed

// ============================================================================
// ATA DEVICE INFO
// ============================================================================

typedef struct {
    uint8_t exists;                     // 1 if drive exists
    uint8_t is_master;                  // 1 if master, 0 if slave
    uint32_t total_sectors;             // Total LBA28 sectors
    uint64_t size_mb;                   // Size in megabytes
    char model[41];                     // Model string (40 chars + null)
    char serial[21];                    // Serial number (20 chars + null)
} ATADevice;

// ============================================================================
// INITIALIZATION
// ============================================================================

// Инициализация ATA драйвера
void ata_init(void);

// Определение устройства (IDENTIFY DEVICE)
int ata_identify(uint8_t is_master, ATADevice* device);

// ============================================================================
// I/O OPERATIONS
// ============================================================================

// Чтение секторов с диска
// lba: Logical Block Address (номер сектора)
// count: количество секторов (1-256)
// buffer: буфер для данных (должен быть >= count * 512 байт)
// Возвращает: 0 при успехе, -1 при ошибке
int ata_read_sectors(uint8_t is_master, uint32_t lba, uint8_t count, uint8_t* buffer);

// Запись секторов на диск
// lba: Logical Block Address (номер сектора)
// count: количество секторов (1-256)
// buffer: данные для записи (должен быть >= count * 512 байт)
// Возвращает: 0 при успехе, -1 при ошибке
int ata_write_sectors(uint8_t is_master, uint32_t lba, uint8_t count, const uint8_t* buffer);

// Сброс кеша записи на диск (flush)
int ata_flush_cache(uint8_t is_master);

// Чтение секторов с retry логикой (более надёжно)
int ata_read_sectors_retry(uint8_t is_master, uint32_t lba, uint8_t count, uint8_t* buffer);

// Запись секторов с retry логикой (более надёжно)
int ata_write_sectors_retry(uint8_t is_master, uint32_t lba, uint8_t count, const uint8_t* buffer);

// ============================================================================
// HIGH-LEVEL BLOCK I/O для TagFS (4KB блоки)
// ============================================================================

// Читать 1 блок TagFS (4KB = 8 секторов)
int ata_read_block(uint32_t block_num, uint8_t* buffer);

// Записать 1 блок TagFS (4KB = 8 секторов)
int ata_write_block(uint32_t block_num, const uint8_t* buffer);

// Читать несколько блоков подряд
int ata_read_blocks(uint32_t start_block, uint32_t count, uint8_t* buffer);

// Записать несколько блоков подряд
int ata_write_blocks(uint32_t start_block, uint32_t count, const uint8_t* buffer);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Ждать готовности диска
int ata_wait_ready(void);

// Ждать DRQ (Data Request)
int ata_wait_drq(void);

// Прочитать статус
uint8_t ata_read_status(void);

// Вывести информацию об устройстве
void ata_print_device_info(const ATADevice* device);

// ============================================================================
// GLOBAL DEVICES
// ============================================================================

extern ATADevice ata_primary_master;
extern ATADevice ata_primary_slave;

#endif // ATA_H
