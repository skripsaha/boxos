#ifndef ATA_H
#define ATA_H

#include "ktypes.h"
#include "boxos_limits.h"
#include "io.h"

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
/* The channel's async queue is full — back-pressure, not a broken drive.
 * It exists so the DMA fast path can tell "this transfer has nowhere to go
 * right now" apart from "this drive's DMA does not work", which is the one
 * verdict ata_read_sectors / ata_write_sectors act on permanently. Never
 * escapes the driver: both entry points consume it by taking the PIO path. */
#define ATA_ERR_BUSY              -10

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
    /* What the drive is built from — see the same field in ahci.h. Zero when
     * the drive would not say; equal to the logical size when it says one
     * physical block is one logical sector. */
    uint32_t physical_sector_size;
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

/*
 * The largest run of sectors this drive takes in one command.
 *
 * ‼ AND IT IS SMALL ON PURPOSE. ata_read_sectors takes the bus-master DMA path
 * only while the count is within ATA_ASYNC_MAX_SECTORS; one sector past that
 * and the command falls back to programmed I/O, where the processor moves
 * every word itself. So the honest answer for a drive whose DMA path is up is
 * that path's width — asking for more is not a bigger bite, it is the same
 * bytes moved the slow way.
 *
 * A drive with no usable DMA path is in PIO whatever it answers, and there a
 * longer run is a real saving: one command's handshake instead of eight.
 */
uint32_t ata_max_run_sectors(uint8_t drive_idx);

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

/* The 400 ns settle, in one place instead of three.
 *
 * ATA8-ACS §6.2.1 requires 400 ns between selecting a device and believing
 * anything the Status register says, and the idiom that satisfies it is four
 * reads of Alt-Status. The reason four reads are ENOUGH is architectural, not
 * hopeful: an x86 `IN` is a NON-POSTED bus transaction — the core stalls until
 * the read completes — and the legacy control port decodes over DMI or
 * LPC/eSPI, where a single cycle costs on the order of 400 ns at the floor.
 * Four of them therefore clear 400 ns with margin on every chipset this kernel
 * targets, and they cost nothing when the port is fast because the clause is
 * about the DEVICE's settling time, not the host's.
 *
 * The reads are not optional and must not be optimised away — hence the
 * `(void)inb(...)`, which is a volatile asm read, and hence this living in a
 * header rather than being written out a fourth time. */
static inline void ata_settle_400ns(uint16_t ctrl_reg) {
    for (int i = 0; i < 4; i++) (void)inb(ctrl_reg);
}

/* How long the driver waits for BSY to fall after it releases SRST.
 *
 * ATA/ATAPI-6 (T13/1410D r3a) §9.2, device state D0SR3:Check_Status, gives the
 * device up to 31 s to finish its internal reset. That is the SPEC quantity,
 * and the boot-time reset gets it in full: nothing else is happening during
 * ata_init, and the only alternative to waiting is declaring a drive that is
 * legitimately recovering to be absent.
 *
 * The other two callers get less, and the reason is the CALLER, not the drive.
 * ata_flush_cache resets the channel from inside g_ata_lock, and spin_lock in
 * this kernel is `pushfq; pop; cli` — so the spec's patience there would be
 * thirty-one seconds with interrupts disabled on that core, which is a far
 * worse failure than mis-declaring a wedged drive. ata_recover_worker runs on
 * a K-Core pump shared with every other deferred kernel job and would starve
 * it for exactly as long. Both take the DECLARED POLICY below — two seconds,
 * the value this function used to apply to everybody.
 *
 * ‼ That is a statement about the RESET, which is recovery, and not about the
 * command being recovered. The flush itself is owed what §8.13.8 warns of
 * ("may take longer than 30 s to complete"), it is paid in full from
 * CONFIG_ATA_IO_TIMEOUT_MS, and ata_flush_cache spends that budget ONCE across
 * all of its attempts rather than per attempt — a flush abandoned early is a
 * durability barrier that did not happen, which is the failure this driver
 * exists to make impossible. What keeps a wait that long legal while IF is
 * zero is not its length but ata_poll_pause (ata.c): every poll loop in this
 * driver services a cross-core TLB shootdown for its own core inline, exactly
 * as spin_lock's contended path does, so no initiator is ever left waiting on
 * an ACK this core cannot send. */
#define ATA_SRST_BSY_SPEC_MS   31000u
#define ATA_SRST_BSY_HELD_MS    2000u

/* ATA-7 §9.2 software reset of one channel (SRST pulse with nIEN held =1
 * throughout, so the drive's INTRQ is silenced before/during/after). Exposed
 * for the BMIDE watchdog's K-Core recovery path (ata_recover_worker).
 *
 * `bsy_patience_ms` is how long the caller can afford to wait for BSY to fall
 * — ATA_SRST_BSY_SPEC_MS when nothing is held, ATA_SRST_BSY_HELD_MS when a
 * lock or a K-Core is. The loop still exits on the FACTS first: a status of 0
 * or 0xFF means nothing is on the channel, and BSY clear means ready. */
void ata_channel_soft_reset(uint8_t channel, uint32_t bsy_patience_ms);

#endif /* ATA_H */
