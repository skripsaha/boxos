#include "acpi_internal.h"
#include "klib.h"

/*
 * MCFG (PCI Express Memory-Mapped Configuration Space) parser.
 *
 * Reference: PCI Firmware Specification 3.0 §4.1.2.
 * Layout:
 *   acpi_sdt_header_t   header     (signature "MCFG")
 *   uint64_t            reserved   (must be zero per spec)
 *   acpi_mcfg_segment_t segments[] (16 bytes each)
 *     uint64_t base_address
 *     uint16_t segment_group
 *     uint8_t  start_bus
 *     uint8_t  end_bus
 *     uint32_t reserved
 *
 * MCFG describes the Enhanced Configuration Access Mechanism (ECAM) MMIO
 * region for each PCI segment group. Without it, code can still poke
 * legacy PCI config space at 0xCF8/0xCFC, but extended config (offset
 * 0x100-0xFFF) — required for MSI-X tables, PCIe capability registers,
 * AER and ASPM control — is unreachable. The ECAM layout per spec is:
 *
 *   ecam_va(seg, bus, dev, fn, off) =
 *     segments[seg].base_address +
 *     ((bus - start_bus) << 20) + (dev << 15) + (fn << 12) + off
 *
 * The PCI subsystem driver (separate audit) consumes acpi_get_mcfg() to
 * pick ECAM in preference to 0xCF8/0xCFC when present.
 */

void acpi_parse_mcfg(void) {
    g_acpi.mcfg.present = false;
    g_acpi.mcfg.count   = 0;

    acpi_mcfg_t* mcfg = (acpi_mcfg_t*)acpi_find_table("MCFG");
    if (!mcfg) {
        debug_printf("[ACPI] No MCFG table — PCI must use legacy 0xCF8/0xCFC\n");
        return;
    }

    uint32_t hdr_with_reserved = (uint32_t)sizeof(acpi_sdt_header_t) + 8u;
    if (mcfg->header.length < hdr_with_reserved) {
        debug_printf("[ACPI] MCFG too short (len=%u)\n", mcfg->header.length);
        return;
    }

    uint32_t bytes_of_segments = mcfg->header.length - hdr_with_reserved;
    if (bytes_of_segments % sizeof(acpi_mcfg_segment_t) != 0) {
        debug_printf("[ACPI] MCFG segment array %u bytes not a multiple of %lu\n",
                     bytes_of_segments,
                     (unsigned long)sizeof(acpi_mcfg_segment_t));
        return;
    }

    uint32_t total = bytes_of_segments / (uint32_t)sizeof(acpi_mcfg_segment_t);
    if (total > ACPI_MCFG_MAX_SEGMENTS) {
        debug_printf("[ACPI] MCFG declares %u segments, clamping to %u\n",
                     total, ACPI_MCFG_MAX_SEGMENTS);
        total = ACPI_MCFG_MAX_SEGMENTS;
    }

    for (uint32_t i = 0; i < total; i++) {
        acpi_mcfg_segment_t* s = &mcfg->segments[i];
        if (s->base_address == 0 || s->end_bus < s->start_bus) {
            debug_printf("[ACPI] MCFG segment %u invalid (base=0x%lx bus %u-%u), skipping\n",
                         i, (unsigned long)s->base_address,
                         s->start_bus, s->end_bus);
            continue;
        }
        g_acpi.mcfg.segments[g_acpi.mcfg.count] = *s;
        g_acpi.mcfg.count++;
        debug_printf("[ACPI] MCFG seg %u: group=%u base=0x%lx bus %u..%u\n",
                     g_acpi.mcfg.count - 1,
                     s->segment_group,
                     (unsigned long)s->base_address,
                     s->start_bus, s->end_bus);
    }

    g_acpi.mcfg.present = (g_acpi.mcfg.count > 0);
}

const acpi_mcfg_info_t *acpi_get_mcfg(void) {
    return g_acpi.mcfg.present ? &g_acpi.mcfg : NULL;
}
