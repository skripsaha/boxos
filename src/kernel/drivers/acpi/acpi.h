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

/* Subsystem accessors. Return NULL/false until acpi_init() succeeds. */
const acpi_hpet_info_t *acpi_get_hpet(void);
const acpi_mcfg_info_t *acpi_get_mcfg(void);

#endif // ACPI_H
