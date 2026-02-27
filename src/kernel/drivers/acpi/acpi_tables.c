#include "acpi_internal.h"
#include "klib.h"
#include "vmm.h"

static bool acpi_validate_table(acpi_sdt_header_t* header) {
    if (!header) return false;

    uint8_t sum = acpi_checksum(header, header->length);
    if (sum != 0) {
        debug_printf("[ACPI] Table checksum failed for %.4s\n", header->signature);
        return false;
    }

    return true;
}

static acpi_fadt_t* acpi_find_fadt_in_rsdt(acpi_rsdt_t* rsdt) {
    uint32_t entry_count = (rsdt->header.length - sizeof(acpi_sdt_header_t)) / 4;

    debug_printf("[ACPI] RSDT contains %u entries\n", entry_count);

    for (uint32_t i = 0; i < entry_count; i++) {
        acpi_sdt_header_t* header = (acpi_sdt_header_t*)acpi_map_physical(rsdt->entries[i], sizeof(acpi_sdt_header_t));

        if (!header) {
            debug_printf("[ACPI] Failed to map table entry %u\n", i);
            continue;
        }

        if (memcmp(header->signature, "FACP", 4) == 0) {
            debug_printf("[ACPI] Found FADT at 0x%x\n", rsdt->entries[i]);

            acpi_fadt_t* fadt = (acpi_fadt_t*)acpi_map_physical(rsdt->entries[i], header->length);
            if (!fadt) {
                debug_printf("[ACPI] Failed to map FADT\n");
                return NULL;
            }

            if (!acpi_validate_table(&fadt->header)) {
                debug_printf("[ACPI] FADT validation failed\n");
                return NULL;
            }

            return fadt;
        }
    }

    return NULL;
}

static acpi_fadt_t* acpi_find_fadt_in_xsdt(acpi_xsdt_t* xsdt) {
    uint32_t entry_count = (xsdt->header.length - sizeof(acpi_sdt_header_t)) / 8;

    debug_printf("[ACPI] XSDT contains %u entries\n", entry_count);

    for (uint32_t i = 0; i < entry_count; i++) {
        acpi_sdt_header_t* header = (acpi_sdt_header_t*)acpi_map_physical(xsdt->entries[i], sizeof(acpi_sdt_header_t));

        if (!header) {
            debug_printf("[ACPI] Failed to map table entry %u\n", i);
            continue;
        }

        if (memcmp(header->signature, "FACP", 4) == 0) {
            debug_printf("[ACPI] Found FADT at 0x%lx\n", xsdt->entries[i]);

            acpi_fadt_t* fadt = (acpi_fadt_t*)acpi_map_physical(xsdt->entries[i], header->length);
            if (!fadt) {
                debug_printf("[ACPI] Failed to map FADT\n");
                return NULL;
            }

            if (!acpi_validate_table(&fadt->header)) {
                debug_printf("[ACPI] FADT validation failed\n");
                return NULL;
            }

            return fadt;
        }
    }

    return NULL;
}

acpi_error_t acpi_parse_tables(acpi_rsdp_t* rsdp) {
    if (!rsdp) return ACPI_ERR_INVALID_RSDP;

    acpi_fadt_t* fadt = NULL;

#ifdef CONFIG_ACPI_USE_XSDT
    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        debug_printf("[ACPI] Using XSDT at 0x%lx\n", rsdp->xsdt_address);
        acpi_xsdt_t* xsdt = (acpi_xsdt_t*)acpi_map_physical(rsdp->xsdt_address, sizeof(acpi_sdt_header_t));

        if (xsdt) {
            xsdt = (acpi_xsdt_t*)acpi_map_physical(rsdp->xsdt_address, xsdt->header.length);
            if (xsdt && acpi_validate_table(&xsdt->header)) {
                fadt = acpi_find_fadt_in_xsdt(xsdt);
            }
        }
    }
#endif

    if (!fadt && rsdp->rsdt_address) {
        debug_printf("[ACPI] Using RSDT at 0x%x\n", rsdp->rsdt_address);
        acpi_rsdt_t* rsdt = (acpi_rsdt_t*)acpi_map_physical(rsdp->rsdt_address, sizeof(acpi_sdt_header_t));

        if (!rsdt) {
            debug_printf("[ACPI] Failed to map RSDT\n");
            return ACPI_ERR_RSDT_NOT_FOUND;
        }

        rsdt = (acpi_rsdt_t*)acpi_map_physical(rsdp->rsdt_address, rsdt->header.length);
        if (!rsdt) {
            debug_printf("[ACPI] Failed to remap RSDT with full size\n");
            return ACPI_ERR_RSDT_NOT_FOUND;
        }

        if (!acpi_validate_table(&rsdt->header)) {
            debug_printf("[ACPI] RSDT validation failed\n");
            return ACPI_ERR_INVALID_TABLE;
        }

        fadt = acpi_find_fadt_in_rsdt(rsdt);
    }

    if (!fadt) {
        debug_printf("[ACPI] FADT not found\n");
        return ACPI_ERR_FADT_NOT_FOUND;
    }

    g_acpi.fadt = fadt;
    g_acpi.pm1a_cnt_blk = fadt->pm1a_control_block;
    g_acpi.pm1b_cnt_blk = fadt->pm1b_control_block;

    debug_printf("[ACPI] PM1a_CNT: 0x%x\n", g_acpi.pm1a_cnt_blk);
    debug_printf("[ACPI] PM1b_CNT: 0x%x\n", g_acpi.pm1b_cnt_blk);

    if (g_acpi.pm1a_cnt_blk == 0) {
        debug_printf("[ACPI] WARNING: PM1a_CNT not available\n");
    }

    uint32_t dsdt_addr = fadt->dsdt;
    if (dsdt_addr == 0 && rsdp->revision >= 2) {
        dsdt_addr = (uint32_t)fadt->x_dsdt;
    }

    if (dsdt_addr == 0) {
        debug_printf("[ACPI] DSDT address is NULL\n");
        return ACPI_ERR_DSDT_NOT_FOUND;
    }

    acpi_sdt_header_t* dsdt_header = (acpi_sdt_header_t*)acpi_map_physical(dsdt_addr, sizeof(acpi_sdt_header_t));
    if (!dsdt_header) {
        debug_printf("[ACPI] Failed to map DSDT header\n");
        return ACPI_ERR_DSDT_NOT_FOUND;
    }

    debug_printf("[ACPI] DSDT at 0x%x, length %u bytes\n", dsdt_addr, dsdt_header->length);

    acpi_error_t err = acpi_extract_s5(dsdt_addr, dsdt_header->length);
    if (err != ACPI_OK) {
        debug_printf("[ACPI] _S5 extraction failed, using fallback values\n");
        g_acpi.slp_typa = 0x05;
        g_acpi.slp_typb = 0x05;
        g_acpi.s5_found = false;
    }

    return ACPI_OK;
}
