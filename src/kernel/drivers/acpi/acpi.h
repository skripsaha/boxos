#ifndef ACPI_H
#define ACPI_H

#include "ktypes.h"

/*
 * BoxOS ACPI subsystem public interface.
 *
 * All on-disk / firmware structures are packed per ACPI 6.5 §5.2.
 * Static asserts guarantee the layout matches the spec across compilers
 * and CPU revisions so a build never silently produces a misaligned
 * accessor.
 */

typedef enum {
    ACPI_OK = 0,
    ACPI_ERR_RSDP_NOT_FOUND = 1,
    ACPI_ERR_INVALID_RSDP = 2,
    ACPI_ERR_RSDT_NOT_FOUND = 3,
    ACPI_ERR_FADT_NOT_FOUND = 4,
    ACPI_ERR_INVALID_TABLE = 5,
    ACPI_ERR_DSDT_NOT_FOUND = 6,
    ACPI_ERR_S5_NOT_FOUND = 7,
    ACPI_ERR_MAP_FAILED = 8,
    ACPI_ERR_NO_PM1A = 9
} acpi_error_t;

/* ACPI 6.5 §5.2.5.3 — Root System Description Pointer.
 * V1 (rev=0) is 20 bytes; V2 (rev=2+) is the full 36-byte structure with
 * a `length` field that may grow in future revisions. */
typedef struct {
    char signature[8];        /* "RSD PTR " */
    uint8_t checksum;         /* V1 checksum (sum of first 20 bytes == 0) */
    char oem_id[6];
    uint8_t revision;         /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_address;
    uint32_t length;          /* total RSDP length when rev >= 2 */
    uint64_t xsdt_address;
    uint8_t extended_checksum;/* V2 checksum (sum of first `length` bytes == 0) */
    uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp_t;
_Static_assert(sizeof(acpi_rsdp_t) == 36, "acpi_rsdp_t must be 36 bytes (ACPI 6.5 §5.2.5.3)");

/* ACPI 6.5 §5.2.6 — System Description Table Header. */
typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;         /* sum of `length` bytes must equal 0 */
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;
_Static_assert(sizeof(acpi_sdt_header_t) == 36, "acpi_sdt_header_t must be 36 bytes");

typedef struct {
    acpi_sdt_header_t header;
    uint32_t entries[];
} __attribute__((packed)) acpi_rsdt_t;

typedef struct {
    acpi_sdt_header_t header;
    uint64_t entries[];
} __attribute__((packed)) acpi_xsdt_t;

/* ACPI 6.5 §5.2.3.2 — Generic Address Structure. */
typedef struct {
    uint8_t address_space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed)) acpi_gas_t;
_Static_assert(sizeof(acpi_gas_t) == 12, "acpi_gas_t must be 12 bytes");

/* ACPI 6.5 §5.2.9 — Fixed ACPI Description Table.
 * Length varies: 116 bytes on ACPI 1.0 (rev=1), 244+ bytes on ACPI 2.0+
 * (rev=3+). All `x_*` and `reset_reg` fields are extensions and must
 * be guarded by a FADT-length check before access. */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved1;
    uint8_t preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command_port;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t pm1_event_length;
    uint8_t pm1_control_length;
    uint8_t pm2_control_length;
    uint8_t pm_timer_length;
    uint8_t gpe0_length;
    uint8_t gpe1_length;
    uint8_t gpe1_base;
    uint8_t cstate_control;
    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alarm;
    uint8_t month_alarm;
    uint8_t century;
    uint16_t boot_arch_flags;
    uint8_t reserved2;
    uint32_t flags;
    acpi_gas_t reset_reg;
    uint8_t reset_value;
    uint8_t reserved3[3];
    uint64_t x_firmware_control;
    uint64_t x_dsdt;
    acpi_gas_t x_pm1a_event_block;
    acpi_gas_t x_pm1b_event_block;
    acpi_gas_t x_pm1a_control_block;
    acpi_gas_t x_pm1b_control_block;
    acpi_gas_t x_pm2_control_block;
    acpi_gas_t x_pm_timer_block;
    acpi_gas_t x_gpe0_block;
    acpi_gas_t x_gpe1_block;
} __attribute__((packed)) acpi_fadt_t;
_Static_assert(sizeof(acpi_fadt_t) >= 244, "acpi_fadt_t too short for ACPI 2.0");

/* ============================================================
 * HPET (Intel HPET Specification 1.0a, IA-PC HPET ACPI table)
 * ============================================================ */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t event_timer_block_id;   /* bits 31:16 vendor, 15:8 num timers, etc. */
    acpi_gas_t base_address;          /* MMIO base of the HPET registers */
    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} __attribute__((packed)) acpi_hpet_t;
