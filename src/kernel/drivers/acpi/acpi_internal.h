#ifndef ACPI_INTERNAL_H
#define ACPI_INTERNAL_H

#include "acpi.h"

typedef struct {
    acpi_rsdp_t* rsdp;
    acpi_fadt_t* fadt;

    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;


    uint16_t slp_typa;
    uint16_t slp_typb;
    bool s5_found;

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

acpi_rsdp_t* acpi_find_rsdp(void);
acpi_error_t acpi_parse_tables(acpi_rsdp_t* rsdp);

bool acpi_search_s5_in_aml(uint8_t* aml, uint32_t aml_len);

void acpi_parse_hpet(void);
void acpi_parse_mcfg(void);
void acpi_parse_srat(void);
void acpi_parse_slit(void);
void acpi_parse_dmar(void);
void acpi_parse_ivrs(void);
void acpi_parse_apei(void);
void acpi_apei_consume(void);
void acpi_dmar_probe_registers(void);

void acpi_erst_bind(acpi_erst_t* erst);
int  erst_run_action(uint8_t action, uint64_t in_value, uint64_t* out_value);
int  erst_write_record(uint64_t record_id, uint64_t* status_out);
int  erst_read_record(uint64_t record_id, uint64_t* status_out);
int  erst_clear_record(uint64_t record_id, uint64_t* status_out);
uint64_t erst_get_record_count(void);

uint8_t acpi_checksum(void* data, size_t length);
volatile void* acpi_map_physical(uintptr_t phys_addr, size_t size);

bool acpi_validate_table(const acpi_sdt_header_t* header);

acpi_sdt_header_t* acpi_find_table(const char* signature);

typedef bool (*acpi_table_iter_cb_t)(acpi_sdt_header_t* tbl, void* user);
void acpi_for_each_table(const char* signature,
                         acpi_table_iter_cb_t cb,
                         void* user);

#endif