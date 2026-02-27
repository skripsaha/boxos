#ifndef ACPI_H
#define ACPI_H

#include "ktypes.h"

// ACPI Error Codes
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

// RSDP (Root System Description Pointer)
typedef struct {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

// SDT Header (common for all ACPI tables)
typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

// RSDT (Root System Description Table)
typedef struct {
    acpi_sdt_header_t header;
    uint32_t entries[];
} __attribute__((packed)) acpi_rsdt_t;

// XSDT (Extended System Description Table)
typedef struct {
    acpi_sdt_header_t header;
    uint64_t entries[];
} __attribute__((packed)) acpi_xsdt_t;

// Generic Address Structure
typedef struct {
    uint8_t address_space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed)) acpi_gas_t;

// FADT (Fixed ACPI Description Table)
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

// AML Opcodes (minimal set for _S5 parsing)
#define AML_SCOPE_OP        0x10
#define AML_NAME_OP         0x08
#define AML_PACKAGE_OP      0x12
#define AML_BYTE_PREFIX     0x0A
#define AML_WORD_PREFIX     0x0B
#define AML_DWORD_PREFIX    0x0C

// Public API
acpi_error_t acpi_init(void);
void acpi_shutdown(void) __attribute__((noreturn));
void acpi_print_info(void);

#endif // ACPI_H