_Static_assert(sizeof(acpi_hpet_t) == 56, "acpi_hpet_t must be 56 bytes");

typedef struct {
    uintptr_t base;             /* MMIO physical base address */
    uint16_t  vendor_id;
    uint8_t   comparator_count; /* number of comparators (n+1 where n is HW field) */
    uint8_t   counter_size_64;  /* 1 if 64-bit main counter, else 32-bit */
    uint8_t   legacy_replacement;/* LegacyReplacement-capable */
    uint8_t   hpet_number;
    uint16_t  minimum_tick;
    bool      present;
} acpi_hpet_info_t;

/* ============================================================
 * MCFG (PCI Firmware Specification 3.0 §4.1.2)
 * ============================================================ */
typedef struct {
    uint64_t base_address;
    uint16_t segment_group;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
} __attribute__((packed)) acpi_mcfg_segment_t;
_Static_assert(sizeof(acpi_mcfg_segment_t) == 16, "acpi_mcfg_segment_t must be 16 bytes");

typedef struct {
    acpi_sdt_header_t header;
    uint64_t reserved;
    acpi_mcfg_segment_t segments[];
} __attribute__((packed)) acpi_mcfg_t;

#define ACPI_MCFG_MAX_SEGMENTS 16

typedef struct {
    acpi_mcfg_segment_t segments[ACPI_MCFG_MAX_SEGMENTS];
    uint8_t  count;
    bool     present;
} acpi_mcfg_info_t;

/* ============================================================
 * SRAT (Static Resource Affinity Table) — ACPI 6.5 §5.2.16
 * ============================================================ */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t reserved1;     /* must be 1 per spec */
    uint64_t reserved2;
} __attribute__((packed)) acpi_srat_t;

#define SRAT_TYPE_LOCAL_APIC   0
#define SRAT_TYPE_MEMORY       1
#define SRAT_TYPE_LOCAL_X2APIC 2

typedef struct {
    uint8_t  type;
    uint8_t  length;
} __attribute__((packed)) srat_entry_header_t;

/* Type 0 — Processor Local APIC/SAPIC Affinity, 16 bytes. */
typedef struct {
    srat_entry_header_t header;
    uint8_t  lo_domain;
    uint8_t  apic_id;
    uint32_t flags;             /* bit 0 = enabled */
    uint8_t  sapic_eid;
    uint8_t  hi_domain[3];      /* bits 8..31 of proximity domain */
    uint32_t clock_domain;
} __attribute__((packed)) srat_local_apic_t;
_Static_assert(sizeof(srat_local_apic_t) == 16, "SRAT Type 0 = 16 bytes");

/* Type 1 — Memory Affinity, 40 bytes. */
typedef struct {
    srat_entry_header_t header;
    uint32_t domain;
    uint16_t reserved1;
    uint64_t base_address;
    uint64_t length;
    uint32_t reserved2;
    uint32_t flags;             /* bit 0=enabled, bit 1=hot-pluggable, bit 2=non-volatile */
    uint64_t reserved3;
} __attribute__((packed)) srat_memory_t;
_Static_assert(sizeof(srat_memory_t) == 40, "SRAT Type 1 = 40 bytes");

/* Type 2 — Processor Local x2APIC Affinity, 24 bytes. */
typedef struct {
    srat_entry_header_t header;
    uint16_t reserved1;
    uint32_t domain;
    uint32_t x2apic_id;
    uint32_t flags;             /* bit 0 = enabled */
    uint32_t clock_domain;
    uint32_t reserved2;
} __attribute__((packed)) srat_local_x2apic_t;
_Static_assert(sizeof(srat_local_x2apic_t) == 24, "SRAT Type 2 = 24 bytes");

#define SRAT_FLAG_ENABLED       (1u << 0)
#define SRAT_MEM_FLAG_HOTPLUG   (1u << 1)
#define SRAT_MEM_FLAG_NONVOL    (1u << 2)

#define ACPI_NUMA_MAX_DOMAINS   32
#define ACPI_NUMA_MAX_CPUS      256
#define ACPI_NUMA_MAX_MEM_RANGES 64

typedef struct {
    uint32_t apic_id;       /* x2APIC ID (also fits 8-bit LAPIC IDs) */
    uint32_t domain;
    bool     enabled;
} acpi_numa_cpu_t;

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t domain;
    uint32_t flags;
} acpi_numa_mem_t;

typedef struct {
    acpi_numa_cpu_t cpus[ACPI_NUMA_MAX_CPUS];
    uint16_t        cpu_count;        /* up to ACPI_NUMA_MAX_CPUS (256) */
    acpi_numa_mem_t mem[ACPI_NUMA_MAX_MEM_RANGES];
    uint8_t         mem_count;
    uint8_t         domain_count;     /* unique domain IDs observed */
    uint32_t        domains[ACPI_NUMA_MAX_DOMAINS];
    bool            present;
} acpi_numa_info_t;

