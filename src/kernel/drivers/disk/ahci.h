#ifndef AHCI_H
#define AHCI_H

#include "ktypes.h"
#include "pci.h"
#include "error.h"
#include "klib.h"  // For spinlock_t
#include "boxos_limits.h"
#include "kernel_config.h"
#include "baton.h"  // embedded never-drop recovery node

// HBA Register Offsets
#define AHCI_HBA_CAP        0x00
#define AHCI_HBA_GHC        0x04
#define AHCI_HBA_IS         0x08
#define AHCI_HBA_PI         0x0C
#define AHCI_HBA_VS         0x10
#define AHCI_HBA_CCC_CTL    0x14
#define AHCI_HBA_CCC_PORTS  0x18
#define AHCI_HBA_EM_LOC     0x1C
#define AHCI_HBA_EM_CTL     0x20
#define AHCI_HBA_CAP2       0x24
#define AHCI_HBA_BOHC       0x28

// Port Register Offsets (base = 0x100 + port * 0x80)
#define AHCI_PORT_CLB       0x00
#define AHCI_PORT_CLBU      0x04
#define AHCI_PORT_FB        0x08
#define AHCI_PORT_FBU      0x0C
#define AHCI_PORT_IS        0x10
#define AHCI_PORT_IE        0x14
#define AHCI_PORT_CMD       0x18
#define AHCI_PORT_TFD       0x20
#define AHCI_PORT_SIG       0x24
#define AHCI_PORT_SSTS      0x28
#define AHCI_PORT_SCTL      0x2C
#define AHCI_PORT_SERR      0x30
#define AHCI_PORT_SACT      0x34
#define AHCI_PORT_CI        0x38
#define AHCI_PORT_SNTF      0x3C

// CAP register bits
#define AHCI_CAP_S64A       (1U << 31)  // 64-bit addressing
#define AHCI_CAP_SNCQ       (1U << 30)  // Native Command Queuing
#define AHCI_CAP_SSNTF      (1U << 29)
#define AHCI_CAP_SMPS       (1U << 28)
#define AHCI_CAP_SSS        (1U << 27)
#define AHCI_CAP_SALP       (1U << 26)
#define AHCI_CAP_SAL        (1U << 25)
#define AHCI_CAP_SCLO       (1U << 24)
#define AHCI_CAP_ISS_SHIFT  20
#define AHCI_CAP_ISS_MASK   0xF
#define AHCI_CAP_SAM        (1U << 18)
#define AHCI_CAP_SPM        (1U << 17)
#define AHCI_CAP_FBSS       (1U << 16)
#define AHCI_CAP_PMD        (1U << 15)
#define AHCI_CAP_SSC        (1U << 14)
#define AHCI_CAP_PSC        (1U << 13)
#define AHCI_CAP_NCS_SHIFT  8           // Number of Command Slots (bits 12:8)
#define AHCI_CAP_NCS_MASK   0x1F
#define AHCI_CAP_CCCS       (1U << 7)
#define AHCI_CAP_EMS        (1U << 6)
#define AHCI_CAP_SXS        (1U << 5)
#define AHCI_CAP_NP_SHIFT   0
#define AHCI_CAP_NP_MASK    0x1F

// GHC register bits
#define AHCI_GHC_AE         (1U << 31)  // AHCI Enable
#define AHCI_GHC_MRSM       (1U << 2)
#define AHCI_GHC_IE         (1U << 1)   // Interrupt Enable
#define AHCI_GHC_HR         (1U << 0)   // HBA Reset

// CAP2 register bits (offset 0x24)
#define AHCI_CAP2_BOH       (1U << 0)   // BIOS/OS Handoff supported
#define AHCI_CAP2_NVMP      (1U << 1)   // NVMHCI Present
#define AHCI_CAP2_APST      (1U << 2)   // Automatic Partial to Slumber

// BOHC register bits (offset 0x28) — BIOS/OS Handoff Control & Status
// AHCI 1.3.1 §3.1.11 ("Offset 28h: BOHC"). The §3.1.18 that stood here names
// no section of that specification: §3.1 ends at 3.1.11, and §3.2 is Vendor
// Specific Registers.
#define AHCI_BOHC_BOS       (1U << 0)   // BIOS Owned Semaphore
#define AHCI_BOHC_OOS       (1U << 1)   // OS Owned Semaphore
#define AHCI_BOHC_SOOE      (1U << 2)   // SMI on OS Ownership Change Enable
#define AHCI_BOHC_OOC       (1U << 3)   // OS Ownership Change (W1C)
#define AHCI_BOHC_BB        (1U << 4)   // BIOS Busy

