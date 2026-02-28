#ifndef AHCI_H
#define AHCI_H

#include "ktypes.h"
#include "pci.h"
#include "error.h"
#include "klib.h"  // For spinlock_t
#include "boxos_limits.h"
#include "kernel_config.h"

// AHCI HBA Register Offsets
#define AHCI_HBA_CAP        0x00  // Host Capabilities
#define AHCI_HBA_GHC        0x04  // Global Host Control
#define AHCI_HBA_IS         0x08  // Interrupt Status
#define AHCI_HBA_PI         0x0C  // Ports Implemented
#define AHCI_HBA_VS         0x10  // Version
#define AHCI_HBA_CCC_CTL    0x14  // Command Completion Coalescing Control
#define AHCI_HBA_CCC_PORTS  0x18  // Command Completion Coalescing Ports
#define AHCI_HBA_EM_LOC     0x1C  // Enclosure Management Location
#define AHCI_HBA_EM_CTL     0x20  // Enclosure Management Control
#define AHCI_HBA_CAP2       0x24  // Host Capabilities Extended
#define AHCI_HBA_BOHC       0x28  // BIOS/OS Handoff Control

// AHCI Port Register Offsets (base = 0x100 + port * 0x80)
#define AHCI_PORT_CLB       0x00  // Command List Base Address
#define AHCI_PORT_CLBU      0x04  // Command List Base Address Upper 32-bits
#define AHCI_PORT_FB        0x08  // FIS Base Address
#define AHCI_PORT_FBU       0x0C  // FIS Base Address Upper 32-bits
#define AHCI_PORT_IS        0x10  // Interrupt Status
#define AHCI_PORT_IE        0x14  // Interrupt Enable
#define AHCI_PORT_CMD       0x18  // Command and Status
#define AHCI_PORT_TFD       0x20  // Task File Data
#define AHCI_PORT_SIG       0x24  // Signature
#define AHCI_PORT_SSTS      0x28  // SATA Status (SCR0: SStatus)
#define AHCI_PORT_SCTL      0x2C  // SATA Control (SCR2: SControl)
#define AHCI_PORT_SERR      0x30  // SATA Error (SCR1: SError)
#define AHCI_PORT_SACT      0x34  // SATA Active (SCR3: SActive)
#define AHCI_PORT_CI        0x38  // Command Issue
#define AHCI_PORT_SNTF      0x3C  // SATA Notification (SCR4: SNotification)

// HBA Capabilities (CAP) Register Bits
#define AHCI_CAP_S64A       (1U << 31)  // Supports 64-bit Addressing
#define AHCI_CAP_SNCQ       (1U << 30)  // Supports Native Command Queuing
#define AHCI_CAP_SSNTF      (1U << 29)  // Supports SNotification Register
#define AHCI_CAP_SMPS       (1U << 28)  // Supports Mechanical Presence Switch
#define AHCI_CAP_SSS        (1U << 27)  // Supports Staggered Spin-up
#define AHCI_CAP_SALP       (1U << 26)  // Supports Aggressive Link Power Management
#define AHCI_CAP_SAL        (1U << 25)  // Supports Activity LED
#define AHCI_CAP_SCLO       (1U << 24)  // Supports Command List Override
#define AHCI_CAP_ISS_SHIFT  20          // Interface Speed Support (bits 23:20)
#define AHCI_CAP_ISS_MASK   0xF
#define AHCI_CAP_SAM        (1U << 18)  // Supports AHCI mode only
#define AHCI_CAP_SPM        (1U << 17)  // Supports Port Multiplier
#define AHCI_CAP_FBSS       (1U << 16)  // FIS-based Switching Supported
#define AHCI_CAP_PMD        (1U << 15)  // PIO Multiple DRQ Block
#define AHCI_CAP_SSC        (1U << 14)  // Slumber State Capable
#define AHCI_CAP_PSC        (1U << 13)  // Partial State Capable
#define AHCI_CAP_NCS_SHIFT  8           // Number of Command Slots (bits 12:8)
#define AHCI_CAP_NCS_MASK   0x1F
#define AHCI_CAP_CCCS       (1U << 7)   // Command Completion Coalescing Supported
#define AHCI_CAP_EMS        (1U << 6)   // Enclosure Management Supported
#define AHCI_CAP_SXS        (1U << 5)   // Supports External SATA
#define AHCI_CAP_NP_SHIFT   0           // Number of Ports (bits 4:0)
#define AHCI_CAP_NP_MASK    0x1F

