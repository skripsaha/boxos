#include "acpi_internal.h"
#include "klib.h"
#include "vmm.h"


void acpi_parse_dmar(void) {
    memset(&g_acpi.dmar, 0, sizeof(g_acpi.dmar));

    acpi_dmar_t* dmar = (acpi_dmar_t*)acpi_find_table("DMAR");
    if (!dmar) return;

    g_acpi.dmar.host_address_width_bits =
        (uint8_t)(dmar->host_address_width + 1);
    g_acpi.dmar.flags = dmar->flags;

    debug_printf("[ACPI] DMAR len=%u HAW=%u flags=0x%x\n",
                 dmar->header.length,
                 g_acpi.dmar.host_address_width_bits,
                 g_acpi.dmar.flags);

    uint8_t* ptr = (uint8_t*)dmar + sizeof(acpi_dmar_t);
    uint8_t* end = (uint8_t*)dmar + dmar->header.length;

    while (ptr + sizeof(dmar_entry_header_t) <= end) {
        dmar_entry_header_t* eh = (dmar_entry_header_t*)ptr;
        if (eh->length < sizeof(dmar_entry_header_t)) break;
        if (ptr + eh->length > end) break;

        switch (eh->type) {
            case DMAR_TYPE_DRHD: {
                if (eh->length < sizeof(dmar_drhd_t)) break;
                dmar_drhd_t* d = (dmar_drhd_t*)eh;
                if (g_acpi.dmar.drhd_count < ACPI_DMAR_MAX_DRHD) {
                    acpi_drhd_info_t* slot =
                        &g_acpi.dmar.drhd[g_acpi.dmar.drhd_count++];
                    slot->register_base   = d->register_base;
                    slot->segment         = d->segment;
                    slot->include_pci_all = (d->flags & 0x1) != 0;
                    debug_printf("[ACPI] DMAR DRHD[%u] seg=%u base=0x%lx %s\n",
                                 g_acpi.dmar.drhd_count - 1,
                                 d->segment,
                                 (unsigned long)d->register_base,
                                 slot->include_pci_all ? "[INCLUDE_PCI_ALL]" : "");
                }
                break;
            }
            case DMAR_TYPE_RMRR:
                debug_printf("[ACPI] DMAR RMRR (reserved memory, len=%u)\n",
                             eh->length);
                break;
            case DMAR_TYPE_ATSR:
                debug_printf("[ACPI] DMAR ATSR (ATS, len=%u)\n", eh->length);
                break;
            case DMAR_TYPE_RHSA:
                debug_printf("[ACPI] DMAR RHSA (NUMA affinity, len=%u)\n",
                             eh->length);
                break;
            case DMAR_TYPE_ANDD:
                debug_printf("[ACPI] DMAR ANDD (ACPI Namespace, len=%u)\n",
                             eh->length);
                break;
            default:
                debug_printf("[ACPI] DMAR type %u len=%u — ignored\n",
                             eh->type, eh->length);
                break;
        }
        ptr += eh->length;
    }

    g_acpi.dmar.present = (g_acpi.dmar.drhd_count > 0);
    debug_printf("[ACPI] DMAR parsed: %u DRHD(s)\n", g_acpi.dmar.drhd_count);
}

const acpi_dmar_info_t *acpi_get_dmar(void) {
    return g_acpi.dmar.present ? &g_acpi.dmar : NULL;
}

#define VTD_REG_VER       0x000
#define VTD_REG_CAP       0x008
#define VTD_REG_ECAP      0x010

void acpi_dmar_probe_registers(void) {
    if (!g_acpi.dmar.present) return;
    for (uint8_t i = 0; i < g_acpi.dmar.drhd_count; i++) {
        const acpi_drhd_info_t* d = &g_acpi.dmar.drhd[i];
        volatile uint8_t* base = (volatile uint8_t*)
            vmm_map_mmio((uintptr_t)d->register_base, 4096,
                          VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                          VMM_FLAG_CACHE_DISABLE);
        if (!base) {
            debug_printf("[DMAR] DRHD[%u] mmio map failed at 0x%lx\n",
                         i, (unsigned long)d->register_base);
            continue;
        }
        uint32_t ver  = *(volatile uint32_t*)(base + VTD_REG_VER);
        uint64_t cap  = *(volatile uint64_t*)(base + VTD_REG_CAP);
        uint64_t ecap = *(volatile uint64_t*)(base + VTD_REG_ECAP);
        debug_printf("[DMAR] DRHD[%u] VER=%u.%u CAP=0x%lx ECAP=0x%lx\n",
                     i,
                     (ver >> 4) & 0xF, ver & 0xF,
                     (unsigned long)cap, (unsigned long)ecap);
        debug_printf("[DMAR]   features:%s%s%s%s%s%s\n",
                     (ecap & (1ULL << 1))  ? " QI"   : "",
                     (ecap & (1ULL << 3))  ? " IR"   : "",
                     (ecap & (1ULL << 7))  ? " PT"   : "",
                     (ecap & (1ULL << 11)) ? " EAFS" : "",
                     (cap  & (1ULL << 7))  ? " PLMR" : "",
                     (cap  & (1ULL << 8))  ? " PHMR" : "");
    }
}