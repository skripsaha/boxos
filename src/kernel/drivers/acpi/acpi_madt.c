#include "acpi_madt.h"
#include "acpi_internal.h"
#include "ioapic.h"
#include "cpuid.h"
#include "klib.h"
#include "vmm.h"

/* ACPI 6.5 §5.2.12 entry-length expectations. A real BIOS that publishes a
 * shorter entry of a known type is broken; we skip such entries rather
 * than misread garbage past their declared end. */
static uint8_t expected_entry_length(uint8_t type) {
    switch (type) {
        case MADT_TYPE_LOCAL_APIC:        return sizeof(madt_local_apic_t);
        case MADT_TYPE_IO_APIC:           return sizeof(madt_io_apic_t);
        case MADT_TYPE_ISO:               return sizeof(madt_iso_t);
        case MADT_TYPE_NMI_SOURCE:        return 8;   /* spec fixed */
        case MADT_TYPE_LOCAL_APIC_NMI:    return sizeof(madt_lapic_nmi_t);
        case MADT_TYPE_LAPIC_OVERRIDE:    return sizeof(madt_lapic_override_t);
        case MADT_TYPE_LX2APIC:           return sizeof(madt_lx2apic_t);
        case MADT_TYPE_LX2APIC_NMI:       return sizeof(madt_lx2apic_nmi_t);
        default:                          return 0;   /* type unknown to us */
    }
}

/*
 * ACPI 6.5 §5.2.12.2: the OSPM determines the BSP at runtime — it is the
 * processor that is currently executing OS init code. CPUID(1).EBX[31:24]
 * yields the local APIC ID of the executing CPU on every x86_64 part that
 * supports the APIC, which is universal for our target hardware (real CPUs
 * shipping with x86_64 always have an integrated LAPIC). Using this
 * instead of "first enabled MADT entry" avoids the trap of MADTs whose
 * Local APIC list is sorted by ACPI processor ID rather than by which
 * core booted first.
 */
