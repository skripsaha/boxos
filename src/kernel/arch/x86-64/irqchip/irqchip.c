#include "irqchip.h"
#include "pic.h"
#include "lapic.h"
#include "ioapic.h"
#include "acpi_madt.h"
#include "cpuid.h"
#include "klib.h"
#include "io.h"

// Current interrupt controller state
static irqchip_type_t active_type = IRQCHIP_PIC;
static irq_chip_t* active_chip = NULL;
static madt_info_t madt_info;

// --- PIC backend (wraps existing pic.c functions) ---

static void pic_chip_enable(uint8_t gsi) {
    if (gsi < 16) pic_enable_irq(gsi);
}

static void pic_chip_disable(uint8_t gsi) {
    if (gsi < 16) pic_disable_irq(gsi);
}

static void pic_chip_eoi(uint8_t gsi) {
    if (gsi < 16) pic_send_eoi(gsi);
}

static uint32_t pic_chip_get_isr(void) {
    return (uint32_t)pic_get_isr();
}

static uint32_t pic_chip_get_irr(void) {
    return (uint32_t)pic_get_irr();
}

static irq_chip_t pic_chip = {
    .name       = "8259A PIC",
    .enable_irq = pic_chip_enable,
    .disable_irq = pic_chip_disable,
    .send_eoi   = pic_chip_eoi,
    .get_isr    = pic_chip_get_isr,
    .get_irr    = pic_chip_get_irr,
    .max_irqs   = 16,
};

// --- APIC backend (LAPIC + IO-APIC) ---

static void apic_chip_enable(uint8_t irq) {
    if (irq < IRQ_MAX_COUNT) {
        // Apply ISA-to-GSI mapping (e.g., MADT says IRQ 0 -> GSI 2 for PIT)
        uint32_t gsi = ioapic_isa_to_gsi(irq);
        // Vector is based on IRQ number, not GSI — so IRQ handler maps back correctly
        uint8_t vector = IRQ_VECTOR_BASE + irq;
        ioapic_enable_irq((uint8_t)gsi, vector, madt_info.bsp_lapic_id);
    }
}

static void apic_chip_disable(uint8_t irq) {
    if (irq < IRQ_MAX_COUNT) {
        uint32_t gsi = ioapic_isa_to_gsi(irq);
        ioapic_disable_irq((uint8_t)gsi);
    }
}

static void apic_chip_eoi(uint8_t gsi) {
    (void)gsi;
    lapic_send_eoi();
}

static uint32_t apic_chip_get_isr(void) {
    // Read first 32 bits of LAPIC ISR (covers vectors 0-31... but we want 32-63)
    // ISR register 1 covers vectors 32-63
    return lapic_read(LAPIC_REG_ISR_BASE + 0x10);  // ISR[1] = vectors 32-63
}

static uint32_t apic_chip_get_irr(void) {
    return lapic_read(LAPIC_REG_IRR_BASE + 0x10);  // IRR[1] = vectors 32-63
}

static irq_chip_t apic_chip = {
    .name       = "APIC (LAPIC + IO-APIC)",
    .enable_irq = apic_chip_enable,
    .disable_irq = apic_chip_disable,
    .send_eoi   = apic_chip_eoi,
    .get_isr    = apic_chip_get_isr,
    .get_irr    = apic_chip_get_irr,
    .max_irqs   = IRQ_MAX_COUNT,
};

// --- Detection ---

static bool cpu_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 9)) != 0;  // CPUID.1:EDX[9] = APIC
}

static void pic_disable_for_apic(void) {
    // Remap PIC to vectors 0x20-0x2F (same as before) then mask all
    // This prevents PIC from firing spurious interrupts on APIC vectors
    pic_init();
    pic_set_mask(0xFF, 0xFF);

    debug_printf("[IRQCHIP] Legacy PIC masked for APIC mode\n");
}

// --- Public API ---

void irqchip_init(void) {
    debug_printf("[IRQCHIP] Detecting interrupt controller...\n");

    // Step 1: Check CPU APIC support
    if (!cpu_has_apic()) {
        debug_printf("[IRQCHIP] CPU does not support APIC, using legacy PIC\n");
        goto use_pic;
    }

    debug_printf("[IRQCHIP] CPU supports APIC\n");

    // Step 2: Parse MADT to find APIC addresses
    acpi_error_t err = acpi_parse_madt(&madt_info);
    if (err != ACPI_OK || !madt_info.valid) {
        debug_printf("[IRQCHIP] MADT not available, using legacy PIC\n");
        goto use_pic;
    }

    // Step 3: Disable legacy PIC (mask all, but keep initialized for spurious handling)
    pic_disable_for_apic();

    // Step 4: Initialize Local APIC
    lapic_init(madt_info.lapic_address);
    if (!lapic_is_enabled()) {
        debug_printf("[IRQCHIP] %[E]LAPIC init failed, falling back to PIC%[D]\n");
        goto use_pic;
    }

    // Step 5: Initialize IO-APIC
    ioapic_init(madt_info.ioapic_address, madt_info.ioapic_gsi_base);

    // Step 6: Activate APIC backend
    active_type = IRQCHIP_APIC;
    active_chip = &apic_chip;

    debug_printf("[IRQCHIP] %[S]Using APIC mode (LAPIC + IO-APIC, %u IRQs)%[D]\n",
                 active_chip->max_irqs);
    return;

use_pic:
    // Initialize PIC (already done in kernel_main, but ensure state is clean)
    pic_init();

    active_type = IRQCHIP_PIC;
    active_chip = &pic_chip;

    debug_printf("[IRQCHIP] %[S]Using legacy PIC mode (16 IRQs)%[D]\n");
}

void irqchip_enable_irq(uint8_t gsi) {
    if (active_chip) {
        active_chip->enable_irq(gsi);
    }
}

void irqchip_disable_irq(uint8_t gsi) {
    if (active_chip) {
        active_chip->disable_irq(gsi);
    }
}

void irqchip_send_eoi(uint8_t gsi) {
    if (active_chip) {
        active_chip->send_eoi(gsi);
    }
}

uint32_t irqchip_get_isr(void) {
    return active_chip ? active_chip->get_isr() : 0;
}

uint32_t irqchip_get_irr(void) {
    return active_chip ? active_chip->get_irr() : 0;
}

irqchip_type_t irqchip_get_type(void) {
    return active_type;
}

const char* irqchip_get_name(void) {
    return active_chip ? active_chip->name : "none";
}

uint8_t irqchip_max_irqs(void) {
    return active_chip ? active_chip->max_irqs : 0;
}
