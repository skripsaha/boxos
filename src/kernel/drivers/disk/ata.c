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
#include "hourglass.h"

/* ===========================================================================
 *  Real-HW PATA driver (PIO transfer mode).
 *
 *  Channel discovery comes from PCI: any device with class/subclass 01/01h
 *  is a PCI IDE controller. Its Programming Interface byte (PIF, config 0x09)
 *  per "PCI IDE Controller Specification" §2.1 has:
 *
 *      bit 0   primary native-mode (1) vs compatibility (0)
 *      bit 1   primary native-mode supported AND switchable
 *      bit 2   secondary native-mode (1) vs compatibility (0)
 *      bit 3   secondary native-mode supported AND switchable
 *      bit 7   Bus Master IDE capable (BMIDE at BAR4)
 *
 *  In compatibility mode the I/O bases are ISA-fixed:
 *      primary:   command 0x1F0-0x1F7, control 0x3F6, IRQ14
 *      secondary: command 0x170-0x177, control 0x376, IRQ15
 *
 *  In native mode the I/O bases come from PCI BARs:
 *      BAR0 -> primary   command block (8 ports)
 *      BAR1 -> primary   control block (4 ports; Device Control / Alt
 *                        Status at offset 2)
 *      BAR2 -> secondary command block
 *      BAR3 -> secondary control block (DCR at offset 2)
 *  and the IRQ comes from PCI config Interrupt Line (offset 0x3C);
 *  both channels typically share the same INTx# pin.
 *
 *  IRQ posture: nIEN=1 on every detected drive + IOAPIC pins for
 *  IRQ14/15 left masked. Both PIO and Bus-Master DMA paths busy-poll
 *  completion — fully spec-compliant without an interrupt path. PIO
 *  mode is negotiated to the fastest the drive advertises (4 → 3 →
 *  default); DMA mode similarly (UDMA 6..0 → MDMA 2..0 → none), except
 *  that Ultra DMA above mode 2 needs an 80-conductor cable established
 *  first — ATA/ATAPI-6 §8.46.11 — or the walk starts at mode 2.
 *  Read/write fast path runs through BMIDE when both the controller
 *  (ProgIF bit 7) and the drive (IDENTIFY word 49 bit 8) support it
 *  and the request fits the staging page (count ≤ 8 sectors); larger
 *  or DMA-failing requests transparently fall back to PIO.
 * =========================================================================*/

AtaChannel g_ata_channels[ATA_CHANNEL_COUNT];
ATADevice  g_ata_devices[ATA_DRIVE_COUNT];

/* Global IDE-controller lock. Serialises command issue across all four
 * drives behind a single ATA controller — the legacy register file is
 * shared so two cores cannot drive different channels concurrently
 * without a re-entrancy story we don't need. */
static spinlock_t g_ata_lock;

/* ATA register offsets within an 8-byte command block (PCI IDE native
 * layout, identical to legacy 0x1F0+N). */
#define ATA_REG_DATA            0
#define ATA_REG_ERROR           1   /* read */
#define ATA_REG_FEATURES        1   /* write */
#define ATA_REG_SECCOUNT        2
#define ATA_REG_LBA_LO          3
#define ATA_REG_LBA_MID         4
#define ATA_REG_LBA_HI          5
#define ATA_REG_DRIVE_HEAD      6
#define ATA_REG_STATUS          7   /* read */
#define ATA_REG_COMMAND         7   /* write */

#define ATA_CTRL_SRST           0x04 /* Device Control register, bit 2 */
#define ATA_CTRL_nIEN           0x02 /* Device Control register, bit 1 */

/* ---------------------------------------------------------------------
 *  Helpers
 * ------------------------------------------------------------------ */

static inline uint8_t drive_channel(uint8_t drive_idx) { return drive_idx >> 1; }
static inline uint8_t drive_is_slave(uint8_t drive_idx) { return drive_idx & 1; }
static inline AtaChannel* drive_channel_ptr(uint8_t drive_idx) {
    return &g_ata_channels[drive_channel(drive_idx)];
}

/* ATA8-ACS §6.2.1's 400 ns after a drive-select. The idiom and the reason four
 * reads satisfy it live once, in ata.h (ata_settle_400ns). */
static inline void ata_delay_400ns(uint8_t drive_idx) {
    ata_settle_400ns(drive_channel_ptr(drive_idx)->ctrl_reg);
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

/* One turn of any busy-poll loop in this driver.
 *
 * Every wait below turns while this core holds g_ata_lock, and spin_lock in
 * this kernel is `pushfq; pop; cli` — so these loops run with INTERRUPTS OFF,
 * for as long as the drive takes. That is not a number this driver gets to
 * choose: ATA/ATAPI-6 (T13/1410D r3a) §8.13.8 says a cache flush alone "may
 * take longer than 30 s to complete", and a command abandoned early is a
 * command whose outcome nobody knows.
 *
 * Exactly one thing in this kernel cannot wait that long for this core: a
 * cross-core TLB shootdown. Its initiator holds every other core to an ACK and
 * calls a missing one a deadlock after CONFIG_TLB_SHOOTDOWN_PANIC_MS — and the
 * ACK cannot arrive here as an IPI, because IF is zero.
 *
 * So the wait answers it by hand, which is what spin_lock's contended path
 * already does with this same function (vmm registers it as klib's wait-service
 * hook). vmm_tlb_shootdown_poll is the idempotent, generation-gated, lock-free
 * handler the IPI vector runs: a no-op when nothing is pending for this core,
 * and a no-op again when the real IPI lands afterwards. THIS is what makes a
 * long ATA wait legal on a multi-core machine — not a shorter timeout, which
 * would only trade a panic for a command given up on. */
static inline void ata_poll_pause(void) {
    vmm_tlb_shootdown_poll();
    cpu_pause();
}

/* Bounded waits — every register handshake must terminate even if the
 * drive is dead. CONFIG_ATA_TIMEOUT_MS comes from kernel_config.h. */
static int ata_wait_clear_bsy(uint8_t drive_idx, uint32_t timeout_ms) {
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    while (rdtsc() < deadline) {
        uint8_t s = ata_read_status(drive_idx);
        if (s == 0xFF) return ATA_ERR_NO_DEVICE;
        if (!(s & ATA_SR_BSY)) return 0;
        ata_poll_pause();
    }
    return ATA_ERR_TIMEOUT;
}

static int ata_wait_ready(uint8_t drive_idx) {
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(CONFIG_ATA_TIMEOUT_MS);
    while (rdtsc() < deadline) {
        uint8_t s = ata_read_status(drive_idx);
        if (s == 0xFF) return ATA_ERR_NO_DEVICE;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRDY)) return 0;
        ata_poll_pause();
    }
    return ATA_ERR_TIMEOUT;
}

