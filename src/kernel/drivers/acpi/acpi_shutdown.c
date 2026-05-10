#include "acpi_internal.h"
#include "io.h"
#include "klib.h"
#include "atomics.h"
#include "cpu_calibrate.h"
#include "vmm.h"
#include <stddef.h>

/* ACPI Generic Address Structure address_space_id values (ACPI 6.x §5.2.3.2) */
#define GAS_AS_SYSTEM_MEMORY  0  /* MMIO */
#define GAS_AS_SYSTEM_IO      1  /* I/O port */

/* True iff `len` bytes of FADT cover the X_/extended field that ends at
 * `field_end` (offsetof(field) + sizeof(field)). ACPI 1.0b FADT is only
 * 116 bytes — its `x_*` and `reset_reg` fields are not present, and
 * struct accesses past EOT yield garbage from neighbouring memory. */
static inline bool fadt_field_present(uint32_t len, size_t field_end) {
    return (size_t)len >= field_end;
}
#define GAS_AS_PCI_CONFIG     2  /* PCI config space */

static void delay_ms(uint32_t ms) {
    uint64_t deadline = rdtsc() + cpu_ms_to_tsc(ms);
    while (rdtsc() < deadline) {
        __asm__ volatile("pause");
    }
}

static void triple_fault(void) {
    debug_printf("[ACPI] Triggering triple fault...\n");

    __asm__ volatile (
        "cli\n"
        "lidt %0\n"
        "int $0x03\n"
        :
        : "m"((struct { uint16_t limit; uint64_t base; }){ 0, 0 })
    );

    while (1) hlt();
}

static void attempt_keyboard_reset(void) {
    debug_printf("[ACPI] Attempting keyboard controller reset...\n");

    uint8_t status;
    for (int i = 0; i < 10; i++) {
        status = inb(0x64);
        if ((status & 0x02) == 0) break;
        delay_ms(1);
    }

    outb(0x64, 0xFE);

    delay_ms(100);
}

/* gas_write_word — write a 16-bit value through a Generic Address Structure.
 *
 * GAS address_space dispatch matches the ACPI spec rather than assuming I/O.
 * On consumer/desktop x86 PM1 CNT lives at I/O 0x604-class ports; on some
 * server boards and embedded SoCs (ACPI 2.0+) it can be memory-mapped, where
 * `outw` would silently miss the register. UEFI machines occasionally route
 * the reset register via PCI config space. Handle all three.
 *
 * For MMIO we map the register UC via vmm_map_mmio so the access is
 * uncached + write-through (PCD=1, PWT=1) and visible to the chipset
 * regardless of MTRR state — same correctness path as the VGA framebuffer.
 */
static int gas_write_word(const acpi_gas_t *gas, uint16_t value) {
    if (!gas || gas->address == 0) return -1;

    switch (gas->address_space) {
        case GAS_AS_SYSTEM_IO:
            outw((uint16_t)gas->address, value);
            return 0;

        case GAS_AS_SYSTEM_MEMORY: {
            volatile void *vmap = vmm_map_mmio((uintptr_t)gas->address,
                                               sizeof(uint16_t),
                                               VMM_FLAGS_KERNEL_RW);
            if (!vmap) {
                debug_printf("[ACPI] GAS MMIO map failed for 0x%lx\n",
                             (unsigned long)gas->address);
                return -1;
            }
            *(volatile uint16_t *)vmap = value;
            /* Don't unmap — we're on a one-way path to power off / reset. */
            return 0;
        }

        case GAS_AS_PCI_CONFIG:
            /* PCI config space is bus:dev:func:offset packed into address.
             * BoxOS doesn't carry a generic PCI-config writer at this layer;
             * uncommon in practice. Caller falls through to the next method. */
            debug_printf("[ACPI] GAS PCI_CONFIG not implemented (addr=0x%lx)\n",
                         (unsigned long)gas->address);
            return -1;

        default:
            debug_printf("[ACPI] GAS unknown address_space=%u\n", gas->address_space);
            return -1;
    }
}

static int gas_write_byte(const acpi_gas_t *gas, uint8_t value) {
    if (!gas || gas->address == 0) return -1;

    switch (gas->address_space) {
        case GAS_AS_SYSTEM_IO:
            outb((uint16_t)gas->address, value);
            return 0;

        case GAS_AS_SYSTEM_MEMORY: {
            volatile void *vmap = vmm_map_mmio((uintptr_t)gas->address,
                                               sizeof(uint8_t),
                                               VMM_FLAGS_KERNEL_RW);
            if (!vmap) return -1;
            *(volatile uint8_t *)vmap = value;
            return 0;
        }

        case GAS_AS_PCI_CONFIG:
        default:
            return -1;
    }
}

