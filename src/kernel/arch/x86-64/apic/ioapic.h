#ifndef IOAPIC_H
#define IOAPIC_H

/* Mask every redirection entry this chip implements. The count comes from the
 * chip's own VER register, so a part with more inputs than the classic
 * twenty-four is fully quieted rather than quieted as far as a number in a
 * header happened to reach. */
void IoapicMaskEveryPin(void);

#include "ktypes.h"

// IO-APIC register access (indirect via IOREGSEL/IOWIN)
#define IOAPIC_IOREGSEL     0x00    // Register select (write index here)
#define IOAPIC_IOWIN        0x10    // Register data (read/write value here)

// IO-APIC registers (accessed via IOREGSEL)
#define IOAPIC_REG_ID       0x00    // IO-APIC ID
#define IOAPIC_REG_VER      0x01    // IO-APIC Version (bits 16-23 = max redirection entry)
#define IOAPIC_REG_ARB      0x02    // IO-APIC Arbitration ID
#define IOAPIC_REG_REDTBL   0x10    // Redirection Table base (entries at 0x10 + 2*n)

// Redirection table entry flags (low 32 bits) — Intel 82093AA §3.2.4
#define IOAPIC_REDIR_VECTOR_MASK    0xFF
#define IOAPIC_REDIR_DELMOD_FIXED   (0 << 8)
#define IOAPIC_REDIR_DELMOD_LOWEST  (1 << 8)
#define IOAPIC_REDIR_DELMOD_SMI     (2 << 8)
#define IOAPIC_REDIR_DELMOD_NMI     (4 << 8)
#define IOAPIC_REDIR_DELMOD_INIT    (5 << 8)
#define IOAPIC_REDIR_DELMOD_EXTINT  (7 << 8)
#define IOAPIC_REDIR_DESTMOD_PHYS   (0 << 11)
#define IOAPIC_REDIR_DESTMOD_LOGIC  (1 << 11)
#define IOAPIC_REDIR_POLARITY_HIGH  (0 << 13)
#define IOAPIC_REDIR_POLARITY_LOW   (1 << 13)
#define IOAPIC_REDIR_TRIGGER_EDGE   (0 << 15)
#define IOAPIC_REDIR_TRIGGER_LEVEL  (1 << 15)
#define IOAPIC_REDIR_MASKED         (1 << 16)

// ISA IRQ to GSI override
typedef struct {
    uint8_t  isa_irq;       // Source (ISA IRQ number)
    uint32_t gsi;           // Global System Interrupt number
    uint16_t flags;         // MPS INTI flags (polarity, trigger mode)
    bool     active;        // Whether this override is in use
} ioapic_iso_t;

/* `gsi_base` is the I/O APIC Structure's Global System Interrupt Base, and
 * ACPI 6.5 §5.2.12.3 makes that field 32 bits wide — §5.2.13 numbers GSIs
 * through every IOAPIC in the machine, so a second chip's base is not a small
 * number. Taking it as a byte put the narrowing here, at the boundary, where
 * the IOAPIC could not see that its own base had moved. */
void ioapic_init(uintptr_t base_addr, uint32_t gsi_base);

/* `dest_lapic_id` is a FULL-WIDTH APIC ID. The redirection entry carries
 * eight bits of it (Intel 82093AA §3.2.4, high dword), so an ID above 0xFF
 * is REFUSED and named inside ioapic.c rather than truncated onto a
 * different, running core. The parameter is uint32_t for that reason: an
 * eight-bit parameter puts the narrowing at the call site, where nothing
 * inside the IOAPIC can see it.
 *
 * `gsi` is 32 bits for the same reason and in the same words: every ACPI
 * field that carries a Global System Interrupt is four bytes wide — the I/O
 * APIC Structure's base (§5.2.12.3, Table 5.24) and the Interrupt Source
 * Override's target (§5.2.12.5, Table 5.25) — and a byte here would make
 * every caller narrow it silently at the call site. (§5.2.13, cited here on
 * its own before, is where GSIs are NUMBERED across the machine; it states
 * no width.) */
void ioapic_enable_irq(uint32_t gsi, uint8_t vector, uint32_t dest_lapic_id);
void ioapic_disable_irq(uint32_t gsi);

// Interrupt Source Override management
void ioapic_register_iso(uint8_t isa_irq, uint32_t gsi, uint16_t flags);

/* Polarity and trigger for a GSI, from whichever source described it —
 * firmware's Interrupt Source Override first, then anything the kernel had to
 * say for a line firmware left unsaid. False when nobody described it and the
 * bus default applies. */
bool ioapic_gsi_flags(uint32_t gsi, uint16_t *out_flags);

/* Describe a line firmware did not. Flags use the MADT encoding: bits 1:0
 * polarity (11 = active low), bits 3:2 trigger (11 = level). An existing
 * firmware override for the same GSI still wins. */
void ioapic_describe_gsi(uint32_t gsi, uint16_t flags);
uint32_t ioapic_isa_to_gsi(uint8_t isa_irq);
uint16_t ioapic_get_iso_flags(uint8_t isa_irq);

/* Program a redirection entry as an NMI Source (MADT Type 3 / IOAPIC
 * Delivery Mode 100). The vector field is don't care for NMI per Intel
 * 82093AA §3.2.4; polarity and trigger come from the MPS INTI flag bits
 * provided in the MADT NMI Source structure. */
void ioapic_program_nmi_source(uint32_t gsi, uint16_t mps_flags,
                                uint32_t dest_lapic_id);

#endif // IOAPIC_H
