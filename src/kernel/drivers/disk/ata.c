#include "ata.h"
#include "ata_async.h"
#include "klib.h"
#include "io.h"
#include "cpu_calibrate.h"
#include "kernel_config.h"
#include "atomics.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "boxos_memory.h"
#include "ahci.h"
#include "ahci_sync.h"


AtaChannel g_ata_channels[ATA_CHANNEL_COUNT];
ATADevice  g_ata_devices[ATA_DRIVE_COUNT];

static spinlock_t g_ata_lock;

#define ATA_REG_DATA            0
#define ATA_REG_ERROR           1
#define ATA_REG_FEATURES        1
#define ATA_REG_SECCOUNT        2
#define ATA_REG_LBA_LO          3
#define ATA_REG_LBA_MID         4
#define ATA_REG_LBA_HI          5
#define ATA_REG_DRIVE_HEAD      6
#define ATA_REG_STATUS          7
#define ATA_REG_COMMAND         7

#define ATA_CTRL_SRST           0x04
#define ATA_CTRL_nIEN           0x02


static inline uint8_t drive_channel(uint8_t drive_idx) { return drive_idx >> 1; }
static inline uint8_t drive_is_slave(uint8_t drive_idx) { return drive_idx & 1; }
static inline AtaChannel* drive_channel_ptr(uint8_t drive_idx) {
    return &g_ata_channels[drive_channel(drive_idx)];
}

static inline void ata_delay_400ns(uint8_t drive_idx) {
    uint16_t ctrl = drive_channel_ptr(drive_idx)->ctrl_reg;
    for (int i = 0; i < 4; i++) (void)inb(ctrl);
}

static inline uint8_t ata_read_status(uint8_t drive_idx) {
    return inb(drive_channel_ptr(drive_idx)->cmd_base + ATA_REG_STATUS);
}

static inline uint8_t ata_read_error(uint8_t drive_idx) {
    return inb(drive_channel_ptr(drive_idx)->cmd_base + ATA_REG_ERROR);
}

static inline void ata_select_drive(uint8_t drive_idx) {
    AtaChannel* ch = drive_channel_ptr(drive_idx);
    uint8_t head_byte = drive_is_slave(drive_idx) ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER;
    outb(ch->cmd_base + ATA_REG_DRIVE_HEAD, head_byte);
    ata_delay_400ns(drive_idx);
}

static int ata_wait_clear_bsy(uint8_t drive_idx, uint32_t timeout_ms) {
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    while (rdtsc() < deadline) {
        uint8_t s = ata_read_status(drive_idx);
        if (s == 0xFF) return ATA_ERR_NO_DEVICE;
        if (!(s & ATA_SR_BSY)) return 0;
    }
    return ATA_ERR_TIMEOUT;
}

static int ata_wait_ready(uint8_t drive_idx) {
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(CONFIG_ATA_TIMEOUT_MS);
    while (rdtsc() < deadline) {
        uint8_t s = ata_read_status(drive_idx);
        if (s == 0xFF) return ATA_ERR_NO_DEVICE;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRDY)) return 0;
    }
    return ATA_ERR_TIMEOUT;
}

static int ata_wait_drq(uint8_t drive_idx) {
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(CONFIG_ATA_IO_TIMEOUT_MS);
    while (rdtsc() < deadline) {
        uint8_t s = ata_read_status(drive_idx);
        if (s == 0xFF) return ATA_ERR_NO_DEVICE;
        if (s & ATA_SR_ERR) return ATA_ERR_DRIVE_FAULT;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return 0;
    }
    return ATA_ERR_TIMEOUT;
}

static const char* ata_decode_error(uint8_t error) {
    if (error & 0x80) return "Bad block detected";
    if (error & 0x40) return "Uncorrectable data error";
    if (error & 0x20) return "Media changed";
    if (error & 0x10) return "ID not found";
    if (error & 0x08) return "Media change request";
    if (error & 0x04) return "Aborted command";
    if (error & 0x02) return "Track 0 not found";
    if (error & 0x01) return "Address mark not found";
    return "Unknown error";
}