// Global Host Control (GHC) Register Bits
#define AHCI_GHC_AE         (1U << 31)  // AHCI Enable
#define AHCI_GHC_MRSM       (1U << 2)   // MSI Revert to Single Message
#define AHCI_GHC_IE         (1U << 1)   // Interrupt Enable
#define AHCI_GHC_HR         (1U << 0)   // HBA Reset

// Port Command and Status (PxCMD) Register Bits
#define AHCI_PCMD_ICC_SHIFT 28          // Interface Communication Control
#define AHCI_PCMD_ICC_MASK  0xF
#define AHCI_PCMD_ASP       (1U << 27)  // Aggressive Slumber / Partial
#define AHCI_PCMD_ALPE      (1U << 26)  // Aggressive Link Power Management Enable
#define AHCI_PCMD_DLAE      (1U << 25)  // Drive LED on ATAPI Enable
#define AHCI_PCMD_ATAPI     (1U << 24)  // Device is ATAPI
#define AHCI_PCMD_APSTE     (1U << 23)  // Automatic Partial to Slumber Transitions Enabled
#define AHCI_PCMD_FBSCP     (1U << 22)  // FIS-based Switching Capable Port
#define AHCI_PCMD_ESP       (1U << 21)  // External SATA Port
#define AHCI_PCMD_CPD       (1U << 20)  // Cold Presence Detect
#define AHCI_PCMD_MPSP      (1U << 19)  // Mechanical Presence Switch Attached to Port
#define AHCI_PCMD_HPCP      (1U << 18)  // Hot Plug Capable Port
#define AHCI_PCMD_PMA       (1U << 17)  // Port Multiplier Attached
#define AHCI_PCMD_CPS       (1U << 16)  // Cold Presence State
#define AHCI_PCMD_CR        (1U << 15)  // Command List Running
#define AHCI_PCMD_FR        (1U << 14)  // FIS Receive Running
#define AHCI_PCMD_MPSS      (1U << 13)  // Mechanical Presence Switch State
#define AHCI_PCMD_CCS_SHIFT 8           // Current Command Slot (bits 12:8)
#define AHCI_PCMD_CCS_MASK  0x1F
#define AHCI_PCMD_FRE       (1U << 4)   // FIS Receive Enable
#define AHCI_PCMD_CLO       (1U << 3)   // Command List Override
#define AHCI_PCMD_POD       (1U << 2)   // Power On Device
#define AHCI_PCMD_SUD       (1U << 1)   // Spin-Up Device
#define AHCI_PCMD_ST        (1U << 0)   // Start

// Port Task File Data (PxTFD) Register Bits
#define AHCI_PTFD_ERR_SHIFT 8           // Error (bits 15:8)
#define AHCI_PTFD_ERR_MASK  0xFF
#define AHCI_PTFD_STS_SHIFT 0           // Status (bits 7:0)
#define AHCI_PTFD_STS_MASK  0xFF
#define AHCI_PTFD_STS_BSY   (1U << 7)   // Busy
#define AHCI_PTFD_STS_DRQ   (1U << 3)   // Data Request
#define AHCI_PTFD_STS_ERR   (1U << 0)   // Error

