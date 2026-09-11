#include "irqchip.h"
#include "pic.h"
#include "lapic.h"
#include "ioapic.h"
#include "acpi_madt.h"
#include "cpuid.h"
#include "klib.h"
#include "io.h"

static irqchip_type_t active_type = IRQCHIP_PIC;
static irq_chip_t* active_chip = NULL;
static madt_info_t madt_info;


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


static void apic_chip_enable(uint8_t irq) {
    if (irq < IRQ_MAX_COUNT) {
        uint32_t gsi = ioapic_isa_to_gsi(irq);
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
    return lapic_read(LAPIC_REG_ISR_BASE + 0x10);
}

static uint32_t apic_chip_get_irr(void) {
    return lapic_read(LAPIC_REG_IRR_BASE + 0x10);
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


static bool cpu_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 9)) != 0;
}

static void pic_disable_for_apic(void) {
    pic_init();
    pic_set_mask(0xFF, 0xFF);

    debug_printf("[IRQCHIP] Legacy PIC masked for APIC mode\n");
}


void irqchip_init(void) {
    debug_printf("[IRQCHIP] Detecting interrupt controller...\n");

    if (!cpu_has_apic()) {
        debug_printf("[IRQCHIP] CPU does not support APIC, using legacy PIC\n");
        goto use_pic;
    }

    debug_printf("[IRQCHIP] CPU supports APIC\n");

    acpi_error_t err = acpi_parse_madt(&madt_info);
    if (err != ACPI_OK || !madt_info.valid) {
        debug_printf("[IRQCHIP] MADT not available, using legacy PIC\n");
        goto use_pic;
    }

    pic_disable_for_apic();

    lapic_init(madt_info.lapic_address);
    if (!lapic_is_enabled()) {
        debug_printf("[IRQCHIP] %[E]LAPIC init failed, falling back to PIC%[D]\n");
        goto use_pic;
    }

    if (madt_info.bsp_acpi_id_resolved) {
        lapic_apply_madt_nmi(&madt_info, madt_info.bsp_acpi_processor_id);
    } else {
        debug_printf("[IRQCHIP] BSP ACPI processor ID unresolved; "
                     "skipping LAPIC NMI programming\n");
    }

    ioapic_init(madt_info.ioapic_address, madt_info.ioapic_gsi_base);

    for (uint8_t i = 0; i < madt_info.nmi_source_count; i++) {
        const madt_nmi_source_info_t* s = &madt_info.nmi_sources[i];
        if (!s->valid) continue;
        ioapic_program_nmi_source(s->gsi, s->mps_flags,
                                   madt_info.bsp_lapic_id);
    }

    active_type = IRQCHIP_APIC;
    active_chip = &apic_chip;

    debug_printf("[IRQCHIP] %[S]Using APIC mode (LAPIC + IO-APIC, %u IRQs)%[D]\n",
                 active_chip->max_irqs);
    return;

use_pic:
    pic_init();

    active_type = IRQCHIP_PIC;
    active_chip = &pic_chip;

    debug_printf("[IRQCHIP] %[S]Using legacy PIC mode (16 IRQs)%[D]\n");
}

const struct madt_info *irqchip_get_madt(void) {
    if (active_type != IRQCHIP_APIC) return NULL;
    return &madt_info;
}

void irqchip_apply_lapic_nmi_self(void) {
    if (active_type != IRQCHIP_APIC) return;
    uint32_t my_apic_id = lapic_get_id();
    uint8_t  acpi_pid   = 0xFF;
    bool found = false;
    for (uint16_t i = 0; i < madt_info.cpu_map_count; i++) {
        if (madt_info.cpu_map[i].apic_id == my_apic_id) {
            acpi_pid = (uint8_t)(madt_info.cpu_map[i].acpi_processor_id & 0xFF);
            found = true;
            break;
        }
    }
    if (!found) {
        acpi_pid = MADT_NMI_PROCESSOR_ALL;
    }
    lapic_apply_madt_nmi(&madt_info, acpi_pid);
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