static void ata_string_fixup(char* str, int len) {
    for (int i = 0; i < len; i += 2) {
        char tmp = str[i];
        str[i] = str[i + 1];
        str[i + 1] = tmp;
    }
    for (int i = len - 1; i >= 0; i--) {
        if (str[i] == ' ') str[i] = '\0';
        else break;
    }
    str[len] = '\0';
}


int ata_set_xfer_mode(uint8_t drive_idx, uint8_t mode_byte) {
    AtaChannel* ch = drive_channel_ptr(drive_idx);
    ata_select_drive(drive_idx);
    if (ata_wait_clear_bsy(drive_idx, CONFIG_ATA_TIMEOUT_MS) != 0)
        return ATA_ERR_TIMEOUT;

    outb(ch->cmd_base + ATA_REG_FEATURES, ATA_SF_SET_XFER_MODE);
    outb(ch->cmd_base + ATA_REG_SECCOUNT, mode_byte);
    outb(ch->cmd_base + ATA_REG_COMMAND,  ATA_CMD_SET_FEATURES);

    if (ata_wait_clear_bsy(drive_idx, CONFIG_ATA_TIMEOUT_MS) != 0)
        return ATA_ERR_TIMEOUT;
    if (ata_read_status(drive_idx) & ATA_SR_ERR) return ATA_ERR_NOT_SUPPORTED;
    return 0;
}

void ata_channel_soft_reset(uint8_t channel) {
    AtaChannel* ch = &g_ata_channels[channel];
    if (!ch->present) return;

    outb(ch->ctrl_reg, ATA_CTRL_SRST | ATA_CTRL_nIEN);
    for (int i = 0; i < 8; i++) (void)inb(ch->ctrl_reg);

    outb(ch->ctrl_reg, ATA_CTRL_nIEN);
    for (int i = 0; i < 4; i++) (void)inb(ch->ctrl_reg);

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(2000);
    while (rdtsc() < deadline) {
        uint8_t s = inb(ch->cmd_base + ATA_REG_STATUS);
        if (s == 0 || s == 0xFF) return;
        if (!(s & ATA_SR_BSY)) return;
    }
    debug_printf("[ATA] Channel %u reset BSY timeout (continuing)\n", channel);
}

static void ata_negotiate_pio(uint8_t drive_idx, const uint16_t* id) {
    uint8_t advertise = (uint8_t)id[64] & 0x03;
    if (advertise & 0x02) {
        if (ata_set_xfer_mode(drive_idx, 0x08 | 4) == 0) {
            g_ata_devices[drive_idx].pio_mode = 4;
            return;
        }
    }
    if (advertise & 0x01) {
        if (ata_set_xfer_mode(drive_idx, 0x08 | 3) == 0) {
            g_ata_devices[drive_idx].pio_mode = 3;
            return;
        }
    }
    g_ata_devices[drive_idx].pio_mode = 0;
}

