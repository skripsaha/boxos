#ifndef ACPI_INTERNAL_H
#define ACPI_INTERNAL_H

#include "acpi.h"

typedef struct {
    /* Root pointer + main fixed table */
    acpi_rsdp_t* rsdp;
    acpi_fadt_t* fadt;

    /* Resolved PM1 control ports (legacy SystemIO view, used for log/info). */
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;

    /* _S5 sleep type values for PM1 control word. */
    uint16_t slp_typa;
    uint16_t slp_typb;
    bool s5_found;

    /* Optional tables — parsed during acpi_init, kept zero/false if absent. */
    acpi_hpet_info_t hpet;
    acpi_mcfg_info_t mcfg;

    bool initialized;
} acpi_state_t;

extern acpi_state_t g_acpi;

/* RSDP discovery + table mapping. */
acpi_rsdp_t* acpi_find_rsdp(void);
acpi_error_t acpi_parse_tables(acpi_rsdp_t* rsdp);

/* AML _S5 extraction from a single loaded table buffer (DSDT or SSDT body).
 * `aml` points at the bytes immediately after the table header. */
bool acpi_search_s5_in_aml(uint8_t* aml, uint32_t aml_len);

/* HPET / MCFG parsers — called from acpi_parse_tables. */
void acpi_parse_hpet(void);
void acpi_parse_mcfg(void);

uint8_t acpi_checksum(void* data, size_t length);
volatile void* acpi_map_physical(uintptr_t phys_addr, size_t size);

/* Validate an SDT (signature + checksum + minimum length). */
bool acpi_validate_table(const acpi_sdt_header_t* header);

/* Find a table by signature in either XSDT (preferred when present per
 * ACPI 6.5 §5.2.5.3) or RSDT. Returns NULL if absent. */
acpi_sdt_header_t* acpi_find_table(const char* signature);

/* Iterate all tables of a given signature (e.g. multiple SSDTs).
 * `cb` is invoked for each match until it returns false or list exhausts. */
typedef bool (*acpi_table_iter_cb_t)(acpi_sdt_header_t* tbl, void* user);
void acpi_for_each_table(const char* signature,
                         acpi_table_iter_cb_t cb,
                         void* user);

#endif // ACPI_INTERNAL_H
