#include "acpi_madt.h"
#include "acpi_internal.h"
#include "ioapic.h"
#include "klib.h"
#include "vmm.h"

static acpi_sdt_header_t* acpi_find_table_in_rsdt(const char* signature) {
    if (!g_acpi.rsdp || !g_acpi.rsdp->rsdt_address) return NULL;

    acpi_rsdt_t* rsdt = (acpi_rsdt_t*)acpi_map_physical(
        g_acpi.rsdp->rsdt_address, sizeof(acpi_sdt_header_t));
    if (!rsdt) return NULL;

    rsdt = (acpi_rsdt_t*)acpi_map_physical(
        g_acpi.rsdp->rsdt_address, rsdt->header.length);
    if (!rsdt) return NULL;

    uint32_t count = (rsdt->header.length - sizeof(acpi_sdt_header_t)) / 4;

    for (uint32_t i = 0; i < count; i++) {
        acpi_sdt_header_t* hdr = (acpi_sdt_header_t*)acpi_map_physical(
            rsdt->entries[i], sizeof(acpi_sdt_header_t));
        if (hdr && memcmp(hdr->signature, signature, 4) == 0) {
            return (acpi_sdt_header_t*)acpi_map_physical(
                rsdt->entries[i], hdr->length);
        }
    }
    return NULL;
}

static acpi_sdt_header_t* acpi_find_table_in_xsdt(const char* signature) {
    if (!g_acpi.rsdp || g_acpi.rsdp->revision < 2 || !g_acpi.rsdp->xsdt_address)
        return NULL;

    acpi_xsdt_t* xsdt = (acpi_xsdt_t*)acpi_map_physical(
        g_acpi.rsdp->xsdt_address, sizeof(acpi_sdt_header_t));
    if (!xsdt) return NULL;

    xsdt = (acpi_xsdt_t*)acpi_map_physical(
        g_acpi.rsdp->xsdt_address, xsdt->header.length);
    if (!xsdt) return NULL;

    uint32_t count = (xsdt->header.length - sizeof(acpi_sdt_header_t)) / 8;

    for (uint32_t i = 0; i < count; i++) {
        acpi_sdt_header_t* hdr = (acpi_sdt_header_t*)acpi_map_physical(
            xsdt->entries[i], sizeof(acpi_sdt_header_t));
        if (hdr && memcmp(hdr->signature, signature, 4) == 0) {
            return (acpi_sdt_header_t*)acpi_map_physical(
                xsdt->entries[i], hdr->length);
        }
    }
    return NULL;
}

static acpi_sdt_header_t* acpi_find_table(const char* signature) {
    acpi_sdt_header_t* table = NULL;

#ifdef CONFIG_ACPI_USE_XSDT
    table = acpi_find_table_in_xsdt(signature);
#endif

    if (!table) {
        table = acpi_find_table_in_rsdt(signature);
    }
    return table;
}

