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

    // Check for ISA override flags
    uint32_t redir_flags = IOAPIC_REDIR_DELMOD_FIXED | IOAPIC_REDIR_DESTMOD_PHYS;

    // Apply Interrupt Source Override flags if present
    if (gsi < ISO_TABLE_SIZE && iso_table[gsi].active) {
        // Check for the original ISA IRQ that maps to this GSI
        for (int i = 0; i < ISO_TABLE_SIZE; i++) {
            if (iso_table[i].active && iso_table[i].gsi == gsi) {
                uint16_t flags = iso_table[i].flags;
                // Polarity: bits 0-1 (00=bus default, 01=active high, 11=active low)
                if ((flags & 0x03) == 0x03) {
                    redir_flags |= IOAPIC_REDIR_POLARITY_LOW;
                }
                // Trigger: bits 2-3 (00=bus default, 01=edge, 11=level)
                if ((flags & 0x0C) == 0x0C) {
                    redir_flags |= IOAPIC_REDIR_TRIGGER_LEVEL;
                }
                break;
            }
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