/*
 * The device is in a state to be GIVEN a command — all of what ATA/ATAPI-6
 * (T13/1410D r3a) requires before the Command register is written, rather than
 * the part of it ata_wait_ready covers.
 *
 * §9.4 host state HI1:Check_Status is where a host sits before issuing:
 * "Transition HI1:HI1: When the status read indicates that either BSY or DRQ
 * is set to one, the host shall make a transition to the HI1: Check_Status
 * state to recheck the status of the selected device", and only "when the
 * status read indicates that both BSY and DRQ are cleared to zero and the
 * correct device is selected" does it move on to write parameters (HI3). On
 * top of that §8.13.7 Prerequisites for FLUSH CACHE EXT reads, in full,
 * "DRDY set to one."
 *
 * DRQ is the half ata_wait_ready does not test, and it is the half that
 * matters after a reset: ata_channel_soft_reset returns the instant BSY falls
 * (or the instant the port reads 0) and never waits for DRDY at all, so an
 * attempt issued straight after one can otherwise land on a device that is
 * still coming back.
 */
static int ata_wait_command_ready(uint8_t drive_idx, uint32_t timeout_ms) {
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(timeout_ms);
    while (rdtsc() < deadline) {
        uint8_t s = ata_read_status(drive_idx);
        if (s == 0xFF) return ATA_ERR_NO_DEVICE;
        if (!(s & (ATA_SR_BSY | ATA_SR_DRQ)) && (s & ATA_SR_DRDY)) return 0;
        ata_poll_pause();
    }
    return ATA_ERR_TIMEOUT;
}

/*
 * The drive has been given a command and this waits for it to carry it out, so
 * the patience is the DRIVE'S rather than a register handshake's — see
 * CONFIG_ATA_IO_TIMEOUT_MS. Three facts end it before the clock ever does: the
 * bus reading 0xFF, which is nothing being there at all; the drive raising
 * ERR; and the drive raising DRQ, which is the answer.
 */
