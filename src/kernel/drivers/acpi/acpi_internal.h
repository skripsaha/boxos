#ifndef ACPI_INTERNAL_H
#define ACPI_INTERNAL_H

#include "acpi.h"

// Global ACPI state
typedef struct {
    acpi_rsdp_t* rsdp;
    acpi_fadt_t* fadt;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint16_t slp_typa;
    uint16_t slp_typb;
    bool s5_found;
    bool initialized;
} acpi_state_t;

extern acpi_state_t g_acpi;

// Internal functions
acpi_rsdp_t* acpi_find_rsdp(void);
acpi_error_t acpi_parse_tables(acpi_rsdp_t* rsdp);
acpi_error_t acpi_extract_s5(uint32_t dsdt_addr, uint32_t dsdt_len);

// Helpers
uint8_t acpi_checksum(void* data, size_t length);
volatile void* acpi_map_physical(uintptr_t phys_addr, size_t size);

#endif // ACPI_INTERNAL_H
