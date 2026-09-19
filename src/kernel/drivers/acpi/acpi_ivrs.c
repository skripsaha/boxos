#include "acpi_internal.h"
#include "klib.h"

/*
 * IVRS (I/O Virtualization Reporting Structure) parser
 *   — AMD I/O Virtualization Technology (IOMMU) Specification, 48882
 *     Rev 3.06-PUB, §5.2 (p.277) and §5.2.2.1 (p.279).
 *
 * IVRS is the AMD analogue of Intel's DMAR, and the unit it describes is an
 * IOMMU: "Each IOMMU in the system is described by one IVHD block" (§5.2.2.1).
 *
 * One IOMMU, several blocks
 * -------------------------
 * §5.2 defines THREE shapes for that one block -- "Types 10h and 11h which
 * support fixed DeviceID device entries only" and "Type 40h which adds support
 * for ACPI HID device entries" -- and firmware emits the same IOMMU in every
 * shape the platform can describe, so that software written before Type 40h
 * existed still finds something it understands: "The type 10h IVHD block is
 * defined for backward compatibility ... The IVHD Type11h contains all relevant
 * IOMMU feature information. It is recommended for system software to detect
 * IOMMU features from the fields in the IVHD Type11h structure information,
 * superseding information in Type10h block and MMIO registers."
 *
 * A typical Zen desktop board therefore carries 10h, 11h AND 40h for its single
 * IOMMU, all naming the same register block. This parser used to append a slot
 * for each of them, and amdvi_init built one unit per slot: three mappings of
 * one 16 KiB register block, three device tables of which two are never read,
 * and DEV_TAB_BAR / CMD_BUF_BAR / EVT_LOG_BAR programmed three times, so the
 * hardware ended up pointed at the last unit's buffers while the driver's own
 * first-match lookup wrote DTEs into the first unit's.
 *
 * So: one slot per IOMMU, recognised by the register block it owns, carrying
 * the highest block type the firmware published for it. Linux reaches the same
 * place from the other side -- get_highest_supported_ivhd_type() finds the
 * highest type among the blocks sharing the first block's DeviceID, and
 * init_iommu_all() then instantiates an IOMMU only for blocks of that type
 * (drivers/iommu/amd/init.c).
 *
 * Device entries inside an IVHD describe which PCI devices/aliases are
 * routed through this IOMMU — relevant only when a full driver is
 * present, so we skip them here.
 */

/* The IOMMU already recorded for this register block, or NULL for a new one.
 * An IOMMU is its MMIO block: §5.2.2.1 gives one base address per IVHD, and
 * "An IOMMU and the peripherals it serves must be on the same PCI Segment
 * defined in the IVHD block", so the segment travels with it. */
static acpi_ivhd_info_t* acpi_ivrs_unit_for(uint64_t iommu_base,
                                            uint16_t pci_segment) {
    for (uint8_t i = 0; i < g_acpi.ivrs.ivhd_count; i++) {
        acpi_ivhd_info_t* slot = &g_acpi.ivrs.ivhd[i];
        if (slot->iommu_base == iommu_base &&
            slot->pci_segment == pci_segment) return slot;
    }
    return NULL;
}

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

            uint64_t base = h->iommu_base;
            uint16_t seg  = h->pci_segment;
            acpi_ivhd_info_t* slot = acpi_ivrs_unit_for(base, seg);

            if (slot) {
                /* The same IOMMU said again in a different shape. Keep the
                 * highest type: 40h supersedes 11h, and 11h supersedes the
                 * backward-compatibility 10h. */
                if (h->type > slot->type) slot->type = h->type;
                debug_printf("[ACPI] IVRS IVHD type=0x%02x seg=%u base=0x%lx "
                             "— same IOMMU, kept as type=0x%02x\n",
                             h->type, seg, (unsigned long)base, slot->type);
            } else if (g_acpi.ivrs.ivhd_count < ACPI_IVRS_MAX_IVHD) {
                slot = &g_acpi.ivrs.ivhd[g_acpi.ivrs.ivhd_count++];
                slot->iommu_base  = base;
                slot->pci_segment = seg;
                slot->type        = h->type;
                debug_printf("[ACPI] IVRS IVHD type=0x%02x seg=%u base=0x%lx\n",
                             h->type, seg, (unsigned long)base);
            } else {
                /* Not a detail for a debug build to swallow: an IOMMU this
                 * kernel has no room for is an IOMMU nothing will drive, and
                 * every device behind it stays outside the translation the
                 * rest of the machine believes is in force. */
                kprintf("[ACPI] IVRS: no room for the IOMMU at seg=%u "
                        "base=0x%lx — this kernel holds %u and the firmware "
                        "names more; that IOMMU will NOT be driven\n",
                        seg, (unsigned long)base,
                        (unsigned)ACPI_IVRS_MAX_IVHD);
            }
        } else {
            debug_printf("[ACPI] IVRS block type=0x%02x len=%u — ignored\n",
                         h->type, h->length);
        }

        ptr += h->length;
    }

    g_acpi.ivrs.present = (g_acpi.ivrs.ivhd_count > 0);
    debug_printf("[ACPI] IVRS parsed: %u IOMMU(s)\n", g_acpi.ivrs.ivhd_count);
}

const acpi_ivrs_info_t *acpi_get_ivrs(void) {
    return g_acpi.ivrs.present ? &g_acpi.ivrs : NULL;
}