int ata_identify(uint8_t drive_idx, ATADevice* device) {
    if (drive_idx >= ATA_DRIVE_COUNT || !device) return ATA_ERR_INVALID_ARGS;

    AtaChannel* ch = drive_channel_ptr(drive_idx);
    if (!ch->present) return ATA_ERR_NO_DEVICE;

    memset(device, 0, sizeof(*device));
    device->channel  = drive_channel(drive_idx);
    device->is_slave = drive_is_slave(drive_idx);

    ata_select_drive(drive_idx);

    outb(ch->cmd_base + ATA_REG_SECCOUNT, 0);
    outb(ch->cmd_base + ATA_REG_LBA_LO,   0);
    outb(ch->cmd_base + ATA_REG_LBA_MID,  0);
    outb(ch->cmd_base + ATA_REG_LBA_HI,   0);
    outb(ch->cmd_base + ATA_REG_COMMAND,  ATA_CMD_IDENTIFY);
    ata_delay_400ns(drive_idx);

    uint8_t s0 = ata_read_status(drive_idx);
    if (s0 == 0 || s0 == 0xFF) return ATA_ERR_NO_DEVICE;

    if (ata_wait_clear_bsy(drive_idx, 1000) != 0) return ATA_ERR_TIMEOUT;

    uint8_t status = ata_read_status(drive_idx);
    uint8_t mid    = inb(ch->cmd_base + ATA_REG_LBA_MID);
    uint8_t hi     = inb(ch->cmd_base + ATA_REG_LBA_HI);

    if (status & ATA_SR_ERR) {
        if (mid == 0x14 && hi == 0xEB) {
            device->is_atapi = 1;
            debug_printf("[ATA] drv%u (ch%u %s): ATAPI device detected "
                         "(CD/DVD); no block-I/O support\n",
                         drive_idx, device->channel,
                         device->is_slave ? "slave" : "master");
        }
        return ATA_ERR_NO_DEVICE;
    }

    if (mid != 0 || hi != 0) {
        debug_printf("[ATA] drv%u: non-ATA signature (mid=0x%02x hi=0x%02x); skipping\n",
                     drive_idx, mid, hi);
        return ATA_ERR_NO_DEVICE;
    }

    if (ata_wait_drq(drive_idx) != 0) {
        debug_printf("[ATA] drv%u: IDENTIFY DRQ failed\n", drive_idx);
        return ATA_ERR_NO_DEVICE;
    }

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ch->cmd_base + ATA_REG_DATA);

    uint32_t logical = 512;
    if ((id[106] & (1u << 14)) && !(id[106] & (1u << 15)) &&
        (id[106] & (1u << 12))) {
        uint32_t words = (uint32_t)id[117] | ((uint32_t)id[118] << 16);
        if (words >= 256) logical = words * 2u;
    }
    if (logical != 512) {
        kprintf("[ATA] drive %u: %u-byte logical sectors, and this stack is "
                "built on 512 — not using it\n", drive_idx, logical);
        return ATA_ERR_NO_DEVICE;
    }
    device->logical_sector_size = logical;

    device->physical_sector_size = logical;
    if ((id[106] & (1u << 14)) && !(id[106] & (1u << 15)) &&
        (id[106] & (1u << 13))) {
        uint32_t exponent = id[106] & 0x0Fu;
        if (exponent < 16 && logical <= (0xFFFFFFFFu >> exponent)) {
            device->physical_sector_size = logical << exponent;
        }
    }

    device->lba48_supported = (id[83] & (1u << 10)) ? 1 : 0;
    if (device->lba48_supported) {
        uint64_t lba48 = 0;
        memcpy(&lba48, &id[100], sizeof(uint64_t));
        device->total_sectors = lba48;
    } else {
        uint32_t lba28 = 0;
        memcpy(&lba28, &id[60], sizeof(uint32_t));
        device->total_sectors = lba28;
    }
    device->size_mb = device->total_sectors / 2048;

    memcpy(device->model, &id[27], 40);
    ata_string_fixup(device->model, 40);
    memcpy(device->serial, &id[10], 20);
    ata_string_fixup(device->serial, 20);

    device->exists = 1;

    ata_negotiate_pio(drive_idx, id);
    ata_async_negotiate_dma_mode(drive_idx, id);

    const char* dma_name = (device->dma_type == ATA_DMA_TYPE_UDMA) ? "UDMA"
                         : (device->dma_type == ATA_DMA_TYPE_MDMA) ? "MDMA"
                         : "none";
    debug_printf("[ATA] drv%u (ch%u %s): %s — %lu MB, %lu sectors, LBA%s, "
                 "PIO mode %u, DMA %s%u\n",
                 drive_idx, device->channel,
                 device->is_slave ? "slave" : "master",
                 device->model, device->size_mb,
                 (unsigned long)device->total_sectors,
                 device->lba48_supported ? "48" : "28",
                 device->pio_mode,
                 dma_name,
                 (device->dma_type == ATA_DMA_TYPE_NONE) ? 0 : device->dma_mode);
    return 0;
}