/* Pick the live PM1 control GAS:
 *   1. ACPI 2.0+ X_ variant if the FADT is long enough to actually contain
 *      it AND it has a non-zero address — spec-preferred descriptor.
 *   2. Synthesise a legacy I/O GAS from the 32-bit FADT field otherwise.
 *
 * The length check matters on real hardware: every ACPI 1.0b firmware (and
 * QEMU's default i440fx FADT) ships rev=1, len=116, where `x_pm1a_*` lives
 * past EOT and `*x_gas` reads garbage. Trusting it sends PM1 writes to a
 * random MMIO address and shutdown silently fails.
 */
static acpi_gas_t resolve_pm1_gas(const acpi_gas_t *x_gas, uint32_t legacy_port,
                                  uint32_t fadt_len, size_t x_field_end) {
    acpi_gas_t out = {0};
    if (x_gas && fadt_field_present(fadt_len, x_field_end) &&
        x_gas->address != 0) {
        out = *x_gas;
        return out;
    }
    if (legacy_port != 0) {
        out.address_space = GAS_AS_SYSTEM_IO;
        out.bit_width     = 16;
        out.bit_offset    = 0;
        out.access_size   = 2;
        out.address       = legacy_port;
    }
    return out;
}

static void attempt_acpi_pm1(const acpi_gas_t *gas, uint16_t slp_typ, const char *tag) {
    if (gas->address == 0) return;

    uint16_t value = (uint16_t)((slp_typ << 10) | (1u << 13));   /* SLP_TYP | SLP_EN */

    debug_printf("[ACPI] %s shutdown via GAS as=%u addr=0x%lx SLP_TYP=0x%x val=0x%x\n",
                 tag, gas->address_space,
                 (unsigned long)gas->address, slp_typ, value);

    if (gas_write_word(gas, value) != 0) {
        debug_printf("[ACPI] %s write failed\n", tag);
        return;
    }
    delay_ms(100);
}

static void attempt_acpi_reset(void) {
    if (!g_acpi.initialized || !g_acpi.fadt) return;

    /* reset_reg + reset_value were added in ACPI 2.0 (FADT rev 3, length
     * grew to 244). On a rev 1 FADT (len=116) those bytes don't exist —
     * accessing them reads adjacent table data and would issue a wild
     * write to a random MMIO/IO address. Guard with a length check. */
    uint32_t flen = g_acpi.fadt->header.length;
    if (!fadt_field_present(flen, offsetof(acpi_fadt_t, reset_value) + 1)) {
        debug_printf("[ACPI] FADT too short for reset register (len=%u)\n", flen);
        return;
    }

    if (!(g_acpi.fadt->flags & (1 << 10))) {
        debug_printf("[ACPI] FADT does not support reset register\n");
        return;
    }

    const acpi_gas_t *reg = &g_acpi.fadt->reset_reg;
    uint8_t val           = g_acpi.fadt->reset_value;

    debug_printf("[ACPI] reset: addr_space=%u addr=0x%lx value=0x%x\n",
                 reg->address_space, (unsigned long)reg->address, val);

    if (gas_write_byte(reg, val) != 0) {
        debug_printf("[ACPI] reset write failed (unsupported address space)\n");
        return;
    }
    delay_ms(100);
}

