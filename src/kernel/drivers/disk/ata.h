#ifndef ATA_H
#define ATA_H

#include "ktypes.h"
#include "boxos_limits.h"

/* ===========================================================================
 *  ATA / IDE driver
 *
 *  Supports the four standard PATA drive slots a PCI IDE controller can
 *  expose (primary master, primary slave, secondary master, secondary slave),
 *  in either ISA-compatibility mode (legacy ports 0x1F0/0x3F6 & 0x170/0x376
 *  with IRQ14/IRQ15) or PCI-native mode (channel bases from BAR0..BAR3 and
 *  the IRQ from the PCI Interrupt Line at config offset 0x3C).
 *
 *  Operating mode: PIO only. nIEN=1 keeps the drive's IRQ line silent
 *  and we busy-poll STATUS; the IOAPIC pins for IRQ14/15 are never
 *  unmasked. PIO transfer mode is auto-negotiated to the highest the
 *  drive advertises (preferring PIO mode 4, falling back to mode 3,
 *  then drive default) via SET FEATURES 0xEF/0x03 per ATA-7 §7.41.
 *
 *  References:
 *    - PCI IDE Controller Specification Rev 1.0 (PIF byte at config 0x09)
 *    - Programming Interface for Bus Master IDE Controller Rev 1.0
 *      (Intel, May 1994) §2.1, §2.2
 *    - ATA8-ACS / T13 1699-D (IDENTIFY data layout, command codes)
 *    - ATA-7 §6.20 (ATAPI signature), §7.41 (SET FEATURES),
 *      §9.2 (software reset)
 *
 *  Drive index convention:
 *    0 = channel 0 (primary)   master
 *    1 = channel 0 (primary)   slave
 *    2 = channel 1 (secondary) master
 *    3 = channel 1 (secondary) slave
 * =========================================================================*/

#define ATA_CHANNEL_COUNT       2
#define ATA_DRIVE_COUNT         4   /* 2 channels × 2 drives each */

/* IDENTIFY / DATA commands (T13 ATA8-ACS Table 51). */
#define ATA_CMD_READ_SECTORS        0x20
#define ATA_CMD_WRITE_SECTORS       0x30
#define ATA_CMD_READ_SECTORS_EXT    0x24    /* 48-bit LBA */
#define ATA_CMD_WRITE_SECTORS_EXT   0x34    /* 48-bit LBA */
#define ATA_CMD_READ_DMA            0xC8
#define ATA_CMD_WRITE_DMA           0xCA
#define ATA_CMD_READ_DMA_EXT        0x25    /* 48-bit LBA */
#define ATA_CMD_WRITE_DMA_EXT       0x35    /* 48-bit LBA */
#define ATA_CMD_IDENTIFY            0xEC
#define ATA_CMD_IDENTIFY_PACKET     0xA1
#define ATA_CMD_CACHE_FLUSH         0xE7
#define ATA_CMD_CACHE_FLUSH_EXT     0xEA    /* 48-bit LBA */
#define ATA_CMD_SET_FEATURES        0xEF    /* set transfer mode etc. */

/* SET FEATURES subcommands (ATA-7 §7.41.5). */
#define ATA_SF_SET_XFER_MODE        0x03

/* DMA transfer kind (ATADevice.dma_type). */
typedef enum {
    ATA_DMA_TYPE_NONE = 0,
    ATA_DMA_TYPE_MDMA = 1,     /* Multiword DMA — SECCOUNT 0x20 | mode */
    ATA_DMA_TYPE_UDMA = 2,     /* Ultra DMA       — SECCOUNT 0x40 | mode */
} AtaDmaType;

/* Status register bits (ATA8-ACS Table 23). */
#define ATA_SR_BSY              0x80
#define ATA_SR_DRDY             0x40
#define ATA_SR_DF               0x20
#define ATA_SR_DSC              0x10
#define ATA_SR_DRQ              0x08
#define ATA_SR_CORR             0x04
#define ATA_SR_IDX              0x02
#define ATA_SR_ERR              0x01

