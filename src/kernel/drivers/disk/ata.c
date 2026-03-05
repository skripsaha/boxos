#include "ata.h"
#include "klib.h"
#include "io.h"
#include "cpu_calibrate.h"
#include "kernel_config.h"
#include "atomics.h"
#include "ata_dma.h"
#include "ahci.h"
#include "ahci_sync.h"

ATADevice ata_primary_master;
ATADevice ata_primary_slave;

static inline void ata_delay_400ns(void) {
    // Read status register 4 times (each read = ~100ns)
    for (int i = 0; i < 4; i++) {
        inb(ATA_PRIMARY_ALTSTATUS);
    }
}

static inline void ata_select_drive(uint8_t is_master) {
    if (is_master) {
        outb(ATA_PRIMARY_DRIVE, ATA_DRIVE_MASTER);
    } else {
        outb(ATA_PRIMARY_DRIVE, ATA_DRIVE_SLAVE);
    }
    ata_delay_400ns();
}

uint8_t ata_read_status(void) {
    return inb(ATA_PRIMARY_STATUS);
}

int ata_wait_ready(void) {
    uint64_t timeout_tsc = rdtsc() + cpu_ms_to_tsc(CONFIG_ATA_TIMEOUT_MS);

    while (rdtsc() < timeout_tsc) {
        uint8_t status = ata_read_status();
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY)) {
            return 0;
        }
    }

    return -1;
}

int ata_wait_drq(void) {
    uint64_t timeout_tsc = rdtsc() + cpu_ms_to_tsc(CONFIG_ATA_TIMEOUT_MS);

    while (rdtsc() < timeout_tsc) {
        uint8_t status = ata_read_status();
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) {
            return 0;
        }
        if (status & ATA_SR_ERR) {
            return -1;
        }
    }

    return -1;
}


static void ata_string_fixup(char* str, int len) {
    // ATA strings are byte-swapped pairs
    for (int i = 0; i < len; i += 2) {
        char tmp = str[i];
        str[i] = str[i + 1];
        str[i + 1] = tmp;
    }

    for (int i = len - 1; i >= 0; i--) {
        if (str[i] == ' ') {
            str[i] = '\0';
        } else {
            break;
        }
    }
    str[len] = '\0';
}


int ata_identify(uint8_t is_master, ATADevice* device) {
    uint16_t identify_data[256];

    memset(device, 0, sizeof(ATADevice));
    device->is_master = is_master;

    ata_select_drive(is_master);

    outb(ATA_PRIMARY_SECCOUNT, 0);
    outb(ATA_PRIMARY_LBA_LO, 0);
    outb(ATA_PRIMARY_LBA_MID, 0);
    outb(ATA_PRIMARY_LBA_HI, 0);

    outb(ATA_PRIMARY_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay_400ns();

    uint8_t status = ata_read_status();
    if (status == 0 || status == 0xFF) {
        return -1;
    }

    uint64_t timeout_tsc = rdtsc() + cpu_ms_to_tsc(1000);

    while (rdtsc() < timeout_tsc) {
        status = ata_read_status();
        if (!(status & ATA_SR_BSY)) break;
    }

    if (rdtsc() >= timeout_tsc) {
        return -1;
    }

    // Non-zero means ATAPI (CD-ROM etc.), not ATA
    uint8_t mid = inb(ATA_PRIMARY_LBA_MID);
    uint8_t hi = inb(ATA_PRIMARY_LBA_HI);
    if (mid != 0 || hi != 0) {
        debug_printf("[ATA] Device is not ATA (mid=0x%x, hi=0x%x)\n", mid, hi);
        return -1;
    }

    if (ata_wait_drq() != 0) {
        debug_printf("[ATA] IDENTIFY command failed\n");
        return -1;
    }

    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(ATA_PRIMARY_DATA);
    }

    device->exists = 1;

    // Use memcpy to avoid strict-aliasing violation
    uint32_t total_sectors_tmp;
    memcpy(&total_sectors_tmp, &identify_data[60], sizeof(uint32_t));
    device->total_sectors = total_sectors_tmp;
    device->size_mb = (device->total_sectors / 2048);

    memcpy(device->model, &identify_data[27], 40);
    ata_string_fixup(device->model, 40);

    memcpy(device->serial, &identify_data[10], 20);
    ata_string_fixup(device->serial, 20);

    debug_printf("[ATA] Detected %s: %s (%lu MB, %u sectors)\n",
            is_master ? "master" : "slave",
            device->model,
            device->size_mb,
            device->total_sectors);

    return 0;
}


