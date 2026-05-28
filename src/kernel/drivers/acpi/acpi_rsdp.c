#include "acpi_internal.h"
#include "io.h"
#include "vmm.h"
#include "klib.h"
#include "boot_info.h"

/* ACPI 6.5 §5.2.5.1 — IA-PC RSDP search locations.
 * Two regions; both must be scanned on 16-byte boundaries:
 *   (a) the first 1 KB of the Extended BIOS Data Area (EBDA);
 *   (b) the BIOS read-only memory region 0xE0000-0xFFFFF.
 */
#define RSDP_SCAN_STEP_BYTES   16u
#define EBDA_SCAN_BYTES        1024u
#define BIOS_ROM_BEGIN         0xE0000u
#define BIOS_ROM_END           0x100000u
/* The EBDA pointer is held at physical 0x40E (segment, shift left 4 to
 * obtain the 20-bit physical address). Frozen PC/AT firmware contract. */
#define BDA_EBDA_SEG_PTR       0x40Eu
#define LOWMEM_END             0xA0000u

/* RSDP revision sanity. Only values currently defined by ACPI 1.0 (0)
 * and ACPI 2.0+ (2) are valid. Real BIOSes occasionally ship 0xFF when
 * the firmware writer never initialised the field. */
static bool rsdp_revision_known(uint8_t rev) {
    return rev == 0 || rev == 2;
}

/* Verify the RSDP signature and BOTH checksums where applicable.
 *
 * ACPI 6.5 §5.2.5.3:
 *   - V1 (rev == 0): sum of the first 20 bytes must equal 0.
 *   - V2 (rev >= 2): the V1 checksum still applies, AND the sum of
 *     `length` bytes (read from the structure itself, not a hard-coded
 *     36) must equal 0. Future revisions may grow the structure; honour
 *     `length`.
 */
static bool validate_rsdp(acpi_rsdp_t* rsdp) {
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0)
        return false;

    if (!rsdp_revision_known(rsdp->revision)) {
        debug_printf("[ACPI] RSDP revision 0x%x not in {0,2}\n", rsdp->revision);
        return false;
    }

    if (acpi_checksum(rsdp, 20) != 0) {
        debug_printf("[ACPI] RSDP V1 checksum failed\n");
        return false;
    }

    if (rsdp->revision >= 2) {
        /* Trust `length` only after bounding it. A malicious or broken
         * RSDP could declare a huge length; cap at 4 KB so a stray scan
         * cannot trip a page fault on unmapped MMIO. */
        uint32_t len = rsdp->length;
        if (len < sizeof(acpi_rsdp_t) || len > 4096) {
            debug_printf("[ACPI] RSDP V2 length 0x%x out of range\n", len);
            return false;
        }
        if (acpi_checksum(rsdp, len) != 0) {
            debug_printf("[ACPI] RSDP V2 extended checksum failed (len=%u)\n", len);
            return false;
        }
    }

    return true;
}

static acpi_rsdp_t* scan_memory_range(uintptr_t start, uintptr_t end) {
    if (end <= start) return NULL;
    size_t span = end - start;

    volatile void* virt_start = acpi_map_physical(start, span);
    if (!virt_start) {
        debug_printf("[ACPI] Failed to map memory range 0x%lx-0x%lx\n", start, end);
        return NULL;
    }

    for (uintptr_t addr = 0; addr <= span - sizeof(acpi_rsdp_t);
         addr += RSDP_SCAN_STEP_BYTES) {
        acpi_rsdp_t* candidate = (acpi_rsdp_t*)((uintptr_t)virt_start + addr);

        if (memcmp(candidate->signature, "RSD PTR ", 8) != 0)
            continue;

        if (validate_rsdp(candidate)) {
            debug_printf("[ACPI] Found valid RSDP at physical 0x%lx\n", start + addr);
            return candidate;
        }
    }

    return NULL;
}

/* ACPI 6.5 §5.2.5.2 — finding the RSDP under UEFI.
 *
 * The bootloader (TagBoot) walks the EFI Configuration Table looking for
 * the ACPI 2.0 GUID (preferred) or the ACPI 1.0 GUID, and forwards the
 * physical address in boot_info v2. Under UEFI low memory may be entirely
 * unmapped or contain garbage — do NOT fall back to EBDA/ROM scans.
 */