void acpi_shutdown(void) {
    __asm__ volatile("cli");

    if (g_acpi.initialized) {
        debug_printf("[ACPI] poweroff via PM1 (PM1a=0x%x PM1b=0x%x SLP_TYPa=0x%x SLP_TYPb=0x%x rev=%u len=%u)\n",
                     g_acpi.pm1a_cnt_blk, g_acpi.pm1b_cnt_blk,
                     g_acpi.slp_typa, g_acpi.slp_typb,
                     g_acpi.fadt ? g_acpi.fadt->header.revision : 0,
                     g_acpi.fadt ? g_acpi.fadt->header.length   : 0);
    } else {
        debug_printf("[ACPI] not initialised; no soft poweroff available\n");
    }

    if (g_acpi.initialized && g_acpi.fadt) {
        uint32_t flen = g_acpi.fadt->header.length;
        acpi_gas_t pm1a = resolve_pm1_gas(&g_acpi.fadt->x_pm1a_control_block,
                                          g_acpi.pm1a_cnt_blk,
                                          flen,
                                          offsetof(acpi_fadt_t, x_pm1a_control_block) +
                                              sizeof(acpi_gas_t));
        acpi_gas_t pm1b = resolve_pm1_gas(&g_acpi.fadt->x_pm1b_control_block,
                                          g_acpi.pm1b_cnt_blk,
                                          flen,
                                          offsetof(acpi_fadt_t, x_pm1b_control_block) +
                                              sizeof(acpi_gas_t));
        if (pm1a.address) attempt_acpi_pm1(&pm1a, g_acpi.slp_typa, "PM1a");
        if (pm1b.address) attempt_acpi_pm1(&pm1b, g_acpi.slp_typb, "PM1b");
    }

    /* Soft poweroff exhausted. ACPI S5 is the only standardised way to
     * power down on x86 — there is no second method on real hardware.
     * NEVER reset/triple-fault here: those reboot the machine, which is
     * the wrong semantics for shutdown. Park the CPU and let the operator
     * pull power. */
    debug_printf("[ACPI] Soft poweroff failed; halting CPU. Power off manually.\n");
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void acpi_reboot(void) {
    __asm__ volatile("cli");

    debug_printf("[ACPI] Attempting ACPI reset register...\n");
    attempt_acpi_reset();

    attempt_keyboard_reset();

    triple_fault();

    debug_printf("[ACPI] All reboot methods failed, entering HLT loop\n");
    while (1) {
        hlt();
    }
}

void acpi_print_info(void) {
    if (!g_acpi.initialized) {
        debug_printf("[ACPI] ACPI not initialized\n");
        return;
    }

    if (g_acpi.rsdp) {
        debug_printf("RSDP:\n");
        debug_printf("  Signature: %.8s\n", g_acpi.rsdp->signature);
        debug_printf("  OEM ID: %.6s\n", g_acpi.rsdp->oem_id);
        debug_printf("  Revision: %u\n", g_acpi.rsdp->revision);
        debug_printf("  RSDT: 0x%x\n", g_acpi.rsdp->rsdt_address);
        if (g_acpi.rsdp->revision >= 2) {
            debug_printf("  XSDT: 0x%lx\n",
                         (unsigned long)g_acpi.rsdp->xsdt_address);
        }
    }

    if (g_acpi.fadt) {
        debug_printf("\nFADT:\n");
        debug_printf("  PM1a_CNT: 0x%x\n", g_acpi.pm1a_cnt_blk);
        debug_printf("  PM1b_CNT: 0x%x\n", g_acpi.pm1b_cnt_blk);
        debug_printf("  DSDT: 0x%x  X_DSDT: 0x%lx\n",
                     g_acpi.fadt->dsdt,
                     (unsigned long)g_acpi.fadt->x_dsdt);
    }

    debug_printf("\n_S5 Object:\n");
    debug_printf("  Found: %s\n",
                 g_acpi.s5_found ? "Yes" : "No (using fallback)");
    debug_printf("  SLP_TYPa: 0x%x\n", g_acpi.slp_typa);
    debug_printf("  SLP_TYPb: 0x%x\n", g_acpi.slp_typb);

    if (g_acpi.hpet.present) {
        debug_printf("\nHPET:\n");
        debug_printf("  Base: 0x%lx vendor=0x%04x\n",
                     (unsigned long)g_acpi.hpet.base,
                     g_acpi.hpet.vendor_id);
        debug_printf("  Counter: %s, %u comparators, min_tick=%u%s\n",
                     g_acpi.hpet.counter_size_64 ? "64-bit" : "32-bit",
                     g_acpi.hpet.comparator_count,
                     g_acpi.hpet.minimum_tick,
                     g_acpi.hpet.legacy_replacement ? " [LegacyReplace]" : "");
    } else {
        debug_printf("\nHPET: not present\n");
    }

    if (g_acpi.mcfg.present) {
        debug_printf("\nMCFG (PCIe ECAM): %u segment(s)\n", g_acpi.mcfg.count);
        for (uint8_t i = 0; i < g_acpi.mcfg.count; i++) {
            const acpi_mcfg_segment_t* s = &g_acpi.mcfg.segments[i];
            debug_printf("  [%u] group=%u base=0x%lx bus %u..%u\n",
                         i, s->segment_group,
                         (unsigned long)s->base_address,
                         s->start_bus, s->end_bus);
        }
    } else {
        debug_printf("\nMCFG: not present (PCI must use legacy 0xCF8/0xCFC)\n");
    }
}
