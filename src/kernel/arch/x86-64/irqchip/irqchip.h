#ifndef IRQCHIP_H
#define IRQCHIP_H

#include "ktypes.h"


#define IRQ_VECTOR_BASE         32
#define IRQ_MAX_COUNT           24
#define IRQ_VECTOR_MAX          (IRQ_VECTOR_BASE + IRQ_MAX_COUNT - 1)

#define LAPIC_TIMER_VECTOR      0xFE
#define LAPIC_SPURIOUS_VECTOR   0xFF

#define AHCI_MSI_VECTOR         0x70
#define XHCI_MSI_VECTOR         0x71
#define XHCI_MSI_VECTOR_COUNT   8
#define XHCI_MSI_VECTOR_LAST    (XHCI_MSI_VECTOR + XHCI_MSI_VECTOR_COUNT - 1)

#define IPI_WAKE_VECTOR         0xF0
#define IPI_SHOOTDOWN_VECTOR    0xF1
#define IPI_PANIC_VECTOR        0xF2

typedef enum {
    IRQCHIP_PIC,
    IRQCHIP_APIC
} irqchip_type_t;

typedef struct irq_chip {
    const char* name;
    void (*enable_irq)(uint8_t gsi);
    void (*disable_irq)(uint8_t gsi);
    void (*send_eoi)(uint8_t gsi);
    uint32_t (*get_isr)(void);
    uint32_t (*get_irr)(void);
    uint8_t max_irqs;
} irq_chip_t;

void irqchip_init(void);
void irqchip_enable_irq(uint8_t gsi);
void irqchip_disable_irq(uint8_t gsi);
void irqchip_send_eoi(uint8_t gsi);
uint32_t irqchip_get_isr(void);
uint32_t irqchip_get_irr(void);

irqchip_type_t irqchip_get_type(void);
const char* irqchip_get_name(void);
uint8_t irqchip_max_irqs(void);

struct madt_info;
const struct madt_info *irqchip_get_madt(void);

void irqchip_apply_lapic_nmi_self(void);

static inline uint8_t irq_gsi_to_vector(uint8_t gsi) {
    return IRQ_VECTOR_BASE + gsi;
}
static inline uint8_t irq_vector_to_gsi(uint8_t vector) {
    return vector - IRQ_VECTOR_BASE;
}

#endif