int ata_program_lba(uint8_t drive_idx, uint64_t lba, uint16_t count,
                    uint8_t cmd28, uint8_t cmd48)
{
    AtaChannel* ch = drive_channel_ptr(drive_idx);
    ATADevice*  d  = &g_ata_devices[drive_idx];

    bool use48 = d->lba48_supported && (lba > 0x0FFFFFFFULL || count > 256);
    uint8_t drv_bit = drive_is_slave(drive_idx) ? 0x10 : 0x00;

    if (use48) {
        outb(ch->cmd_base + ATA_REG_DRIVE_HEAD, 0x40 | drv_bit);
        ata_delay_400ns(drive_idx);
        outb(ch->cmd_base + ATA_REG_SECCOUNT, (uint8_t)((count  >> 8) & 0xFF));
        outb(ch->cmd_base + ATA_REG_LBA_LO,   (uint8_t)((lba    >> 24) & 0xFF));
        outb(ch->cmd_base + ATA_REG_LBA_MID,  (uint8_t)((lba    >> 32) & 0xFF));
        outb(ch->cmd_base + ATA_REG_LBA_HI,   (uint8_t)((lba    >> 40) & 0xFF));
        outb(ch->cmd_base + ATA_REG_SECCOUNT, (uint8_t)(count   & 0xFF));
        outb(ch->cmd_base + ATA_REG_LBA_LO,   (uint8_t)( lba    & 0xFF));
        outb(ch->cmd_base + ATA_REG_LBA_MID,  (uint8_t)((lba >>  8) & 0xFF));
        outb(ch->cmd_base + ATA_REG_LBA_HI,   (uint8_t)((lba >> 16) & 0xFF));
        outb(ch->cmd_base + ATA_REG_COMMAND, cmd48);
    } else {
        uint8_t drive_byte = 0xE0 | drv_bit | (uint8_t)((lba >> 24) & 0x0F);
        outb(ch->cmd_base + ATA_REG_DRIVE_HEAD, drive_byte);
        ata_delay_400ns(drive_idx);
        outb(ch->cmd_base + ATA_REG_SECCOUNT, (uint8_t)count);
        outb(ch->cmd_base + ATA_REG_LBA_LO,   (uint8_t)( lba       & 0xFF));
        outb(ch->cmd_base + ATA_REG_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
        outb(ch->cmd_base + ATA_REG_LBA_HI,   (uint8_t)((lba >> 16) & 0xFF));
        outb(ch->cmd_base + ATA_REG_COMMAND, cmd28);
    }
    return 0;
}

static inline bool ata_legacy_path_active(void) {
    return !ahci_is_initialized();
}

static uint8_t g_ata_dma_fallback_said = 0;

static void ata_note_dma_fallback(uint8_t drive_idx, int rc, uint64_t lba,
                                  bool write)
{
    if (drive_idx >= 8) return;
    if (g_ata_dma_fallback_said & (uint8_t)(1u << drive_idx)) return;
    g_ata_dma_fallback_said |= (uint8_t)(1u << drive_idx);

    kprintf("[ATA] drive %u: DMA %s failed (rc=%d) at LBA %lu — this drive "
            "runs in PIO from here\n",
            drive_idx, write ? "write" : "read", rc, (unsigned long)lba);
}

int ata_read_sectors(uint8_t drive_idx, uint64_t lba, uint16_t count, uint8_t* buffer) {
    if (!buffer || count == 0) return ATA_ERR_INVALID_ARGS;
    if (!ata_legacy_path_active()) return ATA_ERR_NOT_SUPPORTED;
    if (drive_idx >= ATA_DRIVE_COUNT) return ATA_ERR_INVALID_ARGS;

    ATADevice* d = &g_ata_devices[drive_idx];
    AtaChannel* ch = drive_channel_ptr(drive_idx);

    if (!d->exists || !ch->present) return ATA_ERR_NO_DEVICE;
    if (lba + count > d->total_sectors) return ATA_ERR_LBA_OUT_OF_BOUNDS;

    if (ata_async_usable(drive_idx) && count <= ATA_ASYNC_MAX_SECTORS) {
        int rc = ata_dma_sync(drive_idx, lba, count, false, buffer);
        if (rc == 0) return 0;
        ata_note_dma_fallback(drive_idx, rc, lba, false);
    }

    spin_lock(&g_ata_lock);
    ata_select_drive(drive_idx);
    if (ata_wait_ready(drive_idx) != 0) {
        spin_unlock(&g_ata_lock);
        return ATA_ERR_TIMEOUT;
    }
    ata_program_lba(drive_idx, lba, count, ATA_CMD_READ_SECTORS, ATA_CMD_READ_SECTORS_EXT);

    for (uint16_t sec = 0; sec < count; sec++) {
        int rc = ata_wait_drq(drive_idx);
        if (rc != 0) {
            spin_unlock(&g_ata_lock);
            return rc;
        }
        uint16_t* buf16 = (uint16_t*)(buffer + (uint32_t)sec * ATA_SECTOR_SIZE);
        for (int i = 0; i < 256; i++)
            buf16[i] = inw(ch->cmd_base + ATA_REG_DATA);
    }

    spin_unlock(&g_ata_lock);
    return 0;
}

int ata_write_sectors(uint8_t drive_idx, uint64_t lba, uint16_t count, const uint8_t* buffer) {
    if (!buffer || count == 0) return ATA_ERR_INVALID_ARGS;
    if (!ata_legacy_path_active()) return ATA_ERR_NOT_SUPPORTED;
    if (drive_idx >= ATA_DRIVE_COUNT) return ATA_ERR_INVALID_ARGS;

    ATADevice* d = &g_ata_devices[drive_idx];
    AtaChannel* ch = drive_channel_ptr(drive_idx);

    if (!d->exists || !ch->present) return ATA_ERR_NO_DEVICE;

    #define BOOTLOADER_PROTECTED_SECTORS 400
    if (lba < BOOTLOADER_PROTECTED_SECTORS) {
        debug_printf("[ATA] SECURITY: write to protected LBA %lu blocked\n",
                     (unsigned long)lba);
        return ATA_ERR_PROTECTED_SECTOR;
    }
    if (lba + count > d->total_sectors) return ATA_ERR_LBA_OUT_OF_BOUNDS;

    if (ata_async_usable(drive_idx) && count <= ATA_ASYNC_MAX_SECTORS) {
        int rc = ata_dma_sync(drive_idx, lba, count, true, (void*)buffer);
        if (rc == 0) return 0;
        ata_note_dma_fallback(drive_idx, rc, lba, true);
    }

    spin_lock(&g_ata_lock);
    ata_select_drive(drive_idx);
    if (ata_wait_ready(drive_idx) != 0) {
        spin_unlock(&g_ata_lock);
        return ATA_ERR_TIMEOUT;
    }
    ata_program_lba(drive_idx, lba, count, ATA_CMD_WRITE_SECTORS, ATA_CMD_WRITE_SECTORS_EXT);

    for (uint16_t sec = 0; sec < count; sec++) {
        int rc = ata_wait_drq(drive_idx);
        if (rc != 0) {
            spin_unlock(&g_ata_lock);
            return rc;
        }
        const uint16_t* buf16 = (const uint16_t*)(buffer + (uint32_t)sec * ATA_SECTOR_SIZE);
        for (int i = 0; i < 256; i++)
            outw(ch->cmd_base + ATA_REG_DATA, buf16[i]);
    }

    if (ata_wait_clear_bsy(drive_idx, CONFIG_ATA_IO_TIMEOUT_MS) != 0) {
        debug_printf("[ATA] drv%u: BSY-clear timeout after WRITE @LBA %lu\n",
                     drive_idx, (unsigned long)lba);
        spin_unlock(&g_ata_lock);
        return ATA_ERR_TIMEOUT;
    }
    if (ata_read_status(drive_idx) & ATA_SR_ERR) {
        uint8_t e = ata_read_error(drive_idx);
        debug_printf("[ATA] drv%u: ERR=0x%02x (%s) after WRITE @LBA %lu\n",
                     drive_idx, e, ata_decode_error(e), (unsigned long)lba);
        spin_unlock(&g_ata_lock);
        return ATA_ERR_DRIVE_FAULT;
    }

    spin_unlock(&g_ata_lock);
    return 0;
}

int ata_flush_cache(uint8_t drive_idx) {
    if (!ata_legacy_path_active()) return ATA_ERR_NOT_SUPPORTED;
    if (drive_idx >= ATA_DRIVE_COUNT) return ATA_ERR_INVALID_ARGS;
    ATADevice* d = &g_ata_devices[drive_idx];
    AtaChannel* ch = drive_channel_ptr(drive_idx);
    if (!d->exists || !ch->present) return ATA_ERR_NO_DEVICE;

    uint8_t cmd = d->lba48_supported ? ATA_CMD_CACHE_FLUSH_EXT : ATA_CMD_CACHE_FLUSH;

    spin_lock(&g_ata_lock);
    for (int retry = 0; retry < CONFIG_ATA_MAX_RETRIES; retry++) {
        ata_select_drive(drive_idx);
        outb(ch->cmd_base + ATA_REG_COMMAND, cmd);
        if (ata_wait_ready(drive_idx) == 0) {
            spin_unlock(&g_ata_lock);
            return 0;
        }
        if (retry < CONFIG_ATA_MAX_RETRIES - 1)
            ata_channel_soft_reset(drive_channel(drive_idx));
    }
    spin_unlock(&g_ata_lock);
    debug_printf("[ATA] drv%u: cache flush timeout after retries\n", drive_idx);
    return ATA_ERR_FLUSH_FAILED;
}

static int ata_retry_classify(int rc) {
    return (rc == ATA_ERR_NO_DEVICE     ||
            rc == ATA_ERR_LBA_OUT_OF_BOUNDS ||
            rc == ATA_ERR_INVALID_ARGS  ||
            rc == ATA_ERR_PROTECTED_SECTOR ||
            rc == ATA_ERR_NOT_SUPPORTED) ? 1 : 0;
}

static void ata_retry_diag(uint8_t drive_idx, int retry) {
    uint8_t err = ata_read_error(drive_idx);
    if (err) {
        debug_printf("[ATA] drv%u retry %d/%d ERR=0x%02x (%s)\n",
                     drive_idx, retry + 1, CONFIG_ATA_MAX_RETRIES,
                     err, ata_decode_error(err));
    }
    uint64_t until = rdtsc() + cpu_ms_to_tsc(1);
    while (rdtsc() < until) cpu_pause();
}

uint32_t ata_max_run_sectors(uint8_t drive_idx)
{
    if (drive_idx >= ATA_DRIVE_COUNT) {
        return 8u;
    }
    if (ata_async_usable(drive_idx)) {
        return (uint32_t)ATA_ASYNC_MAX_SECTORS;
    }
    return 128u;
}

int ata_read_sectors_retry(uint8_t drive_idx, uint64_t lba, uint16_t count, uint8_t* buffer) {
    for (int retry = 0; retry < CONFIG_ATA_MAX_RETRIES; retry++) {
        int rc = ata_read_sectors(drive_idx, lba, count, buffer);
        if (rc == 0) return 0;
        if (ata_retry_classify(rc)) return rc;
        ata_retry_diag(drive_idx, retry);
    }
    return ATA_ERR_MAX_RETRIES;
}

int ata_write_sectors_retry(uint8_t drive_idx, uint64_t lba, uint16_t count, const uint8_t* buffer) {
    for (int retry = 0; retry < CONFIG_ATA_MAX_RETRIES; retry++) {
        int rc = ata_write_sectors(drive_idx, lba, count, buffer);
        if (rc == 0) return 0;
        if (ata_retry_classify(rc)) return rc;
        ata_retry_diag(drive_idx, retry);
    }
    return ATA_ERR_MAX_RETRIES;
}

static void ata_channels_from_compat(void) {
    g_ata_channels[0] = (AtaChannel){
        .present = true, .native_mode = false,
        .cmd_base = 0x1F0, .ctrl_reg = 0x3F6, .bmide_base = 0, .irq_gsi = 14,
    };
    g_ata_channels[1] = (AtaChannel){
        .present = true, .native_mode = false,
        .cmd_base = 0x170, .ctrl_reg = 0x376, .bmide_base = 0, .irq_gsi = 15,
    };
}

static uint32_t ata_count_extra_ide_controllers(const pci_device_t* primary) {
    uint32_t extra = 0;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint16_t vid = pci_config_read_word((uint8_t)bus, dev, fn, 0x00);
                if (vid == 0xFFFF) {
                    if (fn == 0) break;
                    continue;
                }
                uint8_t cls = pci_config_read_byte((uint8_t)bus, dev, fn, 0x0B);
                uint8_t sub = pci_config_read_byte((uint8_t)bus, dev, fn, 0x0A);
                if (cls == PCI_CLASS_STORAGE && sub == PCI_SUBCLASS_IDE) {
                    if (primary && bus == primary->bus &&
                        dev == primary->device && fn == primary->function)
                        continue;
                    extra++;
                    debug_printf("[ATA] Extra IDE controller %02x:%02x.%u "
                                 "(unused; driver addresses first only)\n",
                                 bus, dev, fn);
                }
            }
        }
    }
    return extra;
}

