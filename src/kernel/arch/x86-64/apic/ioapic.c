#include "ioapic.h"
#include "irqchip.h"
#include "io.h"
#include "klib.h"
#include "vmm.h"

static volatile uint32_t* ioapic_base_virt = NULL;
static uintptr_t ioapic_base_phys = 0;
static uint8_t ioapic_gsi_base = 0;
static uint8_t ioapic_max_entry = 0;

// Interrupt Source Override table (ISA IRQ -> GSI remapping)
#define ISO_TABLE_SIZE 16
static ioapic_iso_t iso_table[ISO_TABLE_SIZE];

uint32_t ioapic_read(uint32_t reg) {
    ioapic_base_virt[IOAPIC_IOREGSEL / 4] = reg;
    return ioapic_base_virt[IOAPIC_IOWIN / 4];
}

void ioapic_write(uint32_t reg, uint32_t value) {
    ioapic_base_virt[IOAPIC_IOREGSEL / 4] = reg;
    ioapic_base_virt[IOAPIC_IOWIN / 4] = value;
}

static void ioapic_write_redir(uint8_t entry, uint32_t low, uint32_t high) {
    uint32_t reg = IOAPIC_REG_REDTBL + entry * 2;
    ioapic_write(reg, low);
    ioapic_write(reg + 1, high);
}

static uint32_t ioapic_read_redir_low(uint8_t entry) {
    return ioapic_read(IOAPIC_REG_REDTBL + entry * 2);
}

void ioapic_init(uintptr_t base_addr, uint8_t gsi_base) {
    ioapic_base_phys = base_addr;
    ioapic_gsi_base = gsi_base;

    debug_printf("[IOAPIC] Initializing IO-APIC at phys 0x%lx, GSI base=%u\n",
                 base_addr, gsi_base);

    // Map IO-APIC MMIO (one page, uncacheable)
    ioapic_base_virt = (volatile uint32_t*)vmm_map_mmio(
        base_addr, 4096,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_CACHE_DISABLE
    );

    if (!ioapic_base_virt) {
        debug_printf("[IOAPIC] %[E]Failed to map IO-APIC MMIO%[D]\n");
        return;
    }

    // NOTE: Do NOT clear iso_table here — it was already populated by
    // acpi_parse_madt() -> ioapic_register_iso() BEFORE ioapic_init() runs.

    // Read version register to get max entries
    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    ioapic_max_entry = ((ver >> 16) & 0xFF);

    uint32_t id = (ioapic_read(IOAPIC_REG_ID) >> 24) & 0xFF;

    debug_printf("[IOAPIC] ID=%u, Version=0x%x, MaxEntry=%u (%u pins)\n",
                 id, ver & 0xFF, ioapic_max_entry, ioapic_max_entry + 1);

    // Mask all entries by default
    for (uint8_t i = 0; i <= ioapic_max_entry && i < IOAPIC_MAX_PINS; i++) {
        ioapic_write_redir(i, IOAPIC_REDIR_MASKED, 0);
    }

    debug_printf("[IOAPIC] All %u pins masked\n", ioapic_max_entry + 1);
    debug_printf("[IOAPIC] %[S]IO-APIC initialized successfully%[D]\n");
}

void ioapic_enable_irq(uint8_t gsi, uint8_t vector, uint8_t dest_lapic_id) {
    uint8_t pin = gsi - ioapic_gsi_base;

    if (pin > ioapic_max_entry) {
        debug_printf("[IOAPIC] %[E]GSI %u out of range (pin=%u, max=%u)%[D]\n",
                     gsi, pin, ioapic_max_entry);
        return;
    }

    uint32_t redir_flags = IOAPIC_REDIR_DELMOD_FIXED | IOAPIC_REDIR_DESTMOD_PHYS;

    uint16_t flags = 0;
    if (ioapic_gsi_flags(gsi, &flags)) {
        // Polarity: bits 0-1 (00=bus default, 01=active high, 11=active low)
        if ((flags & 0x03) == 0x03) {
            redir_flags |= IOAPIC_REDIR_POLARITY_LOW;
        }
        // Trigger: bits 2-3 (00=bus default, 01=edge, 11=level)
        if ((flags & 0x0C) == 0x0C) {
            redir_flags |= IOAPIC_REDIR_TRIGGER_LEVEL;
        }
    }

    uint32_t low = (vector & IOAPIC_REDIR_VECTOR_MASK) | redir_flags;
    uint32_t high = ((uint32_t)dest_lapic_id) << 24;

    ioapic_write_redir(pin, low, high);

    debug_printf("[IOAPIC] GSI %u -> vector %u, dest LAPIC %u (pin %u)\n",
                 gsi, vector, dest_lapic_id, pin);
}

void ioapic_disable_irq(uint8_t gsi) {
    uint8_t pin = gsi - ioapic_gsi_base;

    if (pin > ioapic_max_entry) {
        return;
    }

    uint32_t low = ioapic_read_redir_low(pin);
    low |= IOAPIC_REDIR_MASKED;
    ioapic_write(IOAPIC_REG_REDTBL + pin * 2, low);
}

void ioapic_set_irq(uint8_t gsi, uint8_t vector, uint8_t dest, uint32_t flags) {
    uint8_t pin = gsi - ioapic_gsi_base;

    if (pin > ioapic_max_entry) {
        return;
    }

    uint32_t low = (vector & IOAPIC_REDIR_VECTOR_MASK) | flags;
    uint32_t high = ((uint32_t)dest) << 24;

    ioapic_write_redir(pin, low, high);
}

uint8_t ioapic_get_max_entries(void) {
    return ioapic_max_entry + 1;
}

