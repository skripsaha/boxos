#include "acpi_internal.h"
#include "io.h"
#include "vmm.h"
#include "klib.h"

static bool validate_rsdp(acpi_rsdp_t* rsdp) {
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) {
        return false;
    }

    uint8_t sum = acpi_checksum(rsdp, 20);
    if (sum != 0) {
        debug_printf("[ACPI] RSDP checksum failed\n");
        return false;
    }

    if (rsdp->revision >= 2) {
        sum = acpi_checksum(rsdp, sizeof(acpi_rsdp_t));
        if (sum != 0) {
            debug_printf("[ACPI] RSDP extended checksum failed\n");
            return false;
        }
    }

    return true;
}

static acpi_rsdp_t* scan_memory_range(uintptr_t start, uintptr_t end) {
    volatile void* virt_start = acpi_map_physical(start, end - start);
    if (!virt_start) {
        debug_printf("[ACPI] Failed to map memory range 0x%lx-0x%lx\n", start, end);
        return NULL;
    }

    for (uintptr_t addr = 0; addr < (end - start); addr += 16) {
        acpi_rsdp_t* candidate = (acpi_rsdp_t*)((uintptr_t)virt_start + addr);

        if (memcmp(candidate->signature, "RSD PTR ", 8) == 0) {
            if (validate_rsdp(candidate)) {
                debug_printf("[ACPI] Found valid RSDP at physical 0x%lx\n", start + addr);
                return candidate;
            }
        }
    }

    return NULL;
}

acpi_rsdp_t* acpi_find_rsdp(void) {
    acpi_rsdp_t* rsdp = NULL;

    uint16_t* ebda_ptr = (uint16_t*)acpi_map_physical(0x40E, 2);
    if (ebda_ptr) {
        uint32_t ebda_base = (*ebda_ptr) << 4;
        if (ebda_base >= 0x80000 && ebda_base < 0xA0000) {
            debug_printf("[ACPI] Scanning EBDA at 0x%x\n", ebda_base);
            rsdp = scan_memory_range(ebda_base, ebda_base + 1024);
            if (rsdp) return rsdp;
        }
    }

    debug_printf("[ACPI] Scanning BIOS ROM area (0xE0000-0xFFFFF)\n");
    rsdp = scan_memory_range(0xE0000, 0x100000);
    if (rsdp) return rsdp;

    debug_printf("[ACPI] RSDP not found\n");
    return NULL;
}

uint8_t acpi_checksum(void* data, size_t length) {
    uint8_t sum = 0;
    uint8_t* bytes = (uint8_t*)data;

    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }

    return sum;
}

// ACPI mappings are intentionally never unmapped - RSDP, RSDT, FADT, DSDT
// are persistent kernel data needed for shutdown.
// TODO(v1.1): Fix VMM to allow mapping legacy low memory regions (< 1MB)
// vmm_map_mmio() rejects EBDA (0x40E) and BIOS ROM (0xE0000-0xFFFFF)
// due to overlap check; fallback works in QEMU.
volatile void* acpi_map_physical(uintptr_t phys_addr, size_t size) {
    return vmm_map_mmio(phys_addr, size, VMM_FLAGS_KERNEL_RW);
}
