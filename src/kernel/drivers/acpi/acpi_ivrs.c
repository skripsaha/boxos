#include "acpi_internal.h"
#include "klib.h"

/*
 * IVRS (I/O Virtualization Reporting Structure) parser
 *   — AMD I/O Virtualization Technology (IOMMU) Specification, §5.2.
 *
 * IVRS is the AMD analogue of Intel's DMAR. The first IVRS-block per
 * IOMMU is an IVHD (I/O Virtualization Hardware Definition). We record
 * one entry per IVHD so the future AMD IOMMU driver can find each
 * controller without re-parsing.
 *
 * Device entries inside an IVHD describe which PCI devices/aliases are
 * routed through this IOMMU — relevant only when a full driver is
 * present, so we skip them here.
 */

void acpi_parse_ivrs(void) {
    memset(&g_acpi.ivrs, 0, sizeof(g_acpi.ivrs));

    acpi_ivrs_t* ivrs = (acpi_ivrs_t*)acpi_find_table("IVRS");
    if (!ivrs) return;

    debug_printf("[ACPI] IVRS len=%u iv_info=0x%x\n",
                 ivrs->header.length, ivrs->iv_info);

    uint8_t* ptr = (uint8_t*)ivrs + sizeof(acpi_ivrs_t);
    uint8_t* end = (uint8_t*)ivrs + ivrs->header.length;

    while (ptr + sizeof(ivrs_ivhd_t) <= end) {
        ivrs_ivhd_t* h = (ivrs_ivhd_t*)ptr;
        if (h->length < sizeof(ivrs_ivhd_t)) break;
        if (ptr + h->length > end) break;

        if (h->type == IVRS_BLOCK_IVHD_TYPE10 ||
            h->type == IVRS_BLOCK_IVHD_TYPE11 ||
            h->type == IVRS_BLOCK_IVHD_TYPE40) {

            if (g_acpi.ivrs.ivhd_count < ACPI_IVRS_MAX_IVHD) {
                acpi_ivhd_info_t* slot =
                    &g_acpi.ivrs.ivhd[g_acpi.ivrs.ivhd_count++];
                slot->iommu_base  = h->iommu_base;
                slot->pci_segment = h->pci_segment;
                slot->type        = h->type;
                debug_printf("[ACPI] IVRS IVHD type=0x%02x seg=%u base=0x%lx\n",
                             h->type, h->pci_segment,
                             (unsigned long)h->iommu_base);
            }
        } else {
            debug_printf("[ACPI] IVRS block type=0x%02x len=%u — ignored\n",
                         h->type, h->length);
        }

        ptr += h->length;
    }

    g_acpi.ivrs.present = (g_acpi.ivrs.ivhd_count > 0);
    debug_printf("[ACPI] IVRS parsed: %u IVHD(s)\n", g_acpi.ivrs.ivhd_count);
}

const acpi_ivrs_info_t *acpi_get_ivrs(void) {
    return g_acpi.ivrs.present ? &g_acpi.ivrs : NULL;
}
