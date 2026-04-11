#ifndef IRQCHIP_H
#define IRQCHIP_H

#include "ktypes.h"

// Vector layout:
//   0-31:   CPU exceptions
//   32-55:  Hardware IRQs (GSI 0-23, covers PIC + IO-APIC)
//   128:    Syscall (INT 0x80)
//   254:    LAPIC timer
//   255:    LAPIC spurious

#define IRQ_VECTOR_BASE         32
#define IRQ_MAX_COUNT           24      // Standard IO-APIC: 24 pins (GSI 0-23)
#define IRQ_VECTOR_MAX          (IRQ_VECTOR_BASE + IRQ_MAX_COUNT - 1)  // 55

#define LAPIC_TIMER_VECTOR      0xFE    // 254
#define LAPIC_SPURIOUS_VECTOR   0xFF    // 255

// AMP inter-processor interrupt vectors
#define IPI_WAKE_VECTOR         0xF0    // 240 — wake idle AP or reschedule
#define IPI_SHOOTDOWN_VECTOR    0xF1    // 241 — TLB shootdown
#define IPI_PANIC_VECTOR        0xF2    // 242 — broadcast halt on panic

// Interrupt controller type
typedef enum {
    IRQCHIP_PIC,        // Legacy 8259A PIC (fallback)
    IRQCHIP_APIC        // Local APIC + IO-APIC (preferred)
} irqchip_type_t;

// Abstract interrupt controller operations
typedef struct irq_chip {
    const char* name;
    void (*enable_irq)(uint8_t gsi);
    void (*disable_irq)(uint8_t gsi);
    void (*send_eoi)(uint8_t gsi);
    uint32_t (*get_isr)(void);
    uint32_t (*get_irr)(void);
    uint8_t max_irqs;
} irq_chip_t;

// Global API - all drivers use these instead of pic_* directly
void irqchip_init(void);
void irqchip_enable_irq(uint8_t gsi);
void irqchip_disable_irq(uint8_t gsi);
void irqchip_send_eoi(uint8_t gsi);
uint32_t irqchip_get_isr(void);
uint32_t irqchip_get_irr(void);

irqchip_type_t irqchip_get_type(void);
const char* irqchip_get_name(void);
uint8_t irqchip_max_irqs(void);

// Convert between GSI and IDT vector
static inline uint8_t irq_gsi_to_vector(uint8_t gsi) {
    return IRQ_VECTOR_BASE + gsi;
}
static inline uint8_t irq_vector_to_gsi(uint8_t vector) {
    return vector - IRQ_VECTOR_BASE;
}

#endif // IRQCHIP_H