static int ata_wait_drq(uint8_t drive_idx) {
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(CONFIG_ATA_IO_TIMEOUT_MS);
    while (rdtsc() < deadline) {
        uint8_t s = ata_read_status(drive_idx);
        if (s == 0xFF) return ATA_ERR_NO_DEVICE;
        if (s & ATA_SR_ERR) return ATA_ERR_DRIVE_FAULT;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return 0;
        ata_poll_pause();
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
    /* T13: model & serial strings are byte-swapped per 16-bit word. */
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

/* ===========================================================================
 *  BMIDE engine, IRQ-driven DMA submit/wait, DMA-mode negotiation and
 *  every other BMIDE register touch lives in ata_async.{c,h}. ata.c
 *  retains only PIO + IDENTIFY + channel discovery + the legacy sync
 *  read/write entry points, which call into ata_async via ata_dma_sync.
 * =========================================================================*/

/* Submit a SET FEATURES "set transfer mode" command. Mode byte format
 * (ATA-7 §7.41.5):
 *    0x00 + n            -> PIO mode 0 (n must be 0)
 *    0x08 | n            -> PIO mode n (0..4 in practice)
 *    0x20 | n            -> Multiword DMA mode n (0..2)
 *    0x40 | n            -> Ultra DMA mode n (0..6)
 * Returns 0 on success, negative on drive ABORT or timeout. */
int ata_set_xfer_mode(uint8_t drive_idx, uint8_t mode_byte) {
    AtaChannel* ch = drive_channel_ptr(drive_idx);
    ata_select_drive(drive_idx);
    if (ata_wait_clear_bsy(drive_idx, CONFIG_ATA_TIMEOUT_MS) != 0)
        return ATA_ERR_TIMEOUT;

    outb(ch->cmd_base + ATA_REG_FEATURES, ATA_SF_SET_XFER_MODE);
    outb(ch->cmd_base + ATA_REG_SECCOUNT, mode_byte);
    outb(ch->cmd_base + ATA_REG_COMMAND,  ATA_CMD_SET_FEATURES);

    /*
     * The settle is what makes every line below it MEAN anything.
     *
     * SET FEATURES is a non-data command, so ATA/ATAPI-6 (T13/1410D r3a) §9.4
     * governs it twice over. Device state DND0:Command_Execution gives the
     * drive up to 400 ns to raise BSY at all — "the device shall set BSY to
     * one within 400 ns of the writing of the Command register" — and host
     * state HND1:Check_Status spends exactly that on the other side: "When
     * entering this state from another state other than when an interrupt has
     * occurred, the host shall wait 400 ns before reading the Status
     * register."
     *
     * Without it the first status read can still be the drive's PRE-command
     * idle taskfile, which is BSY=0 with ERR clear — so ata_wait_clear_bsy
     * returns on a byte the command never touched and the ERR test below
     * passes on that same stale byte. A mode the drive ABORTED is then
     * recorded as set: the boot log names a PIO or Ultra DMA mode nobody
     * proved, and ata_async_usable() opens the DMA path on the strength of it.
     * Worse, the function can return while the drive is still BSY, and the
     * very next ata_select_drive writes the Device register during BSY, which
     * §7.15.3 puts outside the register's defined behaviour: "The contents of
     * this register, except for BSY, shall be ignored when BSY is set to one."
     *
     * With it, BSY is up before the first read, ata_wait_clear_bsy waits for
     * the command to actually end, and the ERR test below reads the ENDING
     * status — the one §8.46.6 puts the abort in.
     */
    ata_delay_400ns(drive_idx);

    if (ata_wait_clear_bsy(drive_idx, CONFIG_ATA_TIMEOUT_MS) != 0)
        return ATA_ERR_TIMEOUT;
    if (ata_read_status(drive_idx) & ATA_SR_ERR) return ATA_ERR_NOT_SUPPORTED;
    return 0;
}

/* The minimum hold on SRST, spent as time instead of counted as reads.
 *
 * ATA/ATAPI-6 (T13/1410D r3a) §9.2, host state HSR0:Set_SRST — "The host shall
 * remain in this state with SRST set to one for at least 5 us." That is a SPEC
 * quantity and it is five microseconds of real time.
 *
 * Eight Alt-Status reads stood here. Eight reads is 3.2 us at the ~400 ns
 * LPC/eSPI floor that ata_settle_400ns rests on — SHORT of the clause, and
 * short in the way that is hardest to recognise afterwards, because a reset
 * that half-takes leaves a drive that mostly works.
 *
 * ATA_SRST_HOLD_READS is the fallback for a machine with no time ruler at all,
 * and it is not a guess either: it is the SAME architectural floor, counted
 * out. An x86 `IN` is a non-posted bus transaction, so the core stalls until
 * the legacy port's cycle completes, and 5 us / 400 ns = 12.5 — thirteen reads
 * clear the clause. A missing ruler never becomes an invented one; it becomes
 * the one mechanism that is still provable without a clock. */
#define ATA_SRST_HOLD_US      5u
#define ATA_SRST_HOLD_READS  13

static void ata_hold_srst(uint16_t ctrl_reg) {
    HourGlass glass;
    if (!HourGlassTurn(&glass, ATA_SRST_HOLD_US)) {
        kprintf("[ATA] no time source — holding SRST with %d non-posted port "
                "reads instead\n", ATA_SRST_HOLD_READS);
        for (int i = 0; i < ATA_SRST_HOLD_READS; i++) (void)inb(ctrl_reg);
        return;
    }
    while (!HourGlassRunOut(&glass)) cpu_pause();
}

/* The OTHER half of the same clause: the 2 ms the host owes after it RELEASES
 * SRST, before it is allowed to look at the drive at all.
 *
 * ATA/ATAPI-6 (T13/1410D r3a) §9.2, host state HSR1:Clear_wait — "When in
 * this state, the host shall clear SRST in the Device Control register to
 * zero. The host shall remain in this state for at least 2 ms", and transition
 * HSR1:HSR2 — "When SRST has been cleared to zero for at least 2 ms, the host
 * shall make a transition to the HSR2: Check_status state." HSR2 is the FIRST
 * state in which "the host shall read the Status or Alternate Status
 * register"; there is no earlier state in which a Status byte means anything.
 *
 * Four Alt-Status reads stood here — ata_settle_400ns, the settle owed after a
 * drive SELECT, borrowed for a clause three orders of magnitude longer. The
 * consequence was not a slightly-early read: it was that the single sample
 * taken at ~1.6 µs was TERMINAL on two byte values, so a 0x00 or 0xFF read from
 * a register file still in its reset protocol ended the wait with "no drives
 * on this channel" and no line in the log. Spending the clause HERE, before
 * the loop, is what makes that exit honest again: every sample the loop takes
 * is now one the standard says is worth reading, and no single byte seen
 * before the 2 ms boundary can end anything.
 *
 * The no-ruler fallback is the same architectural floor ata_hold_srst counts
 * out, counted out for this clause instead: an x86 `IN` is a non-posted bus
 * transaction, a legacy-port cycle costs ~400 ns at the floor, and
 * 2 ms / 400 ns = 5000. Reading Alt-Status to spend it is free of side effects
 * by §7.3.4 — "Reading this register shall not clear a pending interrupt." A
 * missing ruler never becomes an invented one. */
#define ATA_SRST_CLEAR_WAIT_US   2000u
#define ATA_SRST_CLEAR_READS     5000

static void ata_srst_clear_reads(uint16_t ctrl_reg) {
    for (int i = 0; i < ATA_SRST_CLEAR_READS; i++) (void)inb(ctrl_reg);
}

static void ata_wait_srst_cleared(uint16_t ctrl_reg) {
    HourGlass glass;
    if (!HourGlassTurn(&glass, ATA_SRST_CLEAR_WAIT_US)) {
        kprintf("[ATA] no time source — spending the post-SRST 2 ms with %d "
                "non-posted port reads instead\n", ATA_SRST_CLEAR_READS);
        ata_srst_clear_reads(ctrl_reg);
        return;
    }
    while (!HourGlassRunOut(&glass)) ata_poll_pause();

    /* A glass that ran out because its RULER stopped, rather than because the
     * span ended, has not spent the clause — and a mandatory wait that only
     * looks spent is the defect this function exists to remove. Say which of
     * the two happened and spend the clause the way a machine with no ruler
     * spends it. */
    if (HourGlassSourceDied(&glass)) {
        kprintf("[ATA] %s died %lu us into the post-SRST 2 ms — finishing it "
                "with %d non-posted port reads\n",
                HourGlassRulerName(&glass),
                (unsigned long)HourGlassElapsedUs(&glass),
                ATA_SRST_CLEAR_READS);
        ata_srst_clear_reads(ctrl_reg);
    }
}

/* ---------------------------------------------------------------------
 *  Software reset — one per channel.
 *
 *  ATA/ATAPI-6 (T13/1410D r3a) §9.2: write SRST=1 (Device Control bit 2)
 *  plus nIEN=1, hold it for at least 5 us (HSR0), then write SRST=0 with
 *  nIEN=1 still asserted and remain there for at least 2 ms (HSR1) before the
 *  first Status read. BSY may then stay set for as long as D0SR3 allows;
 *  how long THIS caller can afford to wait for it arrives as
 *  `bsy_patience_ms`, because the answer depends on what the caller is
 *  holding and not on the drive (see ATA_SRST_BSY_* in ata.h).
 *
 *  Keeping nIEN=1 throughout means the drive's IRQ line is silent before,
 *  during, and after reset — there is no window in which a stale IRQ
 *  could escape to the IOAPIC.
 * ------------------------------------------------------------------ */
void ata_channel_soft_reset(uint8_t channel, uint32_t bsy_patience_ms) {
    AtaChannel* ch = &g_ata_channels[channel];
    if (!ch->present) return;

    outb(ch->ctrl_reg, ATA_CTRL_SRST | ATA_CTRL_nIEN);
    ata_hold_srst(ch->ctrl_reg);

    outb(ch->ctrl_reg, ATA_CTRL_nIEN);
    ata_wait_srst_cleared(ch->ctrl_reg);

    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(bsy_patience_ms);
    while (rdtsc() < deadline) {
        uint8_t s = inb(ch->cmd_base + ATA_REG_STATUS);
        if (s == 0 || s == 0xFF) return; /* no drives on this channel */
        if (!(s & ATA_SR_BSY)) return;   /* ready */
        ata_poll_pause();
    }
    /* kprintf, not debug_printf: a channel that never left BSY is a disk this
     * boot is about to pretend it does not have, and that sentence has to
     * survive into a shipped build. */
    kprintf("[ATA] channel %u still BSY %u ms after its reset — continuing "
            "without it\n", channel, bsy_patience_ms);
}

/* ---------------------------------------------------------------------
 *  PIO transfer mode negotiation (ATA-7 §7.41).
 *
 *  Drives power up in PIO mode 0 (3.3 MB/s). Modes 3 and 4 are
 *  supported by every PCI IDE controller built since the mid-90s
 *  (PIIX/ICH/SiS/VIA/AMD). Word 64 of IDENTIFY advertises support:
 *    bit 0 = PIO mode 3 supported
 *    bit 1 = PIO mode 4 supported
 *  SET FEATURES with subcmd 0x03 and SECCOUNT = 0x08 | mode picks the
 *  mode. PIO mode 4 = 0x0C, mode 3 = 0x0B. If the drive ABORTs with
 *  ERR, fall back to the next-lower mode; final fallback is "leave at
 *  power-up default" — slow but functionally correct.
 * ------------------------------------------------------------------ */
static void ata_negotiate_pio(uint8_t drive_idx, const uint16_t* id) {
    /* IDENTIFY word 64 bits 0..1 = PIO mode 3 / 4 advertised. Tries 4
     * then 3 via SET FEATURES (SECCOUNT 0x08|mode); leaves drive in
     * power-up default (mode 0) when neither is supported. */
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

/* ---------------------------------------------------------------------
 *  IDENTIFY — spec-ordered ATA/ATAPI discrimination.
 *
 *  ATA-7 §6.20:
 *    1. Select drive, wait 400 ns.
 *    2. Issue IDENTIFY (0xEC).
 *    3. Wait BSY clear (or no-drive sentinel 0/0xFF).
 *    4. Read STATUS:
 *         ERR set    -> read LBA_MID/HI. 0x14/0xEB = ATAPI device:
 *                       label as ATAPI, do not enumerate as HDD.
 *         ERR clear  -> read LBA_MID/HI. Nonzero -> SATA shim or
 *                       other non-ATA device; skip. Zero -> proceed.
 *    5. Wait DRQ set.
 *    6. Read 256 16-bit words of IDENTIFY data.
 * ------------------------------------------------------------------ */
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
        /* ATAPI signature: 0x14/0xEB. Mark slot informationally so the
         * boot log accurately reflects what is on the bus, but leave
         * exists=0 — we do not provide block I/O on packet devices. */
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

    /* Logical sector size (ATA8-ACS §7.16.7.74-75): word 106 bit 14=1 +
     * bit 15=0 marks the field valid; bit 12 = logical sector > 256
     * words. words 117-118 then hold the size in 16-bit words. The PIO
     * transfer loop uses 256-word (512-byte) bursts, so refuse anything
     * else rather than silently corrupt LBA arithmetic. */
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

    /* Word 106 again: bit 13 marks a drive with more than one logical sector
     * per physical, and bits 3:0 are the exponent. A 512e drive says 512 and
     * 4096 here, and a volume that was laid out for the smaller of the two
     * wears it twice as fast for every metadata write. */
    device->physical_sector_size = logical;
    if ((id[106] & (1u << 14)) && !(id[106] & (1u << 15)) &&
        (id[106] & (1u << 13))) {
        uint32_t exponent = id[106] & 0x0Fu;
        if (exponent < 16 && logical <= (0xFFFFFFFFu >> exponent)) {
            device->physical_sector_size = logical << exponent;
        }
    }

    /* How big the medium is, asked of the words the standard says are worth
     * reading and of no others.
     *
     * Word 83 bit 10 is the whole answer to "does this device do LBA48" —
     * ATA/ATAPI-6 (T13/1410D r3a) §8.15.42, "If bit 10 of word 83 is set to
     * one, the 48-bit Address feature set is supported" — but only once the
     * word is VALID, which the same section decides two paragraphs earlier:
     * "If bit 14 of word 83 is set to one and bit 15 of word 83 is cleared to
     * zero, the contents of words (83:82) contain valid support information.
     * If not, support information is not valid in these words."
     *
     * Bit 10 alone was read here. On a device that leaves words 82-84
     * unimplemented and floats them to FFFFh — a CompactFlash card in True
     * IDE mode, a DOM, a PATA-bridge board — bits (15:14) read 11b, so the
     * word means nothing, and bit 10 within it reads as one all the same. The
     * capacity then came from words (103:100), FFFFh apiece on the same
     * device, and this driver seated a disk of 2^64 sectors: the bounds checks
     * in ata_read_sectors and ata_write_sectors can never refuse an LBA past
     * the end of the medium after that, and the seat the Boardroom publishes
     * from it is what TagFS sizes the volume's ground against.
     *
     * The bounds are the standard's too, and a value outside them is a field
     * this device did not fill in rather than a number to carry forward.
     * §8.15.52: words (103:100) "contain a value that is one greater than the
     * maximum LBA in used addressable space... The maximum value that shall be
     * placed in this field is 0000FFFFFFFFFFFFh." §6.2.1 item 1, for the LBA28
     * pair: words (61:60) "shall not exceed 0FFFFFFFh. The content of words
     * (61:60) shall be greater than or equal to one and less than or equal to
     * 268,435,455."
     *
     * The zero test on words (103:100) is NOT the standard's — §6.2.1 item 4
     * says the opposite in letter, "The contents of words (61:60) and
     * (103:100) shall not be used to determine if 48-bit addressing is
     * supported" — it is Linux's deliberate real-hardware deviation
     * (ata_id_has_lba48 in include/linux/ata.h), written for drives that
     * assert bit 10 and report a 48-bit capacity of zero. Without it such a
     * drive is seated with total_sectors = 0 and every I/O on it, down to
     * Boardroom's one-sector probe, is refused as out of bounds; with it the
     * drive keeps the LBA28 capacity it does report, which is what Linux
     * mounts it on. */
    #define ATA_LBA48_MAX_SECTORS  0x0000FFFFFFFFFFFFULL
    #define ATA_LBA28_MAX_SECTORS  0x0FFFFFFFUL

    uint64_t lba48 = 0;
    if ((id[83] & 0xC000u) == 0x4000u && (id[83] & (1u << 10))) {
        memcpy(&lba48, &id[100], sizeof(uint64_t));
        if (lba48 > ATA_LBA48_MAX_SECTORS) lba48 = 0;
    }

    if (lba48 != 0) {
        device->lba48_supported = 1;
        device->total_sectors   = lba48;
    } else {
        uint32_t lba28 = 0;
        memcpy(&lba28, &id[60], sizeof(uint32_t));
        if (lba28 > ATA_LBA28_MAX_SECTORS) lba28 = 0;
        device->lba48_supported = 0;
        device->total_sectors   = lba28;
    }

    if (device->total_sectors == 0) {
        /* kprintf, not debug_printf: this is a drive that answered IDENTIFY
         * and is still not being used, and the reason has to reach a shipped
         * build's console — otherwise the board simply comes up one disk
         * short with nothing said. */
        kprintf("[ATA] drive %u: IDENTIFY gives no usable capacity "
                "(word83=0x%04x, words 103:100 and 61:60 both outside the "
                "ranges ATA/ATAPI-6 §6.2.1 defines) — not using it\n",
                drive_idx, id[83]);
        return ATA_ERR_NO_DEVICE;
    }

    device->size_mb = device->total_sectors / 2048;

    memcpy(device->model, &id[27], 40);
    ata_string_fixup(device->model, 40);
    memcpy(device->serial, &id[10], 20);
    ata_string_fixup(device->serial, 20);

    device->exists = 1;

    /* Negotiate PIO mode (used for fallback) and DMA mode (used when
     * BMIDE is enabled on this channel). exists=1 is published first
     * so the SET FEATURES helpers' ata_select_drive() does not skip
     * the slot. */
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

/* ---------------------------------------------------------------------
 *  Read / Write sectors
 * ------------------------------------------------------------------ */
/* Non-static — also invoked from ata_async.c::bmide_kick to program the
 * drive side of a DMA submit. */
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
        /* HOB pair-write per T13 ATA-8 §7.59.4. */
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

/* AHCI-active routing rule: when AHCI is initialised the SATA stack
 * is the only correct entry point for block I/O. Direct callers of
 * the legacy ata_*_sectors API are a misuse — they have no way to
 * resolve drive_idx to an AHCI port. We refuse cleanly so a regression
 * in dispatch logic is loud, not silently corrupting. */
static inline bool ata_legacy_path_active(void) {
    return !ahci_is_initialized();
}

/*
 * A drive that fell back from DMA to PIO — DEMOTED once, said once.
 *
 * The demotion is the substance and the line is the receipt, in that order.
 * This used to be the receipt alone: dma_type stayed whatever negotiation put
 * there, ata_async_usable() therefore stayed true, and every later transfer
 * paid a full failed DMA round trip — a wedged engine on a K-Core costs an
 * SRST of the whole channel per 4 KB — before the PIO path underneath rescued
 * it. The drive did not "run in PIO from here"; it ran in PIO after a toll,
 * forever, while the log said the matter was settled.
 *
 * Clearing dma_type is the one write that makes the sentence true, because it
 * is what ata_async_usable() reads (ata_async.c) and therefore what the DMA
 * gate in ata_read_sectors / ata_write_sectors reads. It also delivers the
 * contract ata.h states for ata_max_run_sectors and could never keep: "A drive
 * with no usable DMA path is in PIO whatever it answers, and there a longer
 * run is a real saving" — the 128-sector PIO branch was unreachable while
 * dma_type never changed, so Boardroom kept chopping PIO traffic into runs
 * sized for a DMA path that was not working.
 *
 * dma_mode goes with it: a mode number belonging to a transfer kind that is
 * now NONE is a number the boot log would still print.
 *
 * Only a verdict about THIS DRIVE demotes it. ATA_ERR_BUSY — the channel's
 * async queue full — is back-pressure and is filtered out by the two callers
 * before they get here, so a momentary queue does not cost a drive its DMA
 * path for the rest of the boot.
 *
 * The line itself stays once-per-drive: every transfer is not worth a line,
 * and no line at all is how a machine ends up running its whole disk in
 * programmed I/O with nobody aware of it, which on real hardware is the
 * difference between a disk and a bottleneck.
 */
static uint8_t g_ata_dma_fallback_said = 0;

static void ata_note_dma_fallback(uint8_t drive_idx, int rc, uint64_t lba,
                                  bool write)
{
    if (drive_idx >= ATA_DRIVE_COUNT) return;

    g_ata_devices[drive_idx].dma_type = ATA_DMA_TYPE_NONE;
    g_ata_devices[drive_idx].dma_mode = 0;

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

    /* Fast path: IRQ-driven Bus-Master DMA via ata_async. Run BEFORE
     * acquiring g_ata_lock — ata_dma_sync may sti;hlt while waiting for
     * the BMIDE IRQ, and that would violate the spinlock's IRQ-saved
     * state. The async engine has its own per-channel cmd_lock for
     * register-file mutual exclusion. */
    if (ata_async_usable(drive_idx) && count <= ATA_ASYNC_MAX_SECTORS) {
        int rc = ata_dma_sync(drive_idx, lba, count, false, buffer);
        if (rc == 0) return 0;
        /* ATA_ERR_BUSY is the channel's queue, not the drive's DMA — this
         * transfer goes the slow way, and the path keeps its reputation. */
        if (rc != ATA_ERR_BUSY) ata_note_dma_fallback(drive_idx, rc, lba, false);
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

    /* Refuse writes to the bootloader region (sectors 0..399) — the
     * MBR + stage2 live there; corrupting them bricks the install. */
    #define BOOTLOADER_PROTECTED_SECTORS 400
    if (lba < BOOTLOADER_PROTECTED_SECTORS) {
        debug_printf("[ATA] SECURITY: write to protected LBA %lu blocked\n",
                     (unsigned long)lba);
        return ATA_ERR_PROTECTED_SECTOR;
    }
    if (lba + count > d->total_sectors) return ATA_ERR_LBA_OUT_OF_BOUNDS;

    /* DMA fast path — same gate as ata_read_sectors. Run BEFORE acquiring
     * g_ata_lock so the sti;hlt in ata_dma_sync's wait loop doesn't
     * collide with the lock's saved-IRQ state. Per-channel cmd_lock in
     * ata_async covers register serialisation for the DMA path. */
    if (ata_async_usable(drive_idx) && count <= ATA_ASYNC_MAX_SECTORS) {
        int rc = ata_dma_sync(drive_idx, lba, count, true, (void*)buffer);
        if (rc == 0) return 0;
        /* ATA_ERR_BUSY is the channel's queue, not the drive's DMA — this
         * transfer goes the slow way, and the path keeps its reputation. */
        if (rc != ATA_ERR_BUSY) ata_note_dma_fallback(drive_idx, rc, lba, true);
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

    /* Protocol compliance: BSY must clear before the next command goes out,
     * and neither ERR nor DF may be set, otherwise the write is uncertain. */
    /* The write is on the medium when the drive stops being busy, so this is
     * the drive carrying a command out and not a handshake — the same patience
     * the read's DRQ wait uses. */
    if (ata_wait_clear_bsy(drive_idx, CONFIG_ATA_IO_TIMEOUT_MS) != 0) {
        debug_printf("[ATA] drv%u: BSY-clear timeout after WRITE @LBA %lu\n",
                     drive_idx, (unsigned long)lba);
        spin_unlock(&g_ata_lock);
        return ATA_ERR_TIMEOUT;
    }
    /* ERR and DF are two DIFFERENT ways the write ended badly, and ATA/ATAPI-6
     * (T13/1410D r3a) §8.62.6 and §8.63.6 name them separately in the error
     * outputs of WRITE SECTOR(S) and WRITE SECTOR(S) EXT: "DF (Device Fault)
     * shall be set to one if a device fault has occurred" alongside "ERR shall
     * be set to one if an Error register bit is set to one". A device fault
     * need not put anything in the Error register, so testing ERR alone lets a
     * faulted write report success. */
    uint8_t status = ata_read_status(drive_idx);
    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        uint8_t e = ata_read_error(drive_idx);
        debug_printf("[ATA] drv%u: STATUS=0x%02x%s ERR=0x%02x (%s) after "
                     "WRITE @LBA %lu\n",
                     drive_idx, status,
                     (status & ATA_SR_DF) ? " DEVICE-FAULT" : "",
                     e, ata_decode_error(e), (unsigned long)lba);
        spin_unlock(&g_ata_lock);
        return ATA_ERR_DRIVE_FAULT;
    }

    spin_unlock(&g_ata_lock);
    return 0;
}

/*
 * One FLUSH CACHE attempt, issued and then JUDGED.
 *
 * This is the volume's only durability barrier, so it is the one command whose
 * refusal must not be able to look like its success — and on this command the
 * two look almost identical. ATA/ATAPI-6 (T13/1410D r3a) covers it twice,
 * §8.12 for FLUSH CACHE (E7h) and §8.13 for FLUSH CACHE EXT (EAh, the one this
 * driver issues on any LBA48 drive), and both run the Non-data protocol of
 * §9.4. The two output clauses differ in ONE bit:
 *
 *   §8.13.5 Normal outputs  "BSY shall be cleared to zero indicating command
 *                           completion. DRDY shall be set to one. DF (Device
 *                           Fault) shall be cleared to zero. DRQ shall be
 *                           cleared to zero. ERR shall be cleared to zero."
 *   §8.13.6 Error outputs   "BSY shall be cleared to zero indicating command
 *                           completion. DRDY shall be set to one. DF (Device
 *                           Fault) shall be set to one if a device fault has
 *                           occurred. DRQ shall be cleared to zero. ERR shall
 *                           be set to one if an Error register bit is set to
 *                           one." — with "ABRT shall be set to one if the
 *                           device is not able to complete the action
 *                           requested by the command" (§8.12.6 says "may";
 *                           §8.13.6, the clause that governs EAh, says
 *                           "shall")
 *
 * So `!BSY && DRDY` is not completion — it is the END of the command, success
 * and failure alike, and the verdict is in ERR and DF. §9.4 transition
 * HND1:HI0 states the duty outright: "If status indicates that an error has
 * occured, the host shall take appropriate error recovery action."
 *
 * Which is why the test below is the WHOLE of those clauses and not the half
 * of them that ERR and DF occupy. Both require DRDY set and DRQ clear, so a
 * status that has neither an error bit nor DRDY — 0x00 from a device still
 * coming back out of a reset, say — is not this command's success either. It
 * is the same silent success in a different disguise, and it is reachable on
 * the very path the retry loop below creates.
 *
 * Two more clauses of the same protocol are honoured here. §9.4 state HND1:
 * "When entering this state from another state other than when an interrupt
 * has occurred, the host shall wait 400 ns before reading the Status
 * register" — without it the first read can still be the drive's PRE-command
 * idle status, which is BSY=0/DRDY=1 and reads as a finished flush. And
 * §8.12.8 and §8.13.8 both carry "NOTE − This command may take longer than
 * 30 s to complete", so the patience owed is the DRIVE CARRYING A COMMAND OUT
 * and not a register handshake — it arrives as `patience_ms`, what is left of
 * the one budget ata_flush_cache spends on the whole call.
 *
 * Returns 0 only when the device ended the command with nothing to say. On
 * ATA_ERR_DRIVE_FAULT the ending status and Error register are handed back for
 * the caller to report; nothing is printed from in here, because this runs
 * under g_ata_lock with interrupts off and the caller can say it once, after.
 */
static int ata_flush_once(uint8_t drive_idx, uint8_t cmd, uint32_t patience_ms,
                          uint8_t* out_status, uint8_t* out_error) {
    AtaChannel* ch = drive_channel_ptr(drive_idx);

    ata_select_drive(drive_idx);

    /* §9.4 HI1→HI3 and §8.13.7 Prerequisites, before the Command register is
     * written at all — see ata_wait_command_ready. */
    int rc = ata_wait_command_ready(drive_idx, patience_ms);
    if (rc != 0) return rc;

    outb(ch->cmd_base + ATA_REG_COMMAND, cmd);
    ata_delay_400ns(drive_idx);   /* §9.4, HND1: before the first Status read */

    rc = ata_wait_clear_bsy(drive_idx, patience_ms);
    if (rc != 0) return rc;

    uint8_t status = ata_read_status(drive_idx);
    if ((status & (ATA_SR_ERR | ATA_SR_DF | ATA_SR_DRQ)) ||
        !(status & ATA_SR_DRDY)) {
        *out_status = status;
        *out_error  = ata_read_error(drive_idx);
        return ATA_ERR_DRIVE_FAULT;
    }
    return 0;
}

int ata_flush_cache(uint8_t drive_idx) {
    if (!ata_legacy_path_active()) return ATA_ERR_NOT_SUPPORTED;
    if (drive_idx >= ATA_DRIVE_COUNT) return ATA_ERR_INVALID_ARGS;
    ATADevice* d = &g_ata_devices[drive_idx];
    AtaChannel* ch = drive_channel_ptr(drive_idx);
    if (!d->exists || !ch->present) return ATA_ERR_NO_DEVICE;

    uint8_t cmd = d->lba48_supported ? ATA_CMD_CACHE_FLUSH_EXT : ATA_CMD_CACHE_FLUSH;

    /* ONE budget for the whole call — the retries share it, they are not each
     * handed a fresh one.
     *
     * CONFIG_ATA_IO_TIMEOUT_MS is this kernel's answer to §8.13.8's "may take
     * longer than 30 s": it is how long the DRIVE may take to put its write
     * cache on the medium. Handing that to each attempt in turn would not be
     * more patient with the drive — it would be a minute and a half of this
     * core with interrupts disabled, holding g_ata_lock, because a retry only
     * happens after the drive has already refused or gone silent once. So each
     * attempt gets what the previous ones left, and what a channel reset costs
     * comes out of the same purse. */
    uint32_t budget_ms = CONFIG_ATA_IO_TIMEOUT_MS;
    uint8_t  status = 0, error = 0;
    int      rc = ATA_ERR_FLUSH_FAILED;

    spin_lock(&g_ata_lock);
    for (int retry = 0; retry < CONFIG_ATA_MAX_RETRIES; retry++) {
        uint64_t attempt_start = rdtsc();

        rc = ata_flush_once(drive_idx, cmd, budget_ms, &status, &error);
        if (rc == 0) {
            spin_unlock(&g_ata_lock);
            return 0;
        }
        /* The bus reads back 0xFF — there is nothing on this channel to flush
         * and nothing a reset would bring back. */
        if (rc == ATA_ERR_NO_DEVICE) break;

        /* ATA_ERR_TIMEOUT — the command never ended at all. Reset the channel
         * between attempts: a wedged drive blocks every subsequent write. The
         * reset runs *inside* the lock so no other core can find the channel
         * mid-reset; ata_channel_soft_reset only touches I/O ports, not heap
         * or other locks, so holding g_ata_lock across it is safe (no
         * lock-order risk).
         *
         * And that lock is why this caller asks for the SHORT patience: the
         * budget above is what the FLUSH ITSELF is owed by the spec, and the
         * reset is recovery on top of it — every millisecond spent here is
         * another millisecond with interrupts off on this core, buying nothing
         * the volume asked for. The spec's 31 s belongs to the boot-time
         * reset, which holds nothing.
         *
         * ATA_ERR_DRIVE_FAULT takes neither branch: a flush the device ENDED
         * with an error wants another flush, not a reset. §8.13.6 — "Subsequent
         * FLUSH CACHE EXT commands continue the process of flushing the cache
         * starting with the first sector after the sector in error" (§8.12.6
         * says the same of FLUSH CACHE). The drive is idle with DRDY set;
         * there is nothing for SRST to unstick, and resetting a drive that is
         * still holding dirty cache is the opposite of what is wanted here. */
        if (rc == ATA_ERR_TIMEOUT && retry < CONFIG_ATA_MAX_RETRIES - 1)
            ata_channel_soft_reset(drive_channel(drive_idx),
                                   ATA_SRST_BSY_HELD_MS);

        uint32_t spent = (uint32_t)cpu_tsc_to_ms(rdtsc() - attempt_start);
        if (spent >= budget_ms) break;
        budget_ms -= spent;
    }
    spin_unlock(&g_ata_lock);

    /* Said once, out here, with the lock released and interrupts back on — and
     * said with kprintf rather than debug_printf, which compiles to nothing in
     * a shipped build. Six of this flush's callers discard its return value, so
     * for them this line is the only thing that will ever report that the drive
     * is still holding data the volume has already been told is on the medium.
     * Every failed flush is a separate broken promise and gets its own line;
     * the driver does not get to decide that the volume's lies became boring. */
    if (rc == ATA_ERR_DRIVE_FAULT) {
        kprintf("[ATA] drv%u: FLUSH CACHE%s refused — STATUS=0x%02x%s%s%s "
                "ERROR=0x%02x (%s); the write cache is NOT on the medium\n",
                drive_idx, (cmd == ATA_CMD_CACHE_FLUSH_EXT) ? " EXT" : "",
                status,
                (status & ATA_SR_DF)   ? " DEVICE-FAULT" : "",
                (status & ATA_SR_DRQ)  ? " DRQ-SET"      : "",
                (status & ATA_SR_DRDY) ? ""              : " NOT-READY",
                error,
                error ? ata_decode_error(error) : "no Error-register bits set");
    } else {
        kprintf("[ATA] drv%u: FLUSH CACHE%s never ended (rc=%d) — the write "
                "cache is NOT on the medium\n",
                drive_idx, (cmd == ATA_CMD_CACHE_FLUSH_EXT) ? " EXT" : "", rc);
    }
    return ATA_ERR_FLUSH_FAILED;
}

/* ---------------------------------------------------------------------
 *  Retry wrappers
 * ------------------------------------------------------------------ */
static int ata_retry_classify(int rc) {
    /* Terminal errors — retrying won't help; bubble up immediately. */
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
    /* PIO. The count is eight bits on the twenty-eight-bit command (zero
     * meaning 256) and sixteen on the extended one; 128 stays inside both and
     * inside every controller's patience. */
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

/* ---------------------------------------------------------------------
 *  Channel discovery from PCI.
 *
 *  Search every PCI function with class/subclass 01/01h. The first
 *  matching device is used; additional controllers are logged so
 *  real-PC inventory is visible in the boot log even though the
 *  driver only addresses one chip.
 * ------------------------------------------------------------------ */
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

/* Count additional PCI IDE controllers beyond the first by walking
 * every (bus,dev,fn) directly. The driver only programs the first
 * controller, but real-HW boot diagnostics should report when there
 * are more so installs with add-in IDE cards aren't a silent surprise. */
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
        /* No PCI IDE controller — but real BIOS-boot PCs may still
         * have legacy ports wired through Super-I/O or PCMCIA cards
         * without exposing a PCI class. Seed the channels from compat
         * bases; IDENTIFY rejects absent slots cleanly. */
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

    /* Primary channel */
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

    /* Secondary channel */
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

    /* Bus-master IDE base — bit 7 of ProgIF advertises capability.
     * Per Intel BMIDE Spec §2.2 the 16 BAR4 I/O ports are split: primary
     * channel at offset 0, secondary at offset 8. We enable PCI Bus
     * Master + I/O Space in the controller's command register so the
     * device can drive PCI bus master cycles when the engine starts. */
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

/* ---------------------------------------------------------------------
 *  Public init
 * ------------------------------------------------------------------ */
void ata_init(void) {
    debug_printf("[ATA] Initialising legacy ATA/IDE driver...\n");
    spinlock_init(&g_ata_lock);

    memset(g_ata_channels, 0, sizeof(g_ata_channels));
    memset(g_ata_devices, 0, sizeof(g_ata_devices));

    if (ahci_is_initialized()) {
        /* AHCI owns block I/O. Surface a single synthetic record at
         * slot 0 so legacy callers (HwDiskInfo, boot diagnostics) see
         * a "device exists" answer; actual reads/writes/flushes return
         * ATA_ERR_NOT_SUPPORTED — the AHCI sync API is the only valid
         * path. */
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

    /* Soft reset both channels with nIEN=1 held throughout — the IRQ
     * line is silent before, during, and after reset.
     *
     * This is the one caller that holds nothing — no lock, no K-Core, and
     * nothing else is running yet — so it is the one that can afford the
     * patience the spec actually names. A drive doing a legitimate internal
     * recovery here would otherwise be declared absent for the whole boot. */
    for (uint8_t c = 0; c < ATA_CHANNEL_COUNT; c++) {
        ata_channel_soft_reset(c, ATA_SRST_BSY_SPEC_MS);
    }

    /* Bring up the BMIDE async engine — per-channel staging + PRD, IRQ
     * registration on each channel's GSI, nIEN cleared so the drive can
     * raise INTRQ. ata_identify -> ata_async_negotiate_dma_mode then
     * picks UDMA/MDMA on detected drives, and ata_read/write_sectors
     * route through ata_dma_sync for the IRQ-driven fast path. */
    ata_async_init();

    /* Probe every slot. ata_identify labels ATAPI devices for the log
     * even though they don't become block devices. */
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