static void ata_discover_channels(void) {
    memset(g_ata_channels, 0, sizeof(g_ata_channels));

    pci_device_t ide;
    if (pci_find_device_by_class(PCI_CLASS_STORAGE, PCI_SUBCLASS_IDE, 0xFF, &ide) != 0) {
        debug_printf("[ATA] No PCI IDE controller; assuming ISA compat ports\n");
        ata_channels_from_compat();
        return;
    }

    uint8_t pif = ide.prog_if;
    uint8_t pci_intline = pci_config_read_byte(ide.bus, ide.device, ide.function, 0x3C);
    debug_printf("[ATA] PCI IDE %02x:%02x.%u ProgIF=0x%02x IntLine=%u\n",
                 ide.bus, ide.device, ide.function, pif, pci_intline);

    uint32_t extra = ata_count_extra_ide_controllers(&ide);
    if (extra) {
        kprintf("[ATA] %u further IDE controller(s) on this machine are not "
                "used — any disk on them will not appear\n", extra);
    }

    if (pif & 0x01) {
        uint32_t bar0 = pci_read_bar(&ide, 0) & ~0x3u;
        uint32_t bar1 = pci_read_bar(&ide, 1) & ~0x3u;
        g_ata_channels[0] = (AtaChannel){
            .present = bar0 != 0, .native_mode = true,
            .cmd_base = (uint16_t)bar0,
            .ctrl_reg = (uint16_t)(bar1 + 2),
            .bmide_base = 0,
            .irq_gsi = pci_intline,
        };
    } else {
        g_ata_channels[0] = (AtaChannel){
            .present = true, .native_mode = false,
            .cmd_base = 0x1F0, .ctrl_reg = 0x3F6, .bmide_base = 0, .irq_gsi = 14,
        };
    }

    if (pif & 0x04) {
        uint32_t bar2 = pci_read_bar(&ide, 2) & ~0x3u;
        uint32_t bar3 = pci_read_bar(&ide, 3) & ~0x3u;
        g_ata_channels[1] = (AtaChannel){
            .present = bar2 != 0, .native_mode = true,
            .cmd_base = (uint16_t)bar2,
            .ctrl_reg = (uint16_t)(bar3 + 2),
            .bmide_base = 0,
            .irq_gsi = pci_intline,
        };
    } else {
        g_ata_channels[1] = (AtaChannel){
            .present = true, .native_mode = false,
            .cmd_base = 0x170, .ctrl_reg = 0x376, .bmide_base = 0, .irq_gsi = 15,
        };
    }

    if (pif & 0x80) {
        uint32_t bar4 = pci_read_bar(&ide, 4) & ~0x3u;
        if (bar4) {
            g_ata_channels[0].bmide_base = (uint16_t)(bar4 + 0);
            g_ata_channels[1].bmide_base = (uint16_t)(bar4 + 8);
            if (pci_enable_bus_master(&ide) != 0) {
                debug_printf("[ATA] Failed to enable PCI bus master; "
                             "BMIDE will refuse to start, falling back to PIO\n");
                g_ata_channels[0].bmide_base = 0;
                g_ata_channels[1].bmide_base = 0;
            }
        }
    }
}