/* The two windows AHCI 1.3.1 §10.6.3 ("OS declares ownership request") names.
 * Both are the specification's own numbers, not a policy of ours: step 4, "If
 * the BIOS Busy (BOHC.BB) has been set to '1' within 25 milliseconds, then the
 * OS driver shall provide the BIOS a minimum of two seconds for finishing
 * outstanding commands on the HBA"; step 5, "If after 25 milliseconds the BIOS
 * Busy (BOHC.BB) bit has not been set to '1', then the OS can assume control
 * and perform whatever cleanup is necessary."
 *
 * The one second that used to stand in their place is neither of them. It was
 * spun on BOHC.BOS — the right bit, for a length the specification never
 * names — and the two-second wait that FOLLOWED it watched the wrong one:
 * BOHC.BB, which §10.6.4 has the firmware clear at step 7, one step BEFORE it
 * releases BOHC.BOS at step 8. So that second wait could end, and did end
 * silently, with the firmware still owning the controller. */
#define AHCI_BOH_BUSY_WINDOW_MS  25u
#define AHCI_BOH_GRANT_MS        2000u

// PxCMD register bits
#define AHCI_PCMD_ICC_SHIFT 28
#define AHCI_PCMD_ICC_MASK  0xF
#define AHCI_PCMD_ASP       (1U << 27)
#define AHCI_PCMD_ALPE      (1U << 26)
#define AHCI_PCMD_DLAE      (1U << 25)
#define AHCI_PCMD_ATAPI     (1U << 24)
#define AHCI_PCMD_APSTE     (1U << 23)
#define AHCI_PCMD_FBSCP     (1U << 22)
#define AHCI_PCMD_ESP       (1U << 21)
#define AHCI_PCMD_CPD       (1U << 20)
#define AHCI_PCMD_MPSP      (1U << 19)
#define AHCI_PCMD_HPCP      (1U << 18)
#define AHCI_PCMD_PMA       (1U << 17)
#define AHCI_PCMD_CPS       (1U << 16)
#define AHCI_PCMD_CR        (1U << 15)  // Command List Running
#define AHCI_PCMD_FR        (1U << 14)  // FIS Receive Running
#define AHCI_PCMD_MPSS      (1U << 13)
#define AHCI_PCMD_CCS_SHIFT 8
#define AHCI_PCMD_CCS_MASK  0x1F
#define AHCI_PCMD_FRE       (1U << 4)   // FIS Receive Enable
#define AHCI_PCMD_CLO       (1U << 3)
#define AHCI_PCMD_POD       (1U << 2)
#define AHCI_PCMD_SUD       (1U << 1)
#define AHCI_PCMD_ST        (1U << 0)   // Start

// PxTFD register bits
#define AHCI_PTFD_ERR_SHIFT 8
#define AHCI_PTFD_ERR_MASK  0xFF
#define AHCI_PTFD_STS_SHIFT 0
#define AHCI_PTFD_STS_MASK  0xFF
#define AHCI_PTFD_STS_BSY   (1U << 7)
#define AHCI_PTFD_STS_DRQ   (1U << 3)
#define AHCI_PTFD_STS_ERR   (1U << 0)

// PxIS/PxIE bits
#define AHCI_PIS_CPDS       (1U << 31)
#define AHCI_PIS_TFES       (1U << 30)  // Task File Error Status
#define AHCI_PIS_HBFS       (1U << 29)  // Host Bus Fatal Error Status
#define AHCI_PIS_HBDS       (1U << 28)
#define AHCI_PIS_IFS        (1U << 27)  // Interface Fatal Error Status
#define AHCI_PIS_INFS       (1U << 26)
#define AHCI_PIS_OFS        (1U << 24)
#define AHCI_PIS_IPMS       (1U << 23)
#define AHCI_PIS_PRCS       (1U << 22)
#define AHCI_PIS_DMPS       (1U << 7)
#define AHCI_PIS_PCS        (1U << 6)
#define AHCI_PIS_DPS        (1U << 5)
#define AHCI_PIS_UFS        (1U << 4)
#define AHCI_PIS_SDBS       (1U << 3)   // Set Device Bits (NCQ)
#define AHCI_PIS_DSS        (1U << 2)
#define AHCI_PIS_PSS        (1U << 1)
#define AHCI_PIS_DHRS       (1U << 0)