/* ============================================================
 * SLIT (System Locality Information Table) — ACPI 6.5 §5.2.17
 * ============================================================ */
typedef struct {
    acpi_sdt_header_t header;
    uint64_t locality_count;
    /* uint8_t matrix[locality_count * locality_count]; */
} __attribute__((packed)) acpi_slit_t;

typedef struct {
    uint8_t  locality_count;
    /* Flattened row-major matrix: distance[i*N + j]. NULL when absent. */
    uint8_t  matrix[ACPI_NUMA_MAX_DOMAINS * ACPI_NUMA_MAX_DOMAINS];
    bool     present;
} acpi_slit_info_t;

/* ============================================================
 * DMAR (Intel VT-d) — VT-d Spec §8.1
 * ============================================================ */
typedef struct {
    acpi_sdt_header_t header;
    uint8_t  host_address_width;   /* max guest address width minus 1 */
    uint8_t  flags;
    uint8_t  reserved[10];
} __attribute__((packed)) acpi_dmar_t;
_Static_assert(sizeof(acpi_dmar_t) == 48, "DMAR header = 48 bytes");

#define DMAR_TYPE_DRHD   0
#define DMAR_TYPE_RMRR   1
#define DMAR_TYPE_ATSR   2
#define DMAR_TYPE_RHSA   3
#define DMAR_TYPE_ANDD   4

typedef struct {
    uint16_t type;
    uint16_t length;
} __attribute__((packed)) dmar_entry_header_t;

typedef struct {
    dmar_entry_header_t header;
    uint8_t  flags;             /* bit 0 = INCLUDE_PCI_ALL */
    uint8_t  reserved;
    uint16_t segment;
    uint64_t register_base;
} __attribute__((packed)) dmar_drhd_t;
_Static_assert(sizeof(dmar_drhd_t) == 16, "DMAR DRHD header = 16 bytes");

#define ACPI_DMAR_MAX_DRHD  8

typedef struct {
    uint64_t register_base;
    uint16_t segment;
    bool     include_pci_all;
} acpi_drhd_info_t;

typedef struct {
    uint8_t  host_address_width_bits;   /* HAW value + 1 */
    uint8_t  flags;
    acpi_drhd_info_t drhd[ACPI_DMAR_MAX_DRHD];
    uint8_t  drhd_count;
    bool     present;
} acpi_dmar_info_t;

/* ============================================================
 * IVRS (AMD IOMMU) — AMD I/O Virtualization Tech Spec
 * ============================================================ */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t iv_info;
    uint64_t reserved;
} __attribute__((packed)) acpi_ivrs_t;
_Static_assert(sizeof(acpi_ivrs_t) == 48, "IVRS header = 48 bytes");

#define IVRS_BLOCK_IVHD_TYPE10  0x10
#define IVRS_BLOCK_IVHD_TYPE11  0x11
#define IVRS_BLOCK_IVHD_TYPE40  0x40

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint16_t length;
    uint16_t device_id;
    uint16_t capability_offset;
    uint64_t iommu_base;
    uint16_t pci_segment;
    uint16_t iommu_info;
    uint32_t iommu_feature;     /* Type 0x10 — feature reporting; 0x11/0x40 differ */
} __attribute__((packed)) ivrs_ivhd_t;
_Static_assert(sizeof(ivrs_ivhd_t) == 24, "IVRS IVHD core = 24 bytes");

#define ACPI_IVRS_MAX_IVHD  4

typedef struct {
    uint64_t iommu_base;
    uint16_t pci_segment;
    uint8_t  type;
} acpi_ivhd_info_t;

typedef struct {
    acpi_ivhd_info_t ivhd[ACPI_IVRS_MAX_IVHD];
    uint8_t  ivhd_count;
    bool     present;
} acpi_ivrs_info_t;

/* ============================================================
 * APEI tables — HEST/BERT/ERST — ACPI 6.5 §18
 * ============================================================ */
typedef struct {
    acpi_sdt_header_t header;
    uint32_t error_source_count;
} __attribute__((packed)) acpi_hest_t;

typedef struct {
    acpi_sdt_header_t header;
    uint32_t region_length;
    uint64_t region_address;
} __attribute__((packed)) acpi_bert_t;

typedef struct {
    acpi_sdt_header_t header;
    uint32_t serialization_header_size;
    uint32_t reserved;
    uint32_t instruction_entry_count;
} __attribute__((packed)) acpi_erst_t;