void ata_init(void) {
    debug_printf("[ATA] Initialising legacy ATA/IDE driver...\n");
    spinlock_init(&g_ata_lock);

    memset(g_ata_channels, 0, sizeof(g_ata_channels));
    memset(g_ata_devices, 0, sizeof(g_ata_devices));

    if (ahci_is_initialized()) {
        debug_printf("[ATA] AHCI active; legacy ATA stack is informational only\n");
        g_ata_devices[0].exists           = 1;
        g_ata_devices[0].channel          = 0;
        g_ata_devices[0].is_slave         = 0;
        g_ata_devices[0].lba48_supported  = 1;
        g_ata_devices[0].logical_sector_size = 512;
        g_ata_devices[0].total_sectors    = 0xFFFFFFFFFFFFULL;
        strncpy(g_ata_devices[0].model, "AHCI SATA Drive", 40);
        g_ata_devices[0].model[40] = '\0';
        return;
    }

    ata_discover_channels();
    debug_printf("[ATA] ch0: present=%u native=%u cmd=0x%04x ctrl=0x%04x irq=%u\n",
                 g_ata_channels[0].present, g_ata_channels[0].native_mode,
                 g_ata_channels[0].cmd_base, g_ata_channels[0].ctrl_reg,
                 g_ata_channels[0].irq_gsi);
    debug_printf("[ATA] ch1: present=%u native=%u cmd=0x%04x ctrl=0x%04x irq=%u\n",
                 g_ata_channels[1].present, g_ata_channels[1].native_mode,
                 g_ata_channels[1].cmd_base, g_ata_channels[1].ctrl_reg,
                 g_ata_channels[1].irq_gsi);

    for (uint8_t c = 0; c < ATA_CHANNEL_COUNT; c++) {
        ata_channel_soft_reset(c);
    }

    ata_async_init();

    for (uint8_t drv = 0; drv < ATA_DRIVE_COUNT; drv++) {
        if (!g_ata_channels[drive_channel(drv)].present) continue;
        ata_identify(drv, &g_ata_devices[drv]);
    }

    debug_printf("[ATA] Init complete (slots: HDD=%u%u%u%u ATAPI=%u%u%u%u "
                 "BMIDE ch0=%u ch1=%u)\n",
                 g_ata_devices[0].exists,   g_ata_devices[1].exists,
                 g_ata_devices[2].exists,   g_ata_devices[3].exists,
                 g_ata_devices[0].is_atapi, g_ata_devices[1].is_atapi,
                 g_ata_devices[2].is_atapi, g_ata_devices[3].is_atapi,
                 ata_async_usable(0) || ata_async_usable(1) ? 1 : 0,
                 ata_async_usable(2) || ata_async_usable(3) ? 1 : 0);
}

void ata_print_device_info(const ATADevice* device) {
    if (!device || (!device->exists && !device->is_atapi)) {
        kprintf("  Device does not exist\n");
        return;
    }
    if (device->is_atapi) {
        kprintf("  Channel: %u (%s) — ATAPI (CD/DVD), no block I/O\n",
                device->channel, device->is_slave ? "slave" : "master");
        return;
    }
    kprintf("  Channel: %u (%s)\n", device->channel,
            device->is_slave ? "slave" : "master");
    kprintf("  Model:   %s\n", device->model);
    kprintf("  Serial:  %s\n", device->serial);
    const char* dma_name = (device->dma_type == ATA_DMA_TYPE_UDMA) ? "UDMA"
                         : (device->dma_type == ATA_DMA_TYPE_MDMA) ? "MDMA"
                         : "none";
    kprintf("  Size:    %lu MB (%lu sectors, LBA%s, PIO mode %u, DMA %s%u)\n",
            device->size_mb, (unsigned long)device->total_sectors,
            device->lba48_supported ? "48" : "28",
            device->pio_mode,
            dma_name,
            (device->dma_type == ATA_DMA_TYPE_NONE) ? 0 : device->dma_mode);
}