/* PxSERR bits (offset 30h) — AHCI 1.3.1 §3.3.12. Only the one bit that
 * ANSWERS A QUESTION is named; the rest are decoded positionally by
 * ahci_decode_serr, which is a printer and not a decision.
 *
 * §3.3.12, DIAG bit 26, verbatim: "Exchanged (X): When set to one this bit
 * indicates that a change in device presence has been detected since the last
 * time this bit was cleared... This bit shall always be set to one anytime a
 * COMINIT signal is received. This bit is reflected in the PxIS.PCS bit."
 *
 * That last sentence is why AHCI_PIS_PCS is not read anywhere below: PCS is
 * this bit seen through another register, so asking both would be asking the
 * same question twice. */
#define AHCI_SERR_DIAG_X    (1U << 26)

// PxSSTS bits
#define AHCI_SSTS_IPM_SHIFT 8
#define AHCI_SSTS_IPM_MASK  0xF
#define AHCI_SSTS_SPD_SHIFT 4
#define AHCI_SSTS_SPD_MASK  0xF
#define AHCI_SSTS_DET_SHIFT 0
#define AHCI_SSTS_DET_MASK  0xF
#define AHCI_SSTS_DET_NONE  0x0
#define AHCI_SSTS_DET_PRESENT 0x3
/* The rest of the DET vocabulary, so a port that is in none of the two states
 * above can still be named instead of guessed at. AHCI 1.3.1 §3.3.10: 1h,
 * "Device presence detected but Phy communication not established" — the
 * middle of the OOB handshake, a stage that passes; 4h, "Phy in offline mode
 * as a result of the interface being disabled or running in a BIST loopback
 * mode" — not a stage of anything, a place software put the Phy, which no
 * amount of waiting moves. */
#define AHCI_SSTS_DET_COMINIT 0x1
#define AHCI_SSTS_DET_OFFLINE 0x4

// Signature values
#define AHCI_SIG_ATA        0x00000101
#define AHCI_SIG_ATAPI      0xEB140101
#define AHCI_SIG_SEMB       0xC33C0101
#define AHCI_SIG_PM         0x96690101

// FIS Types
#define FIS_TYPE_REG_H2D    0x27
#define FIS_TYPE_REG_D2H    0x34
#define FIS_TYPE_DMA_ACT    0x39
#define FIS_TYPE_DMA_SETUP  0x41
#define FIS_TYPE_DATA       0x46
#define FIS_TYPE_BIST       0x58
#define FIS_TYPE_PIO_SETUP  0x5F
#define FIS_TYPE_DEV_BITS   0xA1

// ATA Commands (non-NCQ)
#define ATA_CMD_READ_DMA_EXT    0x25    // READ DMA EXT (48-bit LBA)
#define ATA_CMD_WRITE_DMA_EXT   0x35    // WRITE DMA EXT (48-bit LBA)
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA    // FLUSH CACHE EXT (48-bit LBA)
#define ATA_CMD_IDENTIFY_DEVICE 0xEC    // IDENTIFY DEVICE

// PxCMD.ICC (Interface Communication Control, bits 31:28) values
#define AHCI_PCMD_ICC_ACTIVE    0x1     // bring interface to active power state

// Command Header Flags
#define AHCI_CMDHDR_WRITE       (1U << 6)
#define AHCI_CMDHDR_ATAPI       (1U << 5)
#define AHCI_CMDHDR_PREFETCH    (1U << 7)
#define AHCI_CMDHDR_CLEAR_BSY   (1U << 10)

#define AHCI_MAX_PORTS          32
#define AHCI_MAX_SLOTS          32
#define AHCI_PRDT_MAX_ENTRIES   16

/* HBA register span = generic regs (0x100) + one 0x80 port-register block
 * per port. AHCI 1.3.1 §3: port N regs live at 0x100 + N*0x80, so a full
 * 32-port HBA needs 0x1100 bytes. Mapping a single 4 KiB page (the prior
 * bug) left ports >= 30 in unmapped memory -> #PF / silent loss on
 * high-port-count controllers. vmm_map_mmio rounds this up to pages. */
#define AHCI_ABAR_SPAN          (0x100 + AHCI_MAX_PORTS * 0x80)

#define AHCI_CMDTBL_SIZE        128
#define AHCI_PRDT_ENTRY_SIZE    16
#define AHCI_CMDTBL_ALIGN       128

// HBA Memory-mapped Registers (BAR5)
typedef volatile struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_ports;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t  reserved[212];
    uint8_t  vendor[96];
} __attribute__((packed)) ahci_hba_mem_t;

// Port Registers (offset 0x100 + port * 0x80)
typedef volatile struct {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t reserved0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint8_t  reserved1[44];
    uint8_t  vendor[16];
} __attribute__((packed)) ahci_port_regs_t;