int ata_read_sectors(uint8_t is_master, uint32_t lba, uint8_t count, uint8_t* buffer) {
    if (!buffer || count == 0) {
        debug_printf("[ATA] ERROR: Cannot read 0 sectors\n");
        return ATA_ERR_INVALID_ARGS;
    }

    if (ahci_is_initialized()) {
        // ATA block layer uses port 0 (boot disk); other ports accessible via AHCI API directly
        return ahci_read_sectors_sync(is_master ? 0 : 1, lba, count, buffer);
    }

    ATADevice* device = is_master ? &ata_primary_master : &ata_primary_slave;
    if (!device->exists) {
        debug_printf("[ATA] ERROR: Device does not exist\n");
        return ATA_ERR_NO_DEVICE;
    }

    if (lba >= device->total_sectors) {
        debug_printf("[ATA] ERROR: LBA %u out of bounds (max %u)\n", lba, device->total_sectors);
        return -1;
    }

    if (ata_wait_ready() != 0) {
        return -1;
    }

    uint8_t drive_bits = is_master ? 0xE0 : 0xF0;  // LBA mode + master/slave
    drive_bits |= (lba >> 24) & 0x0F;
    outb(ATA_PRIMARY_DRIVE, drive_bits);
    ata_delay_400ns();

    outb(ATA_PRIMARY_SECCOUNT, count);
    outb(ATA_PRIMARY_LBA_LO, lba & 0xFF);
    outb(ATA_PRIMARY_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_PRIMARY_LBA_HI, (lba >> 16) & 0xFF);

    outb(ATA_PRIMARY_COMMAND, ATA_CMD_READ_SECTORS);

    for (int sector = 0; sector < count; sector++) {
        if (ata_wait_drq() != 0) {
            debug_printf("[ATA] ERROR: Read failed at sector %d\n", sector);
            return -1;
        }

        uint16_t* buf16 = (uint16_t*)(buffer + sector * ATA_SECTOR_SIZE);
        for (int i = 0; i < 256; i++) {
            buf16[i] = inw(ATA_PRIMARY_DATA);
        }
    }

    return 0;
}


int ata_write_sectors(uint8_t is_master, uint32_t lba, uint8_t count, const uint8_t* buffer) {
    if (!buffer || count == 0) {
        return ATA_ERR_INVALID_ARGS;
    }

    if (ahci_is_initialized()) {
        return ahci_write_sectors_sync(is_master ? 0 : 1, lba, count, buffer);
    }

    ATADevice* device = is_master ? &ata_primary_master : &ata_primary_slave;
    if (!device->exists) {
        return ATA_ERR_NO_DEVICE;
    }

    #define BOOTLOADER_PROTECTED_SECTORS 400

    if (lba < BOOTLOADER_PROTECTED_SECTORS) {
        debug_printf("[ATA] SECURITY: Write to protected LBA %u blocked (bootloader area 0-399)\n", lba);
        return ATA_ERR_PROTECTED_SECTOR;
    }

    if (lba >= device->total_sectors) {
        debug_printf("[ATA] ERROR: LBA %u out of bounds (max %u)\n", lba, device->total_sectors);
        return -1;
    }

    debug_printf("[ATA] WRITE: LBA %u, sectors %u (%u bytes)\n", lba, count, count * 512);

    if (ata_wait_ready() != 0) {
        return -1;
    }

    uint8_t drive_bits = is_master ? 0xE0 : 0xF0;  // LBA mode + master/slave
    drive_bits |= (lba >> 24) & 0x0F;
    outb(ATA_PRIMARY_DRIVE, drive_bits);
    ata_delay_400ns();

    outb(ATA_PRIMARY_SECCOUNT, count);
    outb(ATA_PRIMARY_LBA_LO, lba & 0xFF);
    outb(ATA_PRIMARY_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_PRIMARY_LBA_HI, (lba >> 16) & 0xFF);

    outb(ATA_PRIMARY_COMMAND, ATA_CMD_WRITE_SECTORS);

    for (int sector = 0; sector < count; sector++) {
        if (ata_wait_drq() != 0) {
            debug_printf("[ATA] ERROR: Write failed at sector %d\n", sector);
            return -1;
        }

        const uint16_t* buf16 = (const uint16_t*)(buffer + sector * ATA_SECTOR_SIZE);
        for (int i = 0; i < 256; i++) {
            outw(ATA_PRIMARY_DATA, buf16[i]);
        }
    }

    debug_printf("[ATA] WRITE COMPLETE: LBA %u (%u bytes written to disk)\n", lba, count * 512);

    return 0;
}