// Port Interrupt Status/Enable (PxIS/PxIE) Register Bits
#define AHCI_PIS_CPDS       (1U << 31)  // Cold Port Detect Status
#define AHCI_PIS_TFES       (1U << 30)  // Task File Error Status
#define AHCI_PIS_HBFS       (1U << 29)  // Host Bus Fatal Error Status
#define AHCI_PIS_HBDS       (1U << 28)  // Host Bus Data Error Status
#define AHCI_PIS_IFS        (1U << 27)  // Interface Fatal Error Status
#define AHCI_PIS_INFS       (1U << 26)  // Interface Non-fatal Error Status
#define AHCI_PIS_OFS        (1U << 24)  // Overflow Status
#define AHCI_PIS_IPMS       (1U << 23)  // Incorrect Port Multiplier Status
#define AHCI_PIS_PRCS       (1U << 22)  // PhyRdy Change Status
#define AHCI_PIS_DMPS       (1U << 7)   // Device Mechanical Presence Status
#define AHCI_PIS_PCS        (1U << 6)   // Port Connect Change Status
#define AHCI_PIS_DPS        (1U << 5)   // Descriptor Processed
#define AHCI_PIS_UFS        (1U << 4)   // Unknown FIS Interrupt
#define AHCI_PIS_SDBS       (1U << 3)   // Set Device Bits Interrupt
#define AHCI_PIS_DSS        (1U << 2)   // DMA Setup FIS Interrupt
#define AHCI_PIS_PSS        (1U << 1)   // PIO Setup FIS Interrupt
#define AHCI_PIS_DHRS       (1U << 0)   // Device to Host Register FIS Interrupt

// SATA Status (PxSSTS) Register Bits
#define AHCI_SSTS_IPM_SHIFT 8           // Interface Power Management (bits 11:8)
#define AHCI_SSTS_IPM_MASK  0xF
#define AHCI_SSTS_SPD_SHIFT 4           // Interface Speed (bits 7:4)
#define AHCI_SSTS_SPD_MASK  0xF
#define AHCI_SSTS_DET_SHIFT 0           // Device Detection (bits 3:0)
#define AHCI_SSTS_DET_MASK  0xF
#define AHCI_SSTS_DET_NONE  0x0
#define AHCI_SSTS_DET_PRESENT 0x3

// AHCI Signature Values
#define AHCI_SIG_ATA        0x00000101  // SATA drive
#define AHCI_SIG_ATAPI      0xEB140101  // SATAPI drive
#define AHCI_SIG_SEMB       0xC33C0101  // Enclosure management bridge
#define AHCI_SIG_PM         0x96690101  // Port multiplier

// FIS Types
#define FIS_TYPE_REG_H2D    0x27        // Register FIS - Host to Device
#define FIS_TYPE_REG_D2H    0x34        // Register FIS - Device to Host
#define FIS_TYPE_DMA_ACT    0x39        // DMA Activate FIS - Device to Host
#define FIS_TYPE_DMA_SETUP  0x41        // DMA Setup FIS - Bidirectional
#define FIS_TYPE_DATA       0x46        // Data FIS - Bidirectional
#define FIS_TYPE_BIST       0x58        // BIST Activate FIS - Bidirectional
#define FIS_TYPE_PIO_SETUP  0x5F        // PIO Setup FIS - Device to Host
#define FIS_TYPE_DEV_BITS   0xA1        // Set Device Bits FIS - Device to Host

// ATA Commands (non-NCQ)
#define ATA_CMD_READ_DMA_EXT    0x25    // READ DMA EXT (48-bit LBA)
#define ATA_CMD_WRITE_DMA_EXT   0x35    // WRITE DMA EXT (48-bit LBA)

// Command Header Flags
#define AHCI_CMDHDR_WRITE       (1U << 6)   // Write (H2D) direction
#define AHCI_CMDHDR_ATAPI       (1U << 5)   // ATAPI command
#define AHCI_CMDHDR_PREFETCH    (1U << 7)   // Prefetchable
#define AHCI_CMDHDR_CLEAR_BSY   (1U << 10)  // Clear Busy upon R_OK

// AHCI Limits
#define AHCI_MAX_PORTS          32
#define AHCI_MAX_SLOTS          32
#define AHCI_PRDT_MAX_ENTRIES   16          // Phase 1: conservative limit