// Received FIS Structure (256 bytes, 256-byte aligned)
typedef volatile struct {
    uint8_t dsfis[28];
    uint8_t reserved0[4];
    uint8_t psfis[20];
    uint8_t reserved1[12];
    uint8_t rfis[24];
    uint8_t reserved2[4];
    uint8_t sdbfis[8];
    uint8_t ufis[64];
    uint8_t reserved3[96];
} __attribute__((packed)) ahci_fis_t;

// Command Header (32 bytes per entry, 32 entries in 1KB list)
typedef struct {
    uint8_t  cfl:5;         // Command FIS Length (in DWORDs, 2-16)
    uint8_t  a:1;
    uint8_t  w:1;           // Write (1: H2D, 0: D2H)
    uint8_t  p:1;
    uint8_t  r:1;
    uint8_t  b:1;
    uint8_t  c:1;
    uint8_t  reserved0:1;
    uint8_t  pmp:4;
    uint16_t prdtl;         // PRDT Length (number of entries)
    uint32_t prdbc;         // PRD Byte Count (transferred)
    uint32_t ctba;          // Command Table Base Address (low)
    uint32_t ctbau;         // Command Table Base Address (upper)
    uint32_t reserved1[4];
} __attribute__((packed)) ahci_cmd_header_t;

// PRDT Entry (16 bytes)
typedef struct {
    uint32_t dba;           // Data Base Address (low)
    uint32_t dbau;          // Data Base Address (upper)
    uint32_t reserved0;
    uint32_t dbc:22;        // Data Byte Count (0-based, max 4MB)
    uint32_t reserved1:9;
    uint32_t i:1;           // Interrupt on Completion
} __attribute__((packed)) ahci_prdt_entry_t;

// Command Table (128 bytes + PRDT)
typedef struct {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  reserved[48];
    ahci_prdt_entry_t prdt[AHCI_PRDT_MAX_ENTRIES];
} __attribute__((packed)) ahci_cmd_table_t;

// Register FIS - Host to Device (20 bytes)
typedef struct {
    uint8_t  fis_type;      // FIS_TYPE_REG_H2D
    uint8_t  pmport:4;
    uint8_t  reserved0:3;
    uint8_t  c:1;           // 1: Command, 0: Control
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0;          // LBA bits 0-7
    uint8_t  lba1;          // LBA bits 8-15
    uint8_t  lba2;          // LBA bits 16-23
    uint8_t  device;
    uint8_t  lba3;          // LBA bits 24-31
    uint8_t  lba4;          // LBA bits 32-39
    uint8_t  lba5;          // LBA bits 40-47
    uint8_t  featureh;
    uint8_t  countl;
    uint8_t  counth;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  reserved1[4];
} __attribute__((packed)) fis_reg_h2d_t;

// NCQ Commands (FPDMA)
#define ATA_CMD_READ_FPDMA_QUEUED   0x60
#define ATA_CMD_WRITE_FPDMA_QUEUED  0x61

/* Timeout configuration (ms) — values now sourced from kernel_config.h to
 * avoid the historic dual source of truth (5000 here vs 2000 in config). */
#define AHCI_TIMEOUT_CMD_DEFAULT     CONFIG_AHCI_CMD_TIMEOUT_MS
#define AHCI_TIMEOUT_CMD_ENGINE_STOP 500
#define AHCI_TIMEOUT_MIN             1000
#define AHCI_TIMEOUT_MAX             30000

/* COMRESET, in the TWO quantities one constant used to stand for.
 *
 * AHCI 1.3.1 §10.4.2 ("Port Reset") describes the sequence: software writes
 * PxSCTL.DET = 1h, waits "at least 1 millisecond", then writes PxSCTL.DET = 0h
 * and waits for PxSSTS.DET to become 3h.
 *
 * Those are not the same kind of number and they were never the same number.
 * The first is a MINIMUM HOLD with a spec clause behind it — one millisecond,
 * and the hold is the whole point, so there is no register to poll and nothing
 * to exit early on. The old code spent AHCI_TIMEOUT_COMRESET_WAIT (a whole
 * second) in a bare `while (rdtsc() < deadline) cpu_pause();` that read no
 * register at all: one second of dead spin, per port, on every boot that did a
 * COMRESET.
 *
 * The second is a MAXIMUM WAIT with no spec clause behind it — §10.4.2 names
 * no bound on how long the link may take to come up — so one second there is a
 * DECLARED POLICY for how long a port gets before it is called empty. */
#define AHCI_COMRESET_HOLD_US        1000u
#define AHCI_COMRESET_LINK_MS        1000u