acpi_error_t acpi_parse_madt(madt_info_t* info) {
    if (!info) return ACPI_ERR_INVALID_TABLE;

    memset(info, 0, sizeof(madt_info_t));

    if (!g_acpi.initialized || !g_acpi.rsdp) {
        debug_printf("[MADT] ACPI not initialized, cannot parse MADT\n");
        return ACPI_ERR_INVALID_TABLE;
    }

    acpi_madt_t* madt = (acpi_madt_t*)acpi_find_table("APIC");
    if (!madt) {
        debug_printf("[MADT] MADT table not found\n");
        return ACPI_ERR_INVALID_TABLE;
    }

    debug_printf("[MADT] Found MADT: length=%u\n", madt->header.length);
    debug_printf("[MADT] Local APIC address: 0x%x\n", madt->local_apic_address);
    debug_printf("[MADT] Flags: 0x%x (PIC present: %s)\n",
                 madt->flags,
                 (madt->flags & MADT_FLAG_PCAT_COMPAT) ? "yes" : "no");

    info->lapic_address = madt->local_apic_address;
    info->has_pic = (madt->flags & MADT_FLAG_PCAT_COMPAT) != 0;

    // Walk MADT entries
    uint8_t* ptr = (uint8_t*)madt + sizeof(acpi_madt_t);
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    while (ptr < end) {
        madt_entry_header_t* entry = (madt_entry_header_t*)ptr;

        if (entry->length < 2) {
            debug_printf("[MADT] Invalid entry length %u, stopping\n", entry->length);
            break;
        }

        switch (entry->type) {
            case MADT_TYPE_LOCAL_APIC: {
                madt_local_apic_t* lapic = (madt_local_apic_t*)entry;
                bool enabled = (lapic->flags & MADT_LAPIC_ENABLED) ||
                               (lapic->flags & MADT_LAPIC_ONLINE_CAP);

                debug_printf("[MADT]   LAPIC: ACPI ID=%u, APIC ID=%u, %s\n",
                             lapic->acpi_processor_id, lapic->apic_id,
                             enabled ? "enabled" : "disabled");

                // First enabled LAPIC is BSP (0 is a valid APIC ID)
                if (enabled && !info->bsp_lapic_found) {
                    info->bsp_lapic_id = lapic->apic_id;
                    info->bsp_lapic_found = true;
                }
                break;
            }

            case MADT_TYPE_IO_APIC: {
                madt_io_apic_t* ioapic = (madt_io_apic_t*)entry;

                debug_printf("[MADT]   IO-APIC: ID=%u, addr=0x%x, GSI base=%u\n",
                             ioapic->io_apic_id,
                             ioapic->io_apic_address,
                             ioapic->gsi_base);

                // Use the first IO-APIC found
                if (info->ioapic_address == 0) {
                    info->ioapic_address = ioapic->io_apic_address;
                    info->ioapic_id = ioapic->io_apic_id;
                    info->ioapic_gsi_base = ioapic->gsi_base;
                }
                break;
            }

            case MADT_TYPE_ISO: {
                madt_iso_t* iso = (madt_iso_t*)entry;

                debug_printf("[MADT]   ISO: bus=%u, source IRQ %u -> GSI %u, flags=0x%04x\n",
                             iso->bus, iso->source, iso->gsi, iso->flags);

                // Register the override for IO-APIC routing
                ioapic_register_iso(iso->source, iso->gsi, iso->flags);
                break;
            }

            case MADT_TYPE_LOCAL_APIC_NMI: {
                madt_lapic_nmi_t* nmi = (madt_lapic_nmi_t*)entry;
                debug_printf("[MADT]   LAPIC NMI: proc=%u, LINT%u, flags=0x%04x\n",
                             nmi->acpi_processor_id, nmi->lint, nmi->flags);
                break;
            }

            case MADT_TYPE_LAPIC_OVERRIDE: {
                madt_lapic_override_t* ovr = (madt_lapic_override_t*)entry;
                debug_printf("[MADT]   LAPIC Override: addr=0x%lx\n",
                             ovr->local_apic_address);
                info->lapic_address = ovr->local_apic_address;
                break;
            }

            default:
                debug_printf("[MADT]   Unknown entry type %u (len=%u)\n",
                             entry->type, entry->length);
                break;
        }

        ptr += entry->length;
    }

    if (info->lapic_address && info->ioapic_address) {
        info->valid = true;
        debug_printf("[MADT] %[S]MADT parsed: LAPIC=0x%lx, IOAPIC=0x%lx, BSP=%u%[D]\n",
                     info->lapic_address, info->ioapic_address, info->bsp_lapic_id);
    } else {
        debug_printf("[MADT] %[E]MADT incomplete: LAPIC=0x%lx, IOAPIC=0x%lx%[D]\n",
                     info->lapic_address, info->ioapic_address);
    }

    return info->valid ? ACPI_OK : ACPI_ERR_INVALID_TABLE;
}