int ata_flush_cache(uint8_t is_master) {
    if (ahci_is_initialized()) {
        return ahci_flush_cache_sync(is_master ? 0 : 1);
    }

    for (int retry = 0; retry < 3; retry++) {
        ata_select_drive(is_master);
        outb(ATA_PRIMARY_COMMAND, ATA_CMD_CACHE_FLUSH);

        if (ata_wait_ready() == 0) {
            return 0;
        }
    }

    debug_printf("[ATA] WARNING: Cache flush timeout after 3 retries\n");
    return -1;
}

static uint8_t ata_read_error(void) {
    return inb(ATA_PRIMARY_ERROR);
}

static const char* ata_decode_error(uint8_t error) {
    if (error & 0x01) return "Address mark not found";
    if (error & 0x02) return "Track 0 not found";
    if (error & 0x04) return "Aborted command";
    if (error & 0x08) return "Media change request";
    if (error & 0x10) return "ID not found";
    if (error & 0x20) return "Media changed";
    if (error & 0x40) return "Uncorrectable data error";
    if (error & 0x80) return "Bad block detected";
    return "Unknown error";
}

int ata_read_sectors_retry(uint8_t is_master, uint32_t lba, uint8_t count, uint8_t* buffer) {
    const int MAX_RETRIES = 3;

    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        int result = ata_read_sectors(is_master, lba, count, buffer);

        if (result == 0) {
            return 0;
        }

        uint8_t error = ata_read_error();
        if (error != 0) {
            debug_printf("[ATA] Read error (retry %d/%d): %s (0x%02x)\n",
                    retry + 1, MAX_RETRIES, ata_decode_error(error), error);
        }

        // Brief delay between retries (~1ms)
        uint64_t deadline = rdtsc() + cpu_ms_to_tsc(1);
        while (rdtsc() < deadline) { cpu_pause(); }
    }

    debug_printf("[ATA] Read failed after %d retries at LBA %u\n", MAX_RETRIES, lba);
    return -1;
}

int ata_write_sectors_retry(uint8_t is_master, uint32_t lba, uint8_t count, const uint8_t* buffer) {
    const int MAX_RETRIES = 3;

    for (int retry = 0; retry < MAX_RETRIES; retry++) {
        int result = ata_write_sectors(is_master, lba, count, buffer);

        if (result == 0) {
            return 0;
        }

        uint8_t error = ata_read_error();
        if (error != 0) {
            debug_printf("[ATA] Write error (retry %d/%d): %s (0x%02x)\n",
                    retry + 1, MAX_RETRIES, ata_decode_error(error), error);
        }

        // Brief delay between retries (~1ms)
        uint64_t deadline = rdtsc() + cpu_ms_to_tsc(1);
        while (rdtsc() < deadline) { cpu_pause(); }
    }

    debug_printf("[ATA] Write failed after %d retries at LBA %u\n", MAX_RETRIES, lba);
    return -1;
}

// Sectors 0-15 reserved for bootloader (Stage1 + Stage2 occupy 0-9)
#define FILESYSTEM_START_SECTOR 16

