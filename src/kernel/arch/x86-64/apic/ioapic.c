#include "ioapic.h"
#include "irqchip.h"
#include "io.h"
#include "klib.h"
#include "vmm.h"

static volatile uint32_t* ioapic_base_virt = NULL;
static uint8_t ioapic_gsi_base = 0;
static uint8_t ioapic_max_entry = 0;

// Interrupt Source Override table (ISA IRQ -> GSI remapping)
#define ISO_TABLE_SIZE 16
static ioapic_iso_t iso_table[ISO_TABLE_SIZE];

/*
 * One selector, one transaction.
 *
 * The IOAPIC has no addressable registers of its own on the bus. Intel
 * 82093AA §3.0: "To reference an IOAPIC register, a byte memory write that
 * the PIIX3 decodes for the IOAPIC loads the IOREGSEL Register with an
 * 8-bit value that specifies the IOAPIC register ... to be accessed. The
 * IOWIN Register is then used to read/write the desired data from/to the
 * IOAPIC register specified by bits [7:0] of the IOREGSEL Register." The
 * index write and the data access are therefore ONE transaction against a
 * single shared piece of state, and a second core that lands between them
 * does not corrupt its own access — it silently redirects ours.
 *
 * The same section makes the read-modify-write explicit: "to modify a
 * field (e.g., bit, byte), the whole 32 bit register must be read, the
 * field modified, and the 32 bits written back." That is three accesses
 * across two transactions, and masking a pin is exactly that shape.
 *
 * So the lock is held across the WHOLE operation, not around each access:
 * both halves of a redirection entry, and the read and the write-back of a
 * mask. This is the scope of Linux's ioapic_lock (arch/x86/kernel/apic/
 * io_apic.c), which its ioapic_read_entry / ioapic_write_entry take with
 * raw_spinlock_irqsave while the low-level io_apic_read / io_apic_write
 * stay bare. spin_lock() here disables interrupts for the same reason
 * Linux's irqsave form does: an interrupt handler that touches the IOAPIC
 * between our index and our data is the single-core version of the same
 * interleaving.
 */
static spinlock_t ioapic_reg_lock;

static inline uint32_t ioapic_read_held(uint32_t reg) {
    ioapic_base_virt[IOAPIC_IOREGSEL / 4] = reg;
    return ioapic_base_virt[IOAPIC_IOWIN / 4];
}

static inline void ioapic_write_held(uint32_t reg, uint32_t value) {
    ioapic_base_virt[IOAPIC_IOREGSEL / 4] = reg;
    ioapic_base_virt[IOAPIC_IOWIN / 4] = value;
}

static uint32_t ioapic_read(uint32_t reg) {
    spin_lock(&ioapic_reg_lock);
    uint32_t value = ioapic_read_held(reg);
    spin_unlock(&ioapic_reg_lock);
    return value;
}

/*
 * The high dword goes first, and that order is the whole point.
 *
 * A redirection entry is 64 bits reached as two 32-bit registers. The mask
 * bit lives in the LOW dword (82093AA §3.2.4, IOREDTBLx bit 16: "When this
 * bit is 1, the interrupt signal is masked ... When this bit is 0, the
 * interrupt is not masked"); the destination lives in the HIGH dword (bits
 * 63:56, "If the Destination Mode of this entry is Physical Mode ... bits
 * [59:56] contain an APIC ID"). Writing the low dword first therefore arms
 * the line — new vector, mask cleared — while the destination is still
 * whatever the previous programming left there, and an assertion inside
 * that window is delivered to the wrong LAPIC under the new vector.
 *
 * Linux states it in the same words above __ioapic_write_entry: "When we
 * write a new IO APIC routing entry, we need to write the high word first!
 * If the mask bit in the low word is clear, we will enable the interrupt,
 * and we need to make sure the entry is fully populated before that
 * happens."
 */
static void ioapic_write_redir(uint8_t entry, uint32_t low, uint32_t high) {
    uint32_t reg = IOAPIC_REG_REDTBL + entry * 2;
    spin_lock(&ioapic_reg_lock);
    ioapic_write_held(reg + 1, high);
    ioapic_write_held(reg, low);
    spin_unlock(&ioapic_reg_lock);
}