static uint8_t detect_bsp_apic_id(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(CPUID_LEAF_FEATURES, &eax, &ebx, &ecx, &edx);
    return (uint8_t)((ebx >> 24) & 0xFF);
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
    info->bsp_lapic_id = detect_bsp_apic_id();
    info->bsp_lapic_found = true;
    debug_printf("[MADT] BSP LAPIC ID (CPUID.1.EBX[31:24]): %u\n",
                 info->bsp_lapic_id);

    uint8_t* ptr = (uint8_t*)madt + sizeof(acpi_madt_t);
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    while (ptr + sizeof(madt_entry_header_t) <= end) {
        madt_entry_header_t* entry = (madt_entry_header_t*)ptr;

        if (entry->length < sizeof(madt_entry_header_t)) {
            debug_printf("[MADT] Entry length %u below header, stopping\n",
                         entry->length);
            break;
        }
        if (ptr + entry->length > end) {
            debug_printf("[MADT] Entry extends past table end, stopping\n");
            break;
        }

        uint8_t expected = expected_entry_length(entry->type);
        if (expected != 0 && entry->length < expected) {
            debug_printf("[MADT] Type %u length %u < spec %u, skipping\n",
                         entry->type, entry->length, expected);
            ptr += entry->length;
            continue;
        }

        switch (entry->type) {
            case MADT_TYPE_LOCAL_APIC: {
                madt_local_apic_t* lapic = (madt_local_apic_t*)entry;
                bool enabled = (lapic->flags & MADT_LAPIC_ENABLED) ||
                               (lapic->flags & MADT_LAPIC_ONLINE_CAP);
                debug_printf("[MADT]   LAPIC: ACPI ID=%u, APIC ID=%u, %s\n",
                             lapic->acpi_processor_id, lapic->apic_id,
                             enabled ? "enabled" : "disabled");
                /* Resolve BSP ACPI processor ID by matching the APIC ID
                 * that CPUID reported for the running CPU. */
                if (info->bsp_lapic_found &&
                    !info->bsp_acpi_id_resolved &&
                    lapic->apic_id == info->bsp_lapic_id) {
                    info->bsp_acpi_processor_id = lapic->acpi_processor_id;
                    info->bsp_acpi_id_resolved  = true;
                }
                if (info->cpu_map_count < MADT_MAX_CPU_MAP) {
                    madt_cpu_map_t* m = &info->cpu_map[info->cpu_map_count++];
                    m->apic_id           = lapic->apic_id;
                    m->acpi_processor_id = lapic->acpi_processor_id;
                    m->enabled           = enabled;
                }
                break;
            }

            case MADT_TYPE_IO_APIC: {
                madt_io_apic_t* ioapic = (madt_io_apic_t*)entry;
                debug_printf("[MADT]   IO-APIC: ID=%u, addr=0x%x, GSI base=%u\n",
                             ioapic->io_apic_id,
                             ioapic->io_apic_address,
                             ioapic->gsi_base);
                /* Use the first IO-APIC found. Multi-IOAPIC support is a
                 * future IOAPIC-subsystem audit. */
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
                ioapic_register_iso(iso->source, iso->gsi, iso->flags);
                break;
            }

            case MADT_TYPE_LOCAL_APIC_NMI: {
                madt_lapic_nmi_t* nmi = (madt_lapic_nmi_t*)entry;
                if (nmi->lint > 1) {
                    debug_printf("[MADT]   LAPIC NMI: invalid LINT%u, dropping\n",
                                 nmi->lint);
                    break;
                }
                if (info->nmi_count >= MADT_MAX_NMI_ENTRIES) {
                    debug_printf("[MADT]   LAPIC NMI: storage full, dropping\n");
                    break;
                }
                madt_nmi_entry_t* slot = &info->nmi[info->nmi_count++];
                slot->acpi_processor_id = nmi->acpi_processor_id;
                slot->lint              = nmi->lint;
                slot->mps_flags         = nmi->flags;
                slot->valid             = true;
                debug_printf("[MADT]   LAPIC NMI: proc=%u LINT%u flags=0x%04x stored\n",
                             nmi->acpi_processor_id, nmi->lint, nmi->flags);
                break;
            }

            case MADT_TYPE_LAPIC_OVERRIDE: {
                madt_lapic_override_t* ovr = (madt_lapic_override_t*)entry;
                debug_printf("[MADT]   LAPIC Override: addr=0x%lx\n",
                             ovr->local_apic_address);
                info->lapic_address = (uintptr_t)ovr->local_apic_address;
                break;
            }

            case MADT_TYPE_LX2APIC: {
                madt_lx2apic_t* x = (madt_lx2apic_t*)entry;
                bool enabled = (x->flags & MADT_LAPIC_ENABLED) ||
                               (x->flags & MADT_LAPIC_ONLINE_CAP);
                debug_printf("[MADT]   x2APIC: UID=%u APIC ID=%u %s\n",
                             x->acpi_processor_uid, x->x2apic_id,
                             enabled ? "enabled" : "disabled");
                if (info->bsp_lapic_found &&
                    !info->bsp_acpi_id_resolved &&
                    x->x2apic_id == info->bsp_lapic_id) {
                    info->bsp_acpi_processor_id =
                        (uint8_t)(x->acpi_processor_uid & 0xFF);
                    info->bsp_acpi_id_resolved = true;
                }
                if (info->cpu_map_count < MADT_MAX_CPU_MAP) {
                    madt_cpu_map_t* m = &info->cpu_map[info->cpu_map_count++];
                    m->apic_id           = x->x2apic_id;
                    m->acpi_processor_id = x->acpi_processor_uid;
                    m->enabled           = enabled;
                }
                break;
            }

            case MADT_TYPE_LX2APIC_NMI: {
                madt_lx2apic_nmi_t* n = (madt_lx2apic_nmi_t*)entry;
                if (n->lint > 1) break;
                if (info->nmi_count >= MADT_MAX_NMI_ENTRIES) break;
                madt_nmi_entry_t* slot = &info->nmi[info->nmi_count++];
                /* Truncate UID to one byte for our processor-ID match
                 * — adequate until BoxOS supports >255 CPUs. */
                slot->acpi_processor_id = (n->acpi_processor_uid == 0xFFFFFFFFu)
                                          ? MADT_NMI_PROCESSOR_ALL
                                          : (uint8_t)(n->acpi_processor_uid & 0xFF);
                slot->lint      = n->lint;
                slot->mps_flags = n->flags;
                slot->valid     = true;
                debug_printf("[MADT]   x2APIC NMI: UID=%u LINT%u flags=0x%04x stored\n",
                             n->acpi_processor_uid, n->lint, n->flags);
                break;
            }

            case MADT_TYPE_NMI_SOURCE: {
                madt_nmi_source_t* src = (madt_nmi_source_t*)entry;
                if (info->nmi_source_count >= MADT_MAX_NMI_SOURCES) {
                    debug_printf("[MADT]   NMI Source: storage full, dropping\n");
                    break;
                }
                madt_nmi_source_info_t* slot =
                    &info->nmi_sources[info->nmi_source_count++];
                slot->gsi       = src->gsi;
                slot->mps_flags = src->flags;
                slot->valid     = true;
                debug_printf("[MADT]   NMI Source: GSI %u flags=0x%04x stored\n",
                             src->gsi, src->flags);
                break;
            }

            default:
                debug_printf("[MADT]   Type %u entry (len=%u) — not consumed here\n",
                             entry->type, entry->length);
                break;
        }

        ptr += entry->length;
    }

    if (info->lapic_address && info->ioapic_address) {
        info->valid = true;
        debug_printf("[MADT] %[S]MADT parsed: LAPIC=0x%lx IOAPIC=0x%lx BSP=%u NMI=%u%[D]\n",
                     info->lapic_address, info->ioapic_address,
                     info->bsp_lapic_id, info->nmi_count);
    } else {
        debug_printf("[MADT] %[E]MADT incomplete: LAPIC=0x%lx IOAPIC=0x%lx%[D]\n",
                     info->lapic_address, info->ioapic_address);
    }

    return info->valid ? ACPI_OK : ACPI_ERR_INVALID_TABLE;
}

uint8_t amp_collect_lapics(uint8_t* ids_out, uint8_t max_count) {
    if (!ids_out || max_count == 0) return 0;
    if (!g_acpi.initialized || !g_acpi.rsdp) return 0;

    acpi_madt_t* madt = (acpi_madt_t*)acpi_find_table("APIC");
    if (!madt) return 0;

    uint8_t count = 0;
    uint8_t* ptr = (uint8_t*)madt + sizeof(acpi_madt_t);
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    while (ptr + sizeof(madt_entry_header_t) <= end && count < max_count) {
        madt_entry_header_t* entry = (madt_entry_header_t*)ptr;
        if (entry->length < sizeof(madt_entry_header_t)) break;
        if (ptr + entry->length > end) break;

        if (entry->type == MADT_TYPE_LOCAL_APIC &&
            entry->length >= sizeof(madt_local_apic_t)) {
            madt_local_apic_t* lapic = (madt_local_apic_t*)entry;
            bool enabled = (lapic->flags & MADT_LAPIC_ENABLED) ||
                           (lapic->flags & MADT_LAPIC_ONLINE_CAP);
            if (enabled) {
                ids_out[count++] = lapic->apic_id;
            }
        }
        ptr += entry->length;
    }

    return count;
}