typedef struct {
    uint32_t hest_error_source_count;
    uint64_t bert_region_address;
    uint32_t bert_region_length;
    uint32_t erst_instruction_count;
    bool     hest_present;
    bool     bert_present;
    bool     erst_present;
} acpi_apei_info_t;

/* ============================================================
 * AML opcodes — minimal set for _S5 parsing
 * ============================================================ */
#define AML_SCOPE_OP        0x10
#define AML_NAME_OP         0x08
#define AML_PACKAGE_OP      0x12
#define AML_BYTE_PREFIX     0x0A
#define AML_WORD_PREFIX     0x0B
#define AML_DWORD_PREFIX    0x0C

acpi_error_t acpi_init(void);
void acpi_shutdown(void) __attribute__((noreturn));


void acpi_reboot(void) __attribute__((noreturn));
void acpi_print_info(void);

/* Surface any pre-boot hardware error recorded by firmware (APEI BERT
 * region) plus a summary of HEST / ERST visibility. Safe no-op when
 * the APEI tables are absent. Intended for kernel boot dmesg. */
void acpi_apei_consume(void);

/* Register and unmask the System Control Interrupt (SCI) on the GSI
 * announced by FADT.sci_interrupt. Subsequent SCI events (power button,
 * GPE, GHES SCI-class notifications) land in a kernel handler that
 * clears PM1 status bits + walks the active GPE block. Safe no-op
 * when ACPI is not initialised or sci_interrupt is zero. */
void acpi_sci_register(void);

/* Unmask the SCI — after Touch is up (guide_init) and the APEI runtime is
 * initialised: the handler says its events under names it must already hold,
 * and an interrupt cannot resolve one. A level-triggered SCI raised before
 * this waits in the IOAPIC. Also arms the power button. */
void acpi_sci_arm(void);

/* Enter ACPI sleep state `s` ∈ {1..5}. Reads \_Sx package from AML
 * namespace to derive SLP_TYPa/SLP_TYPb, calls _PTS(s) and _BFS(s)
 * (when present) before writing PM1a/b CNT with SLP_TYP + SLP_EN.
 * S5 is implemented via acpi_shutdown(); other states return.
 * Returns 0 on success or negative on missing prerequisites. */
int acpi_enter_sleep(uint8_t state);

/* GPE (General Purpose Event) handler registry. Each GPE bit may have
 * one C-callback. When the SCI handler observes the bit fire, the
 * callback runs at IRQ context — keep it short or queue work elsewhere.
 *
 * `gpe` is the global GPE index: 0..(gpe0_length/2*8 - 1) live in GPE0
 * block, the rest in GPE1.
 *
 * Future AML interpreter audit will wire the firmware-defined
 * `\_GPE._Lxx` / `\_GPE._Exx` methods through the same dispatcher by
 * registering an AML-callback wrapper as the handler. */
typedef void (*acpi_gpe_handler_t)(uint16_t gpe);
#define ACPI_MAX_GPES  256
int  acpi_gpe_register(uint16_t gpe, acpi_gpe_handler_t cb);
void acpi_gpe_unregister(uint16_t gpe);

/* Subsystem accessors. Return NULL/false until acpi_init() succeeds. */
const acpi_hpet_info_t *acpi_get_hpet(void);
const acpi_mcfg_info_t *acpi_get_mcfg(void);
const acpi_numa_info_t *acpi_get_numa(void);
const acpi_slit_info_t *acpi_get_slit(void);

/* Look up the NUMA proximity domain that owns `phys`. Returns the domain
 * ID on hit, ACPI_NUMA_DOMAIN_UNKNOWN otherwise (e.g. no SRAT, address
 * outside every enabled SRAT memory range). Cheap linear scan — the
 * memory range table is bounded to ACPI_NUMA_MAX_MEM_RANGES (64) and
 * lives in g_acpi. Future NUMA-aware PMM consumes this directly. */
#define ACPI_NUMA_DOMAIN_UNKNOWN  0xFFFFFFFFu
uint32_t acpi_numa_domain_for_phys(uint64_t phys);

/* Look up the NUMA proximity domain that owns the CPU with the given
 * full-width APIC ID (xAPIC 8-bit or x2APIC 32-bit). Returns the domain ID
 * on hit, ACPI_NUMA_DOMAIN_UNKNOWN otherwise (no SRAT, CPU not in SRAT, or
 * SRAT marked the CPU disabled). Used by per-K-Core subsystems
 * (ManifestStage scratch, Brook ring placement) to allocate local memory. */
uint32_t acpi_numa_domain_for_apic(uint32_t apic_id);
const acpi_dmar_info_t *acpi_get_dmar(void);
const acpi_ivrs_info_t *acpi_get_ivrs(void);
const acpi_apei_info_t *acpi_get_apei(void);

#endif // ACPI_H