/*
 * Taking a pin out of service is the one case that goes the other way.
 *
 * High-dword-first is right while ARMING a line and wrong while disarming
 * one. The mask bit is in the low dword, so raising it first means the rest
 * of the entry only ever changes under a pin that can no longer deliver.
 * Skip that and the same window opens in reverse: ioapic_init() meets an
 * entry in whatever state firmware left it — on a warm handoff that can be
 * unmasked, with a live vector and a destination of its own — and writing
 * the high dword first drops destination 0 into an entry that is still
 * armed with the OLD vector. An assertion inside that window is formatted
 * from the old low dword and sent to APIC ID 0, which at this point in boot
 * is either the wrong core or no core at all.
 *
 * Linux splits the two for this reason and says so above ioapic_mask_entry:
 * "When we mask an IO APIC routing entry, we need to write the low word
 * first, in order to set the mask bit before we change the high bits!"
 * (arch/x86/kernel/apic/io_apic.c) — the mirror of the sentence quoted
 * above ioapic_write_redir.
 */
static void ioapic_mask_redir(uint8_t entry) {
    uint32_t reg = IOAPIC_REG_REDTBL + entry * 2;
    spin_lock(&ioapic_reg_lock);
    ioapic_write_held(reg, IOAPIC_REDIR_MASKED);
    ioapic_write_held(reg + 1, 0);
    spin_unlock(&ioapic_reg_lock);
}

void ioapic_init(uintptr_t base_addr, uint8_t gsi_base) {
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
        ioapic_mask_redir(i);
    }

    debug_printf("[IOAPIC] All %u pins masked\n", ioapic_max_entry + 1);
    debug_printf("[IOAPIC] %[S]IO-APIC initialized successfully%[D]\n");
}

/*
 * Can this APIC ID be named in a redirection entry at all?
 *
 * Intel 82093AA §3.2.4, IOREDTBLx Destination Field: in Physical Mode the
 * entry's destination is an APIC ID held in the high dword — eight bits on
 * every part BoxOS runs on. An APIC ID above 255 has no representation
 * there, and truncating it to its low eight bits does not merely fail to
 * route the line: it routes the line to a DIFFERENT, running processor,
 * which then takes an interrupt under a vector whose handler expects
 * different hardware. A machine whose BSP is x2APIC ID 0x100 would send
 * every legacy IRQ to the core with ID 0.
 *
 * The refusal lives here, at the field, and not in the caller. A
 * caller-side test is a test this file cannot see, and an eight-bit
 * parameter would put the narrowing at the call where nothing here could
 * ever notice it — the same shape that let MSI arm a truncated
 * destination for as long as it did.
 *
 * So the pin is left masked and the refusal is named. Naming it per line
 * rather than once is deliberate: each refused line is one device that
 * will not raise an interrupt, and the operator needs to know WHICH.
 *
 * What would lift the refusal is interrupt remapping (VT-d IR / AMD-Vi),
 * whose IRTE carries a 32-bit destination and turns the redirection entry
 * into an index. Until that is armed there is no honest 8-bit answer, and
 * inventing one is exactly the failure this refuses.
 */
static bool ioapic_dest_is_nameable(uint32_t dest_lapic_id, uint32_t gsi,
                                    const char *what) {
    if (dest_lapic_id <= 0xFFu) return true;

    kprintf("[IOAPIC] %[E]%s GSI %u not programmed: APIC ID %u does not fit "
            "the 8-bit destination of the redirection entry, and the low "
            "eight bits name the core with ID %u instead%[D]\n",
            what, gsi, dest_lapic_id, dest_lapic_id & 0xFFu);
    return false;
}

void ioapic_enable_irq(uint8_t gsi, uint8_t vector, uint32_t dest_lapic_id) {
    if (!ioapic_dest_is_nameable(dest_lapic_id, gsi, "IRQ")) return;

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
    uint32_t high = dest_lapic_id << 24;

    ioapic_write_redir(pin, low, high);

    debug_printf("[IOAPIC] GSI %u -> vector %u, dest LAPIC %u (pin %u)\n",
                 gsi, vector, dest_lapic_id, pin);
}

void ioapic_disable_irq(uint8_t gsi) {
    uint8_t pin = gsi - ioapic_gsi_base;

    if (pin > ioapic_max_entry) {
        return;
    }

    /* Read-modify-write of one 32-bit register: read and write-back are a
     * single operation, not two transactions that may be interleaved. */
    uint32_t reg = IOAPIC_REG_REDTBL + pin * 2;
    spin_lock(&ioapic_reg_lock);
    uint32_t low = ioapic_read_held(reg);
    ioapic_write_held(reg, low | IOAPIC_REDIR_MASKED);
    spin_unlock(&ioapic_reg_lock);
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
                                uint32_t dest_lapic_id) {
    if (!ioapic_dest_is_nameable(dest_lapic_id, gsi, "NMI Source")) return;

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
    uint32_t high = dest_lapic_id << 24;
    ioapic_write_redir(pin, redir, high);

    debug_printf("[IOAPIC] NMI Source: GSI %u pin %u mps=0x%04x dest=%u low=0x%08x\n",
                 gsi, pin, mps_flags, dest_lapic_id, redir);
}