// Memory sizes
#define AHCI_CLB_SIZE           BOXOS_AHCI_CLB_SIZE        // Command List: 1KB (32 headers * 32B)
#define AHCI_FIS_SIZE           BOXOS_AHCI_FIS_SIZE        // FIS Receive Area: 256B
#define AHCI_CMDTBL_SIZE        128         // Command Table Header: 128B
#define AHCI_PRDT_ENTRY_SIZE    16          // PRDT entry: 16B

// Memory alignment requirements
#define AHCI_CLB_ALIGN          BOXOS_AHCI_CLB_ALIGN
#define AHCI_FIS_ALIGN          BOXOS_AHCI_FIS_ALIGN
#define AHCI_CMDTBL_ALIGN       128

// HBA Memory-mapped Registers (BAR5)
typedef volatile struct {
    uint32_t cap;           // 0x00: Host Capabilities
    uint32_t ghc;           // 0x04: Global Host Control
    uint32_t is;            // 0x08: Interrupt Status
    uint32_t pi;            // 0x0C: Ports Implemented
    uint32_t vs;            // 0x10: Version
    uint32_t ccc_ctl;       // 0x14: Command Completion Coalescing Control
    uint32_t ccc_ports;     // 0x18: Command Completion Coalescing Ports
    uint32_t em_loc;        // 0x1C: Enclosure Management Location
    uint32_t em_ctl;        // 0x20: Enclosure Management Control
    uint32_t cap2;          // 0x24: Host Capabilities Extended
    uint32_t bohc;          // 0x28: BIOS/OS Handoff Control
    uint8_t  reserved[212]; // 0x2C-0xFF: Reserved
    uint8_t  vendor[96];    // 0x100-0x15F: Vendor Specific
} __attribute__((packed)) ahci_hba_mem_t;

// Port Registers (offset 0x100 + port * 0x80)
typedef volatile struct {
    uint32_t clb;           // 0x00: Command List Base Address
    uint32_t clbu;          // 0x04: Command List Base Address Upper 32-bits
    uint32_t fb;            // 0x08: FIS Base Address
    uint32_t fbu;           // 0x0C: FIS Base Address Upper 32-bits
    uint32_t is;            // 0x10: Interrupt Status
    uint32_t ie;            // 0x14: Interrupt Enable
    uint32_t cmd;           // 0x18: Command and Status
    uint32_t reserved0;     // 0x1C: Reserved
    uint32_t tfd;           // 0x20: Task File Data
    uint32_t sig;           // 0x24: Signature
    uint32_t ssts;          // 0x28: SATA Status
    uint32_t sctl;          // 0x2C: SATA Control
    uint32_t serr;          // 0x30: SATA Error
    uint32_t sact;          // 0x34: SATA Active
    uint32_t ci;            // 0x38: Command Issue
    uint32_t sntf;          // 0x3C: SATA Notification
    uint32_t fbs;           // 0x40: FIS-based Switching Control
    uint8_t  reserved1[44]; // 0x44-0x6F: Reserved
    uint8_t  vendor[16];    // 0x70-0x7F: Vendor Specific
} __attribute__((packed)) ahci_port_regs_t;

// Received FIS Structure (256 bytes, 256-byte aligned)
typedef volatile struct {
    uint8_t dsfis[28];      // 0x00: DMA Setup FIS
    uint8_t reserved0[4];   // 0x1C: Reserved
    uint8_t psfis[20];      // 0x20: PIO Setup FIS
    uint8_t reserved1[12];  // 0x34: Reserved
    uint8_t rfis[24];       // 0x40: Register Device to Host FIS
    uint8_t reserved2[4];   // 0x58: Reserved
    uint8_t sdbfis[8];      // 0x5C: Set Device Bits FIS
    uint8_t ufis[64];       // 0x64: Unknown FIS
    uint8_t reserved3[96];  // 0xA4: Reserved
} __attribute__((packed)) ahci_fis_t;