/* And a third question, which is neither of those two: is there anything on
 * this port at all?
 *
 * PxSSTS.DET reading 0h says two different things — "nothing is attached to
 * this port" and "a COMRESET went out just now and the device's COMINIT has
 * not come back yet" — and what used to tell them apart was a hundred
 * milliseconds of DET standing still. That interval was the reference
 * implementation's (Linux libata debounces SStatus with
 * sata_deb_timing_normal = { 5, 100, 2000 }), and a number another kernel
 * happens to use is not a fact about this port.
 *
 * THE FACT IS AN EVENT, AND BOTH SPECIFICATIONS NAME IT. AHCI 1.3.1 §7.3:
 * "When a COMRESET is sent to the device the PxSSTS.DET field shall be
 * cleared to 0h. When a COMINIT is received, the PxSSTS.DET field shall be
 * set to 1h." §3.3.10, note 1: "device presence shall always be indicated
 * anytime a COMINIT signal is received." And the event LATCHES, so it cannot
 * fall between two reads: §3.3.12 has PxSERR.DIAG.X "always set to one
 * anytime a COMINIT signal is received", and only software clears it. So
 * presence is "a COMINIT arrived" — DET off 0h, or DIAG.X standing — and it
 * is known the instant it happens.
 *
 * ABSENCE IS THE ABSENCE OF THAT EVENT, and only the absence needs a bound,
 * because nothing on an empty wire will ever answer. AHCI 1.3.1 §10.1.1 step
 * 5 says so and hands the bound on: "Wait for a positive indication that a
 * device is attached to the port (the maximum amount of time to wait for
 * presence indication is specified in the Serial ATA Revision 2.6
 * specification). This is done by polling PxSSTS.DET."
 *
 * Serial ATA Revision 2.6 (15-February-2007) — THE EDITION THAT SENTENCE
 * NAMES, not an earlier one that happens to say something similar —
 * §15.2.2.2 "OOB signaling sequence failure", verbatim: "The Phy does not
 * have any timeout conditions for the interface OOB reset signaling sequence
 * as defined in section 8.1. If a device is present, the Phy shall detect
 * device presence within 10 ms of a power-on reset (i.e. COMINIT shall be
 * returned within 10 ms of an issued COMRESET). If a device is not present,
 * the Phy is not required to time-out and may remain in the reset state
 * indefinitely until host software intervenes."
 *
 * The device side of the same requirement is stated again in the Phy state
 * machine, rev 2.6 DP2:DR_COMINIT note 2: "As indicated in section 13.1,
 * devices shall respond with a COMINIT signal within 10 ms of the negation of
 * power-on reset or de-qualification of a received COMRESET signal." Note 1
 * of the same state — "COMINIT transmitted for a 6 bursts duration" — and
 * DP3:DR_AwaitCOMWAKE after it are why the event has to be read from a LATCH
 * and not polled for: the device sends COMINIT once and then waits.
 *
 * Ten milliseconds, quoted, from the specification AHCI sends the reader to —
 * and the last sentence of that clause is also the reason software has to
 * hold the clock at all: on an empty wire no fact will ever arrive.
 *
 * ‼ AND THE CLAUSE BOUNDS "10 ms FROM AN ISSUED COMRESET", WHICH IS NOT WHERE
 * THIS WINDOW STARTS — it starts later, when the bring-up loop is reached,
 * and later is the SAFE direction. Presence, once indicated, does not expire:
 * PxSSTS.DET stays at 1h/3h until the next COMRESET (§7.3) and PxSERR.DIAG.X
 * stays set until software clears it (§3.3.12). So a device whose COMINIT
 * landed before this window opened is already standing in both places on the
 * first read; the window only has to cover the case where the COMRESET is
 * concurrent with it, which is the staggered-spin-up path where our own
 * PxCMD.SUD write is what emits it (§10.10.1).
 *
 * A port that raised no COMINIT inside that window is empty as a matter of
 * specification; a port that DID raise one is never judged by this window
 * again, and gets the whole of AHCI_COMRESET_LINK_MS to finish negotiating,
 * however long DET sits at 0h or 1h on the way. */
#define AHCI_PRESENCE_WINDOW_MS      10u

/* How long a device gets to drop BSY and DRQ during port bring-up.
 *
 * AHCI 1.3.1 §10.1.1 step 7 tells software to wait for the device to become
 * ready and defers the duration to the ATA specification; ATA/ATAPI-6
 * (T13/1410D r3a) §9.2, device state D0SR3, is 31 s. That is the SPEC
 * quantity. One second stood here, which is short enough to call a drive doing
 * a legitimate internal reset absent — and this wait is only reached after
 * PxSSTS.DET already said a device answered on the link, so it is never spent
 * on an empty port. Nothing is held across it: ahci_port_init runs at boot
 * with no lock and no K-Core to starve. */
