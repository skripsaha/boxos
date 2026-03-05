#ifndef IOAPIC_H
#define IOAPIC_H

#include "ktypes.h"

// IO-APIC register access (indirect via IOREGSEL/IOWIN)
#define IOAPIC_IOREGSEL     0x00    // Register select (write index here)
#define IOAPIC_IOWIN        0x10    // Register data (read/write value here)

// IO-APIC registers (accessed via IOREGSEL)
#define IOAPIC_REG_ID       0x00    // IO-APIC ID
#define IOAPIC_REG_VER      0x01    // IO-APIC Version (bits 16-23 = max redirection entry)
#define IOAPIC_REG_ARB      0x02    // IO-APIC Arbitration ID
#define IOAPIC_REG_REDTBL   0x10    // Redirection Table base (entries at 0x10 + 2*n)

// Redirection table entry flags (low 32 bits)
#define IOAPIC_REDIR_VECTOR_MASK    0xFF
#define IOAPIC_REDIR_DELMOD_FIXED   (0 << 8)
#define IOAPIC_REDIR_DELMOD_LOWEST  (1 << 8)
#define IOAPIC_REDIR_DESTMOD_PHYS   (0 << 11)
#define IOAPIC_REDIR_DESTMOD_LOGIC  (1 << 11)
#define IOAPIC_REDIR_POLARITY_HIGH  (0 << 13)
#define IOAPIC_REDIR_POLARITY_LOW   (1 << 13)
#define IOAPIC_REDIR_TRIGGER_EDGE   (0 << 15)
#define IOAPIC_REDIR_TRIGGER_LEVEL  (1 << 15)
#define IOAPIC_REDIR_MASKED         (1 << 16)

// Maximum IO-APIC pins we support
#define IOAPIC_MAX_PINS     24

// ISA IRQ to GSI override
typedef struct {
    uint8_t  isa_irq;       // Source (ISA IRQ number)
    uint32_t gsi;           // Global System Interrupt number
    uint16_t flags;         // MPS INTI flags (polarity, trigger mode)
    bool     active;        // Whether this override is in use
} ioapic_iso_t;

void ioapic_init(uintptr_t base_addr, uint8_t gsi_base);
void ioapic_enable_irq(uint8_t gsi, uint8_t vector, uint8_t dest_lapic_id);
void ioapic_disable_irq(uint8_t gsi);
void ioapic_set_irq(uint8_t gsi, uint8_t vector, uint8_t dest, uint32_t flags);
uint8_t ioapic_get_max_entries(void);
uintptr_t ioapic_get_base(void);

// Interrupt Source Override management
void ioapic_register_iso(uint8_t isa_irq, uint32_t gsi, uint16_t flags);
uint32_t ioapic_isa_to_gsi(uint8_t isa_irq);
uint16_t ioapic_get_iso_flags(uint8_t isa_irq);

// Register access
uint32_t ioapic_read(uint32_t reg);
void ioapic_write(uint32_t reg, uint32_t value);

#endif // IOAPIC_H
