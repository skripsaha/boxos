#ifndef IOAPIC_H
#define IOAPIC_H

#include "ktypes.h"

#define IOAPIC_IOREGSEL     0x00
#define IOAPIC_IOWIN        0x10

#define IOAPIC_REG_ID       0x00
#define IOAPIC_REG_VER      0x01
#define IOAPIC_REG_ARB      0x02
#define IOAPIC_REG_REDTBL   0x10

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

#define IOAPIC_MAX_PINS     24

typedef struct {
    uint8_t  isa_irq;
    uint32_t gsi;
    uint16_t flags;
    bool     active;
} ioapic_iso_t;

void ioapic_init(uintptr_t base_addr, uint8_t gsi_base);
void ioapic_enable_irq(uint8_t gsi, uint8_t vector, uint8_t dest_lapic_id);
void ioapic_disable_irq(uint8_t gsi);
void ioapic_set_irq(uint8_t gsi, uint8_t vector, uint8_t dest, uint32_t flags);
uint8_t ioapic_get_max_entries(void);
uintptr_t ioapic_get_base(void);

void ioapic_register_iso(uint8_t isa_irq, uint32_t gsi, uint16_t flags);

bool ioapic_gsi_flags(uint32_t gsi, uint16_t *out_flags);

void ioapic_describe_gsi(uint32_t gsi, uint16_t flags);
uint32_t ioapic_isa_to_gsi(uint8_t isa_irq);
uint16_t ioapic_get_iso_flags(uint8_t isa_irq);

void ioapic_program_nmi_source(uint32_t gsi, uint16_t mps_flags,
                                uint8_t dest_lapic_id);

uint32_t ioapic_read(uint32_t reg);
void ioapic_write(uint32_t reg, uint32_t value);

#endif