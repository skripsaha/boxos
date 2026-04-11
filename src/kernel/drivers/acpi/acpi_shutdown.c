#include "acpi_internal.h"
#include "io.h"
#include "klib.h"
#include "atomics.h"
#include "cpu_calibrate.h"

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

static void attempt_acpi_pm1(uint32_t pm_ctrl, uint16_t slp_typ) {
    if (pm_ctrl == 0) return;

    debug_printf("[ACPI] Attempting PM1 shutdown (port 0x%x, SLP_TYP=0x%x)...\n",
                 pm_ctrl, slp_typ);

    uint16_t value = (slp_typ << 10) | (1 << 13);

    outw(pm_ctrl, value);

    delay_ms(100);
}

static void attempt_acpi_reset(void) {
    if (!g_acpi.initialized || !g_acpi.fadt) return;

    if (!(g_acpi.fadt->flags & (1 << 10))) {
        debug_printf("[ACPI] FADT does not support reset register\n");
        return;
    }

    acpi_gas_t* reg = &g_acpi.fadt->reset_reg;
    uint8_t val = g_acpi.fadt->reset_value;

    debug_printf("[ACPI] ACPI reset: addr_space=%u addr=0x%lx value=0x%x\n",
                 reg->address_space, (unsigned long)reg->address, val);

    switch (reg->address_space) {
        case 0:
            *(volatile uint8_t*)(uintptr_t)reg->address = val;
            break;
        case 1:
            outb((uint16_t)reg->address, val);
            break;
        case 2:
            break;
    }

    delay_ms(100);
}

void acpi_shutdown(void) {
    __asm__ volatile("cli");

    if (g_acpi.initialized) {
        debug_printf("[ACPI] Using ACPI shutdown (PM1a=0x%x, PM1b=0x%x)\n",
                     g_acpi.pm1a_cnt_blk, g_acpi.pm1b_cnt_blk);
        debug_printf("[ACPI] SLP_TYPa=0x%x, SLP_TYPb=0x%x\n",
                     g_acpi.slp_typa, g_acpi.slp_typb);
    } else {
        debug_printf("[ACPI] ACPI not initialized, using fallback methods\n");
    }

    if (g_acpi.initialized && g_acpi.pm1a_cnt_blk != 0) {
        attempt_acpi_pm1(g_acpi.pm1a_cnt_blk, g_acpi.slp_typa);
    }

    if (g_acpi.initialized && g_acpi.pm1b_cnt_blk != 0) {
        attempt_acpi_pm1(g_acpi.pm1b_cnt_blk, g_acpi.slp_typb);
    }

    debug_printf("[ACPI] PM1 shutdown failed, trying keyboard reset...\n");
    attempt_keyboard_reset();

    triple_fault();

    debug_printf("[ACPI] All shutdown methods failed, entering HLT loop\n");
    while (1) {
        hlt();
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
            debug_printf("  XSDT: 0x%lx\n", g_acpi.rsdp->xsdt_address);
        }
    }

    if (g_acpi.fadt) {
        debug_printf("\nFADT:\n");
        debug_printf("  PM1a_CNT: 0x%x\n", g_acpi.pm1a_cnt_blk);
        debug_printf("  PM1b_CNT: 0x%x\n", g_acpi.pm1b_cnt_blk);
        debug_printf("  DSDT: 0x%x\n", g_acpi.fadt->dsdt);
    }

    debug_printf("\n_S5 Object:\n");
    debug_printf("  Found: %s\n", g_acpi.s5_found ? "Yes" : "No (using fallback)");
    debug_printf("  SLP_TYPa: 0x%x\n", g_acpi.slp_typa);
    debug_printf("  SLP_TYPb: 0x%x\n", g_acpi.slp_typb);
}