/* Drive/Head register top nibbles for IDENTIFY (compat 28-bit decoded). */
#define ATA_DRIVE_MASTER        0xA0
#define ATA_DRIVE_SLAVE         0xB0

/* Public return codes (negative = error). */
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

/* A physical IDE channel (primary or secondary). Bases come from PCI: in
 * compatibility mode they are the ISA-fixed ports, in PCI-native mode
 * they come from BAR0..BAR3 of the controller. */
typedef struct {
    bool        present;
    bool        native_mode;
    uint16_t    cmd_base;        /* 8-byte command block (data/error/seccnt/LBA/drive/cmd) */
    uint16_t    ctrl_reg;        /* Device Control / Alternate Status register */
    uint16_t    bmide_base;      /* Bus-Master IDE channel base; 0 if BMIDE absent */
    uint8_t     irq_gsi;         /* GSI for completeness; not enabled (PIO uses polling) */
} AtaChannel;

/* A drive seen behind a channel. exists==1 once IDENTIFY succeeded as
 * an ATA HDD. is_atapi==1 means a CD/DVD or other packet device responded
 * to the bus; we label it but do not provide block I/O. */
typedef struct {
    uint8_t  exists;
    uint8_t  is_atapi;
    uint8_t  channel;            /* 0 = primary, 1 = secondary */
    uint8_t  is_slave;           /* 0 = master, 1 = slave */
    uint8_t  lba48_supported;
    uint8_t  pio_mode;           /* 0..4; 0 = default, no negotiation */
    uint8_t  dma_type;           /* AtaDmaType */
    uint8_t  dma_mode;           /* 0..6; meaning depends on dma_type */
    uint32_t logical_sector_size;
    uint64_t total_sectors;
    uint64_t size_mb;
    char     model[41];
    char     serial[21];
} ATADevice;

extern AtaChannel g_ata_channels[ATA_CHANNEL_COUNT];
extern ATADevice  g_ata_devices[ATA_DRIVE_COUNT];

/* Backwards-compat aliases for callers (debug printers, tests) that
 * referenced specific named slots in the pre-channel-abstraction code.
 * New code should key off the drive_idx 0..3. */
#define ata_primary_master   (g_ata_devices[0])
#define ata_primary_slave    (g_ata_devices[1])

void ata_init(void);
int  ata_identify(uint8_t drive_idx, ATADevice* device);

int  ata_read_sectors(uint8_t drive_idx, uint64_t lba, uint16_t count, uint8_t* buffer);
int  ata_write_sectors(uint8_t drive_idx, uint64_t lba, uint16_t count, const uint8_t* buffer);
int  ata_flush_cache(uint8_t drive_idx);

int  ata_read_sectors_retry(uint8_t drive_idx, uint64_t lba, uint16_t count, uint8_t* buffer);
int  ata_write_sectors_retry(uint8_t drive_idx, uint64_t lba, uint16_t count, const uint8_t* buffer);

void ata_print_device_info(const ATADevice* device);

/* Exposed for ata_async.c's BMIDE submit path. ata_program_lba writes
 * the LBA-and-command portion of an issue; ata_set_xfer_mode performs
 * a SET FEATURES "set transfer mode" (also reused by ata_negotiate_pio
 * in ata.c). Both expect the caller to have already selected the drive
 * and to be holding any required channel-wide serialisation. */
int  ata_program_lba(uint8_t drive_idx, uint64_t lba, uint16_t count,
                     uint8_t cmd28, uint8_t cmd48);
int  ata_set_xfer_mode(uint8_t drive_idx, uint8_t mode_byte);

/* ATA-7 §9.2 software reset of one channel (SRST pulse with nIEN held =1
 * throughout, so the drive's INTRQ is silenced before/during/after). Exposed
 * for the BMIDE watchdog's K-Core recovery path (ata_recover_worker); busy-
 * waits BSY up to 2 s, so it is only ever called off IRQ/PIT context. */
void ata_channel_soft_reset(uint8_t channel);

#endif /* ATA_H */
