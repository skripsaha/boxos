#include "acpi_internal.h"
#include "io.h"
#include "vmm.h"
#include "klib.h"
#include "boot_info.h"

#define RSDP_SCAN_STEP_BYTES   16u
#define EBDA_SCAN_BYTES        1024u
#define BIOS_ROM_BEGIN         0xE0000u
#define BIOS_ROM_END           0x100000u
#define BDA_EBDA_SEG_PTR       0x40Eu
#define LOWMEM_END             0xA0000u

static bool rsdp_revision_known(uint8_t rev) {
    return rev == 0 || rev == 2;
}

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

static acpi_rsdp_t* rsdp_from_uefi(void) {
    boot_info_t* bi = boot_info_get();
    if (!bi || !boot_info_valid(bi))
        return NULL;
    if (!boot_info_is_uefi(bi))
        return NULL;
    if (bi->total_size < offsetof(boot_info_t, rsdp_addr) + sizeof(bi->rsdp_addr))
        return NULL;
    if (bi->boot_method != 1)
        return NULL;
    if (bi->rsdp_addr == 0) {
        debug_printf("[ACPI] UEFI boot but firmware exposed no RSDP — refusing legacy scan\n");
        return NULL;
    }

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

static acpi_rsdp_t* rsdp_from_bios(void) {
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

    debug_printf("[ACPI] Scanning BIOS ROM area (0x%x-0x%x)\n",
                 BIOS_ROM_BEGIN, BIOS_ROM_END - 1);
    return scan_memory_range(BIOS_ROM_BEGIN, BIOS_ROM_END);
}

acpi_rsdp_t* acpi_find_rsdp(void) {
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

volatile void* acpi_map_physical(uintptr_t phys_addr, size_t size) {
    return vmm_map_mmio(phys_addr, size, VMM_FLAGS_KERNEL_RW);
}