static acpi_rsdp_t* rsdp_from_uefi(void) {
    boot_info_t* bi = boot_info_get();
    if (!bi || !boot_info_valid(bi))
        return NULL;
    /* Accept any UEFI version (v2 or v3+). The RSDP forwarding contract
     * is identical from v2 onward — only the trailing fields grow. */
    if (!boot_info_is_uefi(bi))
        return NULL;
    /* total_size is the size of THIS boot_info_t copy at link time;
     * loader-side may be older and write a smaller record. Require at
     * least up through rsdp_addr. */
    if (bi->total_size < offsetof(boot_info_t, rsdp_addr) + sizeof(bi->rsdp_addr))
        return NULL;
    if (bi->boot_method != 1)
        return NULL;             /* not UEFI */
    if (bi->rsdp_addr == 0) {
        debug_printf("[ACPI] UEFI boot but firmware exposed no RSDP — refusing legacy scan\n");
        return NULL;
    }

    /* Map enough to inspect the V2 structure's `length` field, then
     * remap with the real size to validate the extended checksum. */
    acpi_rsdp_t* probe = (acpi_rsdp_t*)acpi_map_physical(bi->rsdp_addr,
                                                        sizeof(acpi_rsdp_t));
    if (!probe || memcmp(probe->signature, "RSD PTR ", 8) != 0) {
        debug_printf("[ACPI] UEFI RSDP at 0x%lx is invalid\n",
                     (unsigned long)bi->rsdp_addr);
        return NULL;
    }

    uint32_t full_len = (probe->revision >= 2 && probe->length >= sizeof(acpi_rsdp_t))
                            ? probe->length
                            : (uint32_t)sizeof(acpi_rsdp_t);
    acpi_rsdp_t* rsdp = (acpi_rsdp_t*)acpi_map_physical(bi->rsdp_addr, full_len);

    if (rsdp && validate_rsdp(rsdp)) {
        debug_printf("[ACPI] RSDP from UEFI boot_info: 0x%lx (rev=%u, len=%u)\n",
                     (unsigned long)bi->rsdp_addr, rsdp->revision,
                     (rsdp->revision >= 2) ? rsdp->length
                                           : (uint32_t)sizeof(acpi_rsdp_t));
        return rsdp;
    }
    return NULL;
}

/* Legacy BIOS path — only used when not booted via UEFI. */
static acpi_rsdp_t* rsdp_from_bios(void) {
    /* Step 1: try EBDA. The segment pointer is two bytes at physical
     * 0x40E. A zero segment means "no EBDA"; a segment that resolves
     * above 0xA0000 is nonsense (legacy upper memory). Real AMI/Award
     * BIOSes have shipped EBDA pointers anywhere from 0x40000 upward,
     * so accept any non-zero segment that lands strictly below 0xA0000.
     */
    uint16_t* ebda_ptr = (uint16_t*)acpi_map_physical(BDA_EBDA_SEG_PTR,
                                                      sizeof(uint16_t));
    if (ebda_ptr) {
        uint32_t ebda_base = ((uint32_t)*ebda_ptr) << 4;
        if (ebda_base != 0 && (ebda_base + EBDA_SCAN_BYTES) <= LOWMEM_END) {
            debug_printf("[ACPI] Scanning first %u bytes of EBDA at 0x%x\n",
                         EBDA_SCAN_BYTES, ebda_base);
            acpi_rsdp_t* rsdp = scan_memory_range(ebda_base,
                                                  ebda_base + EBDA_SCAN_BYTES);
            if (rsdp) return rsdp;
        } else if (ebda_base != 0) {
            debug_printf("[ACPI] EBDA segment 0x%x out of bounds, skipping\n",
                         ebda_base);
        }
    }

    /* Step 2: BIOS ROM region 0xE0000-0xFFFFF. */
    debug_printf("[ACPI] Scanning BIOS ROM area (0x%x-0x%x)\n",
                 BIOS_ROM_BEGIN, BIOS_ROM_END - 1);
    return scan_memory_range(BIOS_ROM_BEGIN, BIOS_ROM_END);
}

acpi_rsdp_t* acpi_find_rsdp(void) {
    /* UEFI: the firmware contract is the EFI Configuration Table; there is
     * no spec-compliant fallback. If TagBoot couldn't find the RSDP, neither
     * can we. */
    boot_info_t* bi = boot_info_get();
    if (bi && boot_info_valid(bi) && boot_info_is_uefi(bi)
        && bi->boot_method == 1) {
        return rsdp_from_uefi();
    }

    return rsdp_from_bios();
}

uint8_t acpi_checksum(void* data, size_t length) {
    uint8_t sum = 0;
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < length; i++)
        sum += bytes[i];
    return sum;
}

/* ACPI tables, once located, are referenced for the lifetime of the kernel
 * (shutdown path needs the FADT). vmm_map_mmio sets PCD=1+PWT=1 (PA3 = UC),
 * which is the spec-correct cacheability for memory-mapped firmware
 * structures. We intentionally never unmap. */
volatile void* acpi_map_physical(uintptr_t phys_addr, size_t size) {
    return vmm_map_mmio(phys_addr, size, VMM_FLAGS_KERNEL_RW);
}
