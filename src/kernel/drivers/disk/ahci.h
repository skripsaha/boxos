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
// AHCI 1.3.1 §3.1.18
#define AHCI_BOHC_BOS       (1U << 0)   // BIOS Owned Semaphore
#define AHCI_BOHC_OOS       (1U << 1)   // OS Owned Semaphore
#define AHCI_BOHC_SOOE      (1U << 2)   // SMI on OS Ownership Change Enable
#define AHCI_BOHC_OOC       (1U << 3)   // OS Ownership Change (W1C)
#define AHCI_BOHC_BB        (1U << 4)   // BIOS Busy

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

// PxSSTS bits
#define AHCI_SSTS_IPM_SHIFT 8
#define AHCI_SSTS_IPM_MASK  0xF
#define AHCI_SSTS_SPD_SHIFT 4
#define AHCI_SSTS_SPD_MASK  0xF
#define AHCI_SSTS_DET_SHIFT 0
#define AHCI_SSTS_DET_MASK  0xF
#define AHCI_SSTS_DET_NONE  0x0
#define AHCI_SSTS_DET_PRESENT 0x3

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
#define AHCI_TIMEOUT_COMRESET_WAIT   1000
#define AHCI_TIMEOUT_CMD_ENGINE_STOP 500
#define AHCI_TIMEOUT_MIN             1000
#define AHCI_TIMEOUT_MAX             30000

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

typedef struct {
    ahci_port_regs_t* regs;

    void*     clb_virt;
    uintptr_t clb_phys;

    void*     fis_virt;
    uintptr_t fis_phys;

    void*     ctba_virt[AHCI_MAX_SLOTS];
    uintptr_t ctba_phys[AHCI_MAX_SLOTS];

    uint32_t  slot_bitmap;  // 1 = free, 0 = used
    uint8_t   active;
    uint32_t  signature;

    /* Per-port device geometry/capability from IDENTIFY DEVICE. The command
     * class is decided per port (AHCI/SATA forbids mixing queued and
     * non-queued commands outstanding on one port): ncq -> FPDMA QUEUED with
     * PxSACT completion; !ncq -> READ/WRITE DMA EXT with PxCI completion. */
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
    enum {
        AHCI_PORT_EMPTY = 0,
        AHCI_PORT_ACTIVE = 1,
        AHCI_PORT_FAILED = 2,
        AHCI_PORT_ERROR = 3
    } status;

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
error_t ahci_port_recover(ahci_port_t* port);

void ahci_get_port_stats(uint8_t port_num, ahci_port_stats_t* stats_out);
void ahci_log_error(ahci_port_t* port, uint32_t pxis);
const char* ahci_decode_serr(uint32_t serr);

#endif // AHCI_H
