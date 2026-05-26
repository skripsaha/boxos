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

    /* ACPI Power Management Timer (ACPI 6.5 §4.8.3.3).
     *
     * A free-running 24 or 32-bit counter clocked at exactly 3.579545
     * MHz. Reachable via I/O port `pm_timer_io` (legacy) or via the
     * SystemIO/SystemMemory GAS in FADT.X_PM_TIMER_BLOCK. Width is
     * 32-bit when FADT.flags bit 8 (TMR_VAL_EXT) is set, else 24-bit.
     *
     * Used as a TSC calibration source when HPET is absent or untrust-
     * worthy (some pre-2010 server boards ship without HPET; some
     * QEMU/Bochs builds disable HPET emulation). 0 == not present. */
    uint32_t pm_timer_io;       /* I/O port (legacy) or 0 if MMIO */
    uint8_t  pm_timer_bits;     /* 24 or 32 */
    bool     pm_timer_present;

    /* _S5 sleep type values for PM1 control word. */
    uint16_t slp_typa;
    uint16_t slp_typb;
    bool s5_found;

    /* Optional tables — parsed during acpi_init, kept zero/false if absent. */
    acpi_hpet_info_t hpet;
    acpi_mcfg_info_t mcfg;
    acpi_numa_info_t numa;
    acpi_slit_info_t slit;
    acpi_dmar_info_t dmar;
    acpi_ivrs_info_t ivrs;
    acpi_apei_info_t apei;

    bool initialized;
} acpi_state_t;

extern acpi_state_t g_acpi;

/* RSDP discovery + table mapping. */
acpi_rsdp_t* acpi_find_rsdp(void);
acpi_error_t acpi_parse_tables(acpi_rsdp_t* rsdp);

/* AML _S5 extraction from a single loaded table buffer (DSDT or SSDT body).
 * `aml` points at the bytes immediately after the table header. */
bool acpi_search_s5_in_aml(uint8_t* aml, uint32_t aml_len);

/* HPET / MCFG / NUMA / IOMMU / APEI parsers — called from acpi_parse_tables. */
void acpi_parse_hpet(void);
void acpi_parse_mcfg(void);
void acpi_parse_srat(void);
void acpi_parse_slit(void);
void acpi_parse_dmar(void);
void acpi_parse_ivrs(void);
void acpi_parse_apei(void);
void acpi_apei_consume(void);
void acpi_dmar_probe_registers(void);

/* ERST runtime executor. Implementation in acpi_erst.c. */
void acpi_erst_bind(acpi_erst_t* erst);
int  erst_run_action(uint8_t action, uint64_t in_value, uint64_t* out_value);
int  erst_write_record(uint64_t record_id, uint64_t* status_out);
int  erst_read_record(uint64_t record_id, uint64_t* status_out);
int  erst_clear_record(uint64_t record_id, uint64_t* status_out);
uint64_t erst_get_record_count(void);

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