#define AHCI_DEVICE_READY_MS         31000u

#define AHCI_MAX_RETRIES             CONFIG_AHCI_MAX_RETRIES
#define AHCI_MAX_COMRESET_ATTEMPTS   CONFIG_AHCI_MAX_COMRESET_ATTEMPTS

typedef struct {
    uint32_t cmd_count;
    uint32_t error_count;
    uint32_t timeout_count;
    uint32_t tfes_count;
    uint32_t comreset_count;
    uint32_t comreset_fail_count;
    uint64_t last_error_tsc;
    uint32_t last_serr;
    uint32_t last_tfd;
} ahci_port_stats_t;

typedef struct {
    uint32_t port_num;
    uint32_t pxis;
    uint32_t pxserr;
    uint32_t pxtfd;
    uint64_t tsc;
    const char* description;
} ahci_error_detail_t;

/* WHERE A PORT STANDS WITH RESPECT TO THE MACHINE.
 *
 * A port that stopped answering used to be marked AHCI_PORT_ERROR in a field
 * nothing read, so it stayed in service and went on being handed out. It now
 * LEAVES — it gives up its Boardroom seat the same way a USB stick that is
 * pulled out does — and this says whether that leaving is final.
 *
 * And leaving is not final. A pulled cable, a drive that browned out, a
 * backplane reseated: the link says so with a COMINIT, exactly the event
 * bring-up reads to decide a port is occupied at all, and the port is built
 * again and takes its seat back — BoardroomNoteArrival, the same road a USB
 * stick returns by. So this is read by the watchdog's return scan, not merely
 * written: it is what tells a port that LEFT from a port that was never
 * there, and only the first of those may come back. */
typedef enum {
    AHCI_STANDING_IN_SERVICE = 0,  /* the ordinary state; `active` agrees */
    AHCI_STANDING_LEFT             /* gone, and a COMINIT may bring it back */
} AhciPortStanding;