// Command Header (32 bytes per entry, 32 entries in 1KB list)
typedef struct {
    uint8_t  cfl:5;         // Command FIS Length (in DWORDs, 2-16)
    uint8_t  a:1;           // ATAPI
    uint8_t  w:1;           // Write (1: H2D, 0: D2H)
    uint8_t  p:1;           // Prefetchable
    uint8_t  r:1;           // Reset
    uint8_t  b:1;           // BIST
    uint8_t  c:1;           // Clear Busy upon R_OK
    uint8_t  reserved0:1;
    uint8_t  pmp:4;         // Port Multiplier Port
    uint16_t prdtl;         // PRDT Length (number of entries)
    uint32_t prdbc;         // PRD Byte Count (transferred)
    uint32_t ctba;          // Command Table Base Address (low)
    uint32_t ctbau;         // Command Table Base Address (upper)
    uint32_t reserved1[4];  // Reserved
} __attribute__((packed)) ahci_cmd_header_t;

// PRDT Entry (16 bytes)
typedef struct {
    uint32_t dba;           // Data Base Address (low)
    uint32_t dbau;          // Data Base Address (upper)
    uint32_t reserved0;     // Reserved
    uint32_t dbc:22;        // Data Byte Count (0-based, max 4MB)
    uint32_t reserved1:9;   // Reserved
    uint32_t i:1;           // Interrupt on Completion
} __attribute__((packed)) ahci_prdt_entry_t;

// Command Table (128 bytes + PRDT)
typedef struct {
    uint8_t  cfis[64];      // Command FIS
    uint8_t  acmd[16];      // ATAPI Command (12 or 16 bytes)
    uint8_t  reserved[48];  // Reserved
    ahci_prdt_entry_t prdt[AHCI_PRDT_MAX_ENTRIES]; // PRDT entries
} __attribute__((packed)) ahci_cmd_table_t;

// Register FIS - Host to Device (20 bytes)
typedef struct {
    uint8_t  fis_type;      // FIS_TYPE_REG_H2D
    uint8_t  pmport:4;      // Port Multiplier port
    uint8_t  reserved0:3;   // Reserved
    uint8_t  c:1;           // 1: Command, 0: Control
    uint8_t  command;       // ATA command register
    uint8_t  featurel;      // Feature register (low)
    uint8_t  lba0;          // LBA bits 0-7
    uint8_t  lba1;          // LBA bits 8-15
    uint8_t  lba2;          // LBA bits 16-23
    uint8_t  device;        // Device register
    uint8_t  lba3;          // LBA bits 24-31
    uint8_t  lba4;          // LBA bits 32-39
    uint8_t  lba5;          // LBA bits 40-47
    uint8_t  featureh;      // Feature register (high)
    uint8_t  countl;        // Count register (low)
    uint8_t  counth;        // Count register (high)
    uint8_t  icc;           // Isochronous Command Completion
    uint8_t  control;       // Control register
    uint8_t  reserved1[4];  // Reserved
} __attribute__((packed)) fis_reg_h2d_t;

// NCQ Commands (FPDMA)
#define ATA_CMD_READ_FPDMA_QUEUED   0x60
#define ATA_CMD_WRITE_FPDMA_QUEUED  0x61

// Timeout configuration
#define AHCI_TIMEOUT_CMD_DEFAULT     5000
#define AHCI_TIMEOUT_COMRESET_WAIT   1000
#define AHCI_TIMEOUT_CMD_ENGINE_STOP 500
#define AHCI_TIMEOUT_MIN             1000
#define AHCI_TIMEOUT_MAX             30000

// Retry limits
#define AHCI_MAX_RETRIES             CONFIG_AHCI_MAX_RETRIES
#define AHCI_MAX_COMRESET_ATTEMPTS   CONFIG_AHCI_MAX_COMRESET_ATTEMPTS

// Error statistics
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

// Error detail
typedef struct {
    uint32_t port_num;
    uint32_t pxis;
    uint32_t pxserr;
    uint32_t pxtfd;
    uint64_t tsc;
    const char* description;
} ahci_error_detail_t;

// Port State
typedef struct {
    ahci_port_regs_t* regs; // Port registers (MMIO)

    void*     clb_virt;     // Command List virtual address
    uintptr_t clb_phys;     // Command List physical address

    void*     fis_virt;     // FIS Receive Area virtual address
    uintptr_t fis_phys;     // FIS Receive Area physical address

    void*     ctba_virt[AHCI_MAX_SLOTS]; // Command Table virtual addresses
    uintptr_t ctba_phys[AHCI_MAX_SLOTS]; // Command Table physical addresses

    uint32_t  slot_bitmap;  // Free slots (1 = free, 0 = used)
    uint8_t   active;       // Port is active
    uint32_t  signature;    // Device signature

    // NCQ Slot Management
    uint32_t ci_snapshot;              // Last known PxCI (for completion delta)
    volatile uint32_t completed_slots; // IRQ sets bits, guide.c clears

    // Active Command Tracking (per slot)
    uint32_t event_id[AHCI_MAX_SLOTS];     // Map slot -> async_io event_id
    uint32_t pid[AHCI_MAX_SLOTS];          // Map slot -> process ID
    uint64_t submit_tsc[AHCI_MAX_SLOTS];   // TSC at submission (for timeout)

    // DMA Staging Buffers (WRITE operations)
    void* staging_phys[AHCI_MAX_SLOTS];    // Physical addresses (4KB each)
    void* staging_virt[AHCI_MAX_SLOTS];    // Virtual addresses (kernel mapped)

    // Statistics
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
} ahci_port_t;

// AHCI Controller State
typedef struct {
    pci_device_t pci_dev;   // PCI device info

    ahci_hba_mem_t* hba_mem; // HBA MMIO base (virtual)
    uintptr_t hba_phys;      // HBA MMIO base (physical)

    // Phase 3: Single-port support (port 0 only)
    ahci_port_t port0;

    uint32_t cap;           // Cached CAP register
    uint8_t  num_slots;     // Number of command slots per port
    bool     ncq_support;   // Native Command Queuing supported
    bool     s64a_support;  // 64-bit addressing supported

    uint8_t  irq_vector;    // PCI config offset 0x3C
    bool     irq_enabled;
    uint64_t total_interrupts;

    bool     initialized;   // Initialization complete
} ahci_controller_t;

// API Functions
int ahci_init(void);
void ahci_test_read(void);

// IRQ Infrastructure
void ahci_init_irq(void);
void ahci_port_enable_irq(uint8_t port_num);
void ahci_irq_handler(void);

// Slot Management
int ahci_alloc_slot(uint8_t port_num);
void ahci_free_slot(uint8_t port_num, uint8_t slot);
bool ahci_can_submit_port(uint8_t port_num);

// NCQ Operations
boxos_error_t ahci_build_ncq_read(uint8_t port_num, uint8_t slot, uint64_t lba,
                                   uint16_t sector_count, void* buffer_phys);
boxos_error_t ahci_build_ncq_write(uint8_t port_num, uint8_t slot, uint64_t lba,
                                    uint16_t sector_count, void* buffer_phys);

// Async Transfer API (requires async_io.h to be included by caller)
struct async_io_request;
boxos_error_t ahci_start_async_transfer(struct async_io_request* req);
void ahci_check_timeouts(void);

// Internal accessors (for guide.c)
ahci_port_t* ahci_get_port_state(uint8_t port_num);
volatile ahci_port_regs_t* ahci_get_port_regs_pub(uint8_t port_num);

bool ahci_is_initialized(void);

// Error recovery
boxos_error_t ahci_port_comreset(ahci_port_t* port);
boxos_error_t ahci_port_recover(ahci_port_t* port);

// Diagnostics
void ahci_get_port_stats(uint8_t port_num, ahci_port_stats_t* stats_out);
void ahci_log_error(ahci_port_t* port, uint32_t pxis);
const char* ahci_decode_serr(uint32_t serr);

#endif // AHCI_H