int ata_read_block(uint32_t block_num, uint8_t* buffer) {
    if (!buffer) return ATA_ERR_INVALID_ARGS;
    if (!ata_primary_master.exists) return ATA_ERR_NO_DEVICE;

    // TagFS blocks are already absolute sector numbers (no offset needed)
    uint32_t lba = block_num * 8;
    return ata_read_sectors_retry(1, lba, 8, buffer);
}

int ata_write_block(uint32_t block_num, const uint8_t* buffer) {
    if (!buffer) return ATA_ERR_INVALID_ARGS;
    if (!ata_primary_master.exists) return ATA_ERR_NO_DEVICE;

    // TagFS blocks are already absolute sector numbers (no offset needed)
    uint32_t lba = block_num * 8;
    return ata_write_sectors_retry(1, lba, 8, buffer);
}

int ata_read_blocks(uint32_t start_block, uint32_t count, uint8_t* buffer) {
    for (uint32_t i = 0; i < count; i++) {
        if (ata_read_block(start_block + i, buffer + i * 4096) != 0) {
            return -1;
        }
    }
    return 0;
}

int ata_write_blocks(uint32_t start_block, uint32_t count, const uint8_t* buffer) {
    for (uint32_t i = 0; i < count; i++) {
        if (ata_write_block(start_block + i, buffer + i * 4096) != 0) {
            return -1;
        }
    }
    return 0;
}

static void ata_soft_reset(void) {
    debug_printf("[ATA] Performing software reset...\n");

    outb(ATA_PRIMARY_CONTROL, 0x04);  // SRST=1
    ata_delay_400ns();
    ata_delay_400ns();  // Minimum 800ns

    outb(ATA_PRIMARY_CONTROL, 0x00);  // SRST=0
    ata_delay_400ns();

    // BSY can take up to 31 seconds to clear per spec; 10s is reasonable
    uint64_t timeout_tsc = rdtsc() + cpu_ms_to_tsc(10000);

    while (rdtsc() < timeout_tsc) {
        uint8_t status = ata_read_status();

        if (status == 0 || status == 0xFF) {
            debug_printf("[ATA] No drive detected after reset (status=0x%x)\n", status);
            return;
        }

        if (!(status & ATA_SR_BSY)) {
            debug_printf("[ATA] Reset complete, drive ready\n");
            return;
        }
    }

    debug_printf("[ATA] Reset timeout - continuing anyway\n");
}

void ata_init(void) {
    debug_printf("[ATA] Initializing ATA/IDE driver...\n");

    memset(&ata_primary_master, 0, sizeof(ATADevice));
    memset(&ata_primary_slave, 0, sizeof(ATADevice));

    if (ahci_is_initialized()) {
        debug_printf("[ATA] AHCI controller detected, using AHCI backend\n");

        ata_primary_master.exists = 1;
        ata_primary_master.is_master = 1;
        ata_primary_master.total_sectors = 0xFFFFFFFF;
        strncpy(ata_primary_master.model, "AHCI SATA Drive", 40);
        ata_primary_master.model[40] = '\0';

        debug_printf("[ATA] Using AHCI synchronous wrapper\n");
        return;
    }

    debug_printf("[ATA] No AHCI detected, using legacy PIO mode\n");
    ata_soft_reset();

    debug_printf("[ATA] Detecting primary master...\n");
    int result = ata_identify(1, &ata_primary_master);
    if (result == 0) {
        debug_printf("[ATA] Primary master detected: %s (%lu MB, %lu sectors)\n",
                ata_primary_master.model, ata_primary_master.size_mb,
                (unsigned long)ata_primary_master.total_sectors);
    } else {
        debug_printf("[ATA] No primary master drive detected\n");
    }

    debug_printf("[ATA] Initialization complete (master=%d, slave=%d)\n",
            ata_primary_master.exists, ata_primary_slave.exists);
}

void ata_print_device_info(const ATADevice* device) {
    if (!device->exists) {
        kprintf("  Device does not exist\n");
        return;
    }

    kprintf("  Type: %s\n", device->is_master ? "Master" : "Slave");
    kprintf("  Model: %s\n", device->model);
    kprintf("  Serial: %s\n", device->serial);
    kprintf("  Size: %lu MB (%u sectors)\n", device->size_mb, device->total_sectors);
}