uintptr_t ioapic_get_base(void) {
    return ioapic_base_phys;
}

/*
 * A line the firmware did not describe, described by whoever does know.
 *
 * The override table above answers for ISA interrupts and is indexed by ISA
 * IRQ, because there are sixteen of those. Not every line that needs
 * describing is one of them: the ACPI SCI is level-triggered and active low by
 * specification rather than by bus default, and its GSI is whatever the FADT
 * says — on some boards well past fifteen. Firmware usually supplies an
 * override for it and usually does not have to.
 *
 * Firmware wins where it spoke: it knows its own board, and this is only for
 * the lines it left unsaid.
 */
#define GSI_DESCRIBED_MAX 8

typedef struct {
    uint32_t gsi;
    uint16_t flags;
    bool     active;
} ioapic_gsi_desc_t;

static ioapic_gsi_desc_t gsi_desc_table[GSI_DESCRIBED_MAX];

bool ioapic_gsi_flags(uint32_t gsi, uint16_t *out_flags) {
    for (int i = 0; i < ISO_TABLE_SIZE; i++) {
        if (iso_table[i].active && iso_table[i].gsi == gsi) {
            if (out_flags) *out_flags = iso_table[i].flags;
            return true;
        }
    }
    for (int i = 0; i < GSI_DESCRIBED_MAX; i++) {
        if (gsi_desc_table[i].active && gsi_desc_table[i].gsi == gsi) {
            if (out_flags) *out_flags = gsi_desc_table[i].flags;
            return true;
        }
    }
    return false;
}

void ioapic_describe_gsi(uint32_t gsi, uint16_t flags) {
    for (int i = 0; i < GSI_DESCRIBED_MAX; i++) {
        if (gsi_desc_table[i].active && gsi_desc_table[i].gsi == gsi) {
            gsi_desc_table[i].flags = flags;
            return;
        }
    }
    for (int i = 0; i < GSI_DESCRIBED_MAX; i++) {
        if (!gsi_desc_table[i].active) {
            gsi_desc_table[i].gsi    = gsi;
            gsi_desc_table[i].flags  = flags;
            gsi_desc_table[i].active = true;
            return;
        }
    }
    debug_printf("[IOAPIC] %[E]no room left to describe GSI %u%[D]\n", gsi);
}

void ioapic_register_iso(uint8_t isa_irq, uint32_t gsi, uint16_t flags) {
    if (isa_irq >= ISO_TABLE_SIZE) {
        debug_printf("[IOAPIC] %[E]ISO: ISA IRQ %u out of range%[D]\n", isa_irq);
        return;
    }

    iso_table[isa_irq].isa_irq = isa_irq;
    iso_table[isa_irq].gsi = gsi;
    iso_table[isa_irq].flags = flags;
    iso_table[isa_irq].active = true;

    debug_printf("[IOAPIC] ISO: ISA IRQ %u -> GSI %u (flags=0x%04x)\n",
                 isa_irq, gsi, flags);
}

uint32_t ioapic_isa_to_gsi(uint8_t isa_irq) {
    if (isa_irq < ISO_TABLE_SIZE && iso_table[isa_irq].active) {
        return iso_table[isa_irq].gsi;
    }
    // Default identity mapping: ISA IRQ N = GSI N
    return isa_irq;
}

uint16_t ioapic_get_iso_flags(uint8_t isa_irq) {
    if (isa_irq < ISO_TABLE_SIZE && iso_table[isa_irq].active) {
        return iso_table[isa_irq].flags;
    }
    return 0;
}

void ioapic_program_nmi_source(uint32_t gsi, uint16_t mps_flags,
                                uint8_t dest_lapic_id) {
    if (!ioapic_base_virt) {
        debug_printf("[IOAPIC] NMI source request but IOAPIC not mapped\n");
        return;
    }

    if (gsi < ioapic_gsi_base) {
        debug_printf("[IOAPIC] NMI Source GSI %u below this IOAPIC base %u\n",
                     gsi, ioapic_gsi_base);
        return;
    }
    uint32_t pin32 = gsi - ioapic_gsi_base;
    if (pin32 > ioapic_max_entry) {
        debug_printf("[IOAPIC] NMI Source GSI %u beyond max pin %u\n",
                     gsi, ioapic_max_entry);
        return;
    }
    uint8_t pin = (uint8_t)pin32;

    /* Translate MPS INTI flags (ACPI 6.5 §5.2.12.5) into redirection bits.
     * Bus default for the LPC/ISA bus is edge-triggered, active high; on
     * the system bus the default is level-triggered, active low. NMI
     * sources are almost always edge active high — but honour whatever
     * the firmware explicitly states. */
    uint32_t redir = IOAPIC_REDIR_DELMOD_NMI | IOAPIC_REDIR_DESTMOD_PHYS;
    uint16_t polarity = mps_flags & 0x3;
    uint16_t trigger  = (mps_flags >> 2) & 0x3;
    if (polarity == 0x3) redir |= IOAPIC_REDIR_POLARITY_LOW;
    if (trigger  == 0x3) redir |= IOAPIC_REDIR_TRIGGER_LEVEL;

    /* Vector is don't-care for NMI delivery, but Intel SDM advises leaving
     * a sentinel for analysis; 0x00 is fine. */
    uint32_t high = ((uint32_t)dest_lapic_id) << 24;
    ioapic_write_redir(pin, redir, high);

    debug_printf("[IOAPIC] NMI Source: GSI %u pin %u mps=0x%04x dest=%u low=0x%08x\n",
                 gsi, pin, mps_flags, dest_lapic_id, redir);
}