typedef struct {
    ahci_port_regs_t* regs;

    void*     clb_virt;
    uintptr_t clb_phys;

    void*     fis_virt;
    uintptr_t fis_phys;

    void*     ctba_virt[AHCI_MAX_SLOTS];
    uintptr_t ctba_phys[AHCI_MAX_SLOTS];

    uint32_t  slot_bitmap;  // 1 = free, 0 = used

    /* Every command slot this port may legally use (1 = exists), fixed at
     * bring-up; slot_bitmap is the free subset of it. It is the LOWER BOUND of
     * what the HBA offers (CAP.NCS + 1) and what the device will queue
     * (IDENTIFY DEVICE word 75 bits 4:0, plus one), because AHCI 1.3.1
     * §5.6.4.1 asks for exactly that bound — and because the same section
     * makes the slot index the NCQ tag ("software must match the tag number it
     * uses to the slot it is placing the command in"), this is also the set of
     * tags the device has queue entries for. */
    uint32_t  slot_mask;

    /* A whole-port hold, for the one command class that cannot share the port
     * (AHCI 1.3.1 §1.7). ahci_port_take_whole raises it at the instant every
     * slot is free and empties slot_bitmap, so ahci_alloc_slot has nothing to
     * hand out for as long as that one command runs; ahci_port_give_back
     * lowers it and puts the slots back. */
    bool      slot_held;
    uint8_t   active;
    uint32_t  signature;

    /* Per-port device geometry/capability from IDENTIFY DEVICE. The command
     * class is decided per port (AHCI/SATA forbids mixing queued and
     * non-queued commands outstanding on one port): ncq -> FPDMA QUEUED with
     * PxSACT completion; !ncq -> READ/WRITE DMA EXT with PxCI completion.
     *
     * That settles reads and writes, and it is not the whole story: FLUSH
     * CACHE EXT has no queued form at all, so on an NCQ port it is the one
     * command that can only be issued with the queue already empty. It is not
     * an exception to the rule above but the reason the rule needs a second
     * mechanism — ahci_port_take_whole — which ahci_flush_cache_sync uses
     * instead of drawing one slot beside live NCQ tags. */
    uint32_t  logical_sector_size;     // bytes; only 512 is supported by the stack
    /* What the drive is BUILT from, as opposed to what it is addressed in.
     * A 512e disk is addressed in 512-byte sectors and made of 4096-byte
     * ones, and a volume laid out on the finer grid costs the drive a read
     * and a rewrite for every write that does not cover a whole physical
     * block. Zero when the drive would not say. */
    uint32_t  physical_sector_size;
    uint64_t  total_sectors;           // device capacity in logical sectors
    bool      ncq;                     // HBA SNCQ && device IDENTIFY word 76 bit 8
    bool      lba48;                   // 48-bit LBA addressing supported

    /* Bitmask of command slots issued and awaiting completion. The IRQ
     * handler computes completions as issued_mask & ~outstanding, where
     * `outstanding` is PxSACT for NCQ ports or PxCI for non-NCQ ports. */
    uint32_t issued_mask;
    volatile uint32_t completed_slots;

    /* Set (CAS 0->1) by the IRQ handler when a fatal error schedules a
     * deferred COMRESET; cleared by the recovery bottom-half. Prevents a
     * storm of error IRQs from queuing redundant recoveries. */
    volatile uint32_t recovering;

    uint32_t event_id[AHCI_MAX_SLOTS];
    uint32_t pid[AHCI_MAX_SLOTS];
    uint64_t submit_tsc[AHCI_MAX_SLOTS];

    /* Per-slot async completion. Set non-NULL by ahci_submit_*_async(); the
     * IRQ handler invokes the callback once the slot retires (or on TFES).
     * Sync callers leave these NULL — the existing port->ci poll path
     * picks up their completion. Each slot is owned by exactly one flow
     * (sync OR async, never both) since ahci_alloc_slot returns a unique
     * slot index. */
    void   (*cb[AHCI_MAX_SLOTS])(uint8_t port, uint8_t slot,
                                  error_t status, void *ctx);
    void   *cb_ctx[AHCI_MAX_SLOTS];

    uint64_t ncq_reads;
    uint64_t ncq_writes;
    uint64_t ncq_errors;
    uint64_t ncq_timeouts;

    spinlock_t lock;
    ahci_port_stats_t stats;
    uint8_t port_num;

    /* WHO IS HOLDING THE WHOLE-PORT DOOR, while slot_held is true.
     *
     * Not bookkeeping: it is the only thing that tells a door which will be
     * given back from a door which will not. A core that is stopped while it
     * holds this port — every AP at shutdown, once halt_all_ap_cores has run
     * — can never reach ahci_port_give_back, and a waiter that does not ask
     * WHO is waiting on an event that cannot happen. amp_core_online() is
     * this kernel's own answer to "is that core still running", and
     * ahci_port_take_whole asks it rather than counting seconds. */
    uint8_t door_holder;

    /* WHETHER THIS PORT IS STILL IN THE MACHINE, and if it left, whether it
     * can come back. Written by ahci_port_retire / ahci_port_init, read by
     * the watchdog's return scan and by ahci_deferred_recover — a value with
     * readers, unlike the `status` field this replaces, which had four writes
     * and no reader anywhere in the tree. */
    volatile uint8_t standing;   /* AhciPortStanding */

    /* Never-drop recovery node (Ф26 M4): a fatal-error or watchdog-wedge
     * IRQ posts COMRESET to a K-Core via BatonPass with no
     * allocation. The `recovering` 0->1 CAS coalesces a storm of error IRQs
     * into one post, and ahci_deferred_recover resets it to 0 on completion,
     * so this node is enqueued at most once at a time. run =
     * ahci_deferred_recover, ctx = this port; set once in ahci_port_init. */
    Baton recover_node;
} ahci_port_t;

typedef struct {
    pci_device_t pci_dev;

    ahci_hba_mem_t* hba_mem;
    uintptr_t hba_phys;

    ahci_port_t ports[AHCI_MAX_PORTS];
    uint32_t    port_implemented;   // PI register value (bitmask)
    uint8_t     num_active_ports;

    uint32_t cap;
    uint8_t  num_slots;
    bool     ncq_support;
    bool     s64a_support;

    uint8_t  irq_vector;    // from PCI config offset 0x3C
    bool     irq_enabled;
    uint64_t total_interrupts;

    bool     initialized;
} ahci_controller_t;

int ahci_init(void);
void ahci_test_read(void);

void ahci_init_irq(void);
void ahci_port_enable_irq(uint8_t port_num);
void ahci_irq_handler(void);

/* Ф26 M1 — periodic async-completion watchdog (backstop for a lost/coalesced
 * completion MSI or a wedged device). Call once per PIT tick on the BSP.
 * Tier 1 reconciles a lost edge from the PxSACT/PxCI level (retires as success);
 * Tier 2 fails a genuinely wedged port's slots + defers a COMRESET. No-op on a
 * single core (async I/O disabled there). */
void ahci_watchdog_scan(void);

int ahci_alloc_slot(uint8_t port_num);
void ahci_free_slot(uint8_t port_num, uint8_t slot);
bool ahci_can_submit_port(uint8_t port_num);

/* WHY A WHOLE-PORT COMMAND COULD NOT BE ISSUED — three answers, and only one
 * of them is a clock.
 *
 * These used to be one answer: -1. So a port whose door was being held by
 * another flush AT THAT INSTANT, and a device that had been given commands
 * and never answered them, reached the filesystem wearing the same word, and
 * the filesystem turned both into ERR_IO. One of those is a transient the
 * caller should come back from; the other is a medium that is failing. */
typedef enum {
    AHCI_TAKE_GOT = 0,   /* the port is the caller's; *slot_out is its slot */
    AHCI_TAKE_HELD,      /* another whole-port command holds the door NOW */
    AHCI_TAKE_SILENT,    /* work outstanding to the device never came back */
    AHCI_TAKE_NO_PORT    /* there is no such port in service */
} AhciTake;

/* Take every command slot of a port for ONE non-queued command, and give them
 * back when it has finished. AHCI 1.3.1 §1.7: "System software is responsible
 * to ensure that queued and non-queued commands are not mixed in the command
 * list for the same device with the exception of the NCQ Unload command."
 *
 * The wait costs no other flow anything: the port is only closed at the
 * instant it is found completely idle, and only for as long as that one
 * command needs — a caller that retries it, and recovers the port between
 * tries, holds across both, because those are the same command still.
 *
 * `patience_ms` bounds ONE of the two ways this can fail to get the port, and
 * the enum above says which. A door already held is answered AHCI_TAKE_HELD
 * on the spot, with no clock turned at all: that is a fact about this instant
 * and waiting cannot make it truer. The clock is spent only on work the
 * DEVICE owes — commands issued and not yet retired — and running it out is
 * AHCI_TAKE_SILENT, which is a device that stopped answering.
 *
 * Every outcome but AHCI_TAKE_GOT holds nothing and issues nothing. A
 * successful take must be paired with exactly one give-back. */
AhciTake ahci_port_take_whole(uint8_t port_num, uint32_t patience_ms,
                              uint8_t* slot_out);
void ahci_port_give_back(uint8_t port_num);

/* Build a read/write command for `slot`. Picks FPDMA QUEUED (NCQ) or
 * READ/WRITE DMA EXT (non-queued) based on the port's command class, so the
 * caller does not need to know which. Arm with ahci_arm_slot(). */
error_t ahci_build_io(uint8_t port_num, uint8_t slot, uint64_t lba,
                      uint16_t sector_count, void* buffer_phys, bool write);

/* Arm a built slot: NCQ ports set PxSACT then PxCI; non-NCQ set PxCI only,
 * and track the slot in issued_mask for the IRQ completion path. */
void ahci_arm_slot(uint8_t port_num, uint8_t slot);

/* True if the port issues NCQ (FPDMA) commands. Callers that poll for
 * completion must watch PxSACT when true, PxCI when false. */
bool ahci_port_is_ncq(uint8_t port_num);

ahci_port_t* ahci_get_port_state(uint8_t port_num);
volatile ahci_port_regs_t* ahci_get_port_regs_pub(uint8_t port_num);

bool ahci_is_initialized(void);
uint8_t ahci_get_active_port_count(void);
uint32_t ahci_get_active_port_mask(void);

error_t ahci_port_comreset(ahci_port_t* port);

/* Get the port's link back, or take the port out of the machine.
 *
 * Returns OK when a COMRESET brought the link up again. On ERR_IO the port has
 * LEFT: it is no longer handed out by ahci_get_port, its outstanding commands
 * have been failed, its interrupt is disarmed and its engine stopped, and the
 * Boardroom has been told its seat is empty the same way a USB stick's
 * departure is told. Nothing may be issued on it afterwards, and every ahci_*
 * call that names it answers as it does for a port that was never there.
 *
 * Leaving is not final. The watchdog watches a departed port's PxSERR.DIAG.X
 * latch, and a COMINIT on it brings the port through bring-up again — full
 * IDENTIFY, because the disk on that cable may not be the one that left — and
 * back into the room through BoardroomNoteArrival. */
error_t ahci_port_recover(ahci_port_t* port);

void ahci_get_port_stats(uint8_t port_num, ahci_port_stats_t* stats_out);
void ahci_log_error(ahci_port_t* port, uint32_t pxis);
const char* ahci_decode_serr(uint32_t serr);

#endif // AHCI_H
