#ifndef LAPIC_H
#define LAPIC_H

#include "ktypes.h"

#define LAPIC_REG_ID            0x020
#define LAPIC_REG_VERSION       0x030
#define LAPIC_REG_TPR           0x080
#define LAPIC_REG_EOI           0x0B0
#define LAPIC_REG_SVR           0x0F0
#define LAPIC_REG_ISR_BASE      0x100
#define LAPIC_REG_IRR_BASE      0x200
#define LAPIC_REG_ESR           0x280
#define LAPIC_REG_ICR_LOW       0x300
#define LAPIC_REG_ICR_HIGH      0x310
#define LAPIC_REG_TIMER_LVT     0x320
#define LAPIC_REG_LINT0_LVT     0x350
#define LAPIC_REG_LINT1_LVT     0x360
#define LAPIC_REG_TIMER_ICR     0x380
#define LAPIC_REG_TIMER_CCR     0x390
#define LAPIC_REG_TIMER_DCR     0x3E0

#define LAPIC_SVR_ENABLE        (1 << 8)

#define LAPIC_LVT_MASKED            (1u << 16)
#define LAPIC_LVT_TIMER_PERIODIC    (1u << 17)
#define LAPIC_LVT_TIMER_TSC_DEADLINE (2u << 17)
#define LAPIC_LVT_TRIGGER_LEVEL     (1u << 15)
#define LAPIC_LVT_REMOTE_IRR        (1u << 14)
#define LAPIC_LVT_PIN_POLARITY_LOW  (1u << 13)
#define LAPIC_LVT_DELIVERY_FIXED    (0u << 8)
#define LAPIC_LVT_DELIVERY_SMI      (2u << 8)
#define LAPIC_LVT_DELIVERY_NMI      (4u << 8)
#define LAPIC_LVT_DELIVERY_EXTINT   (7u << 8)
#define LAPIC_LVT_DELIVERY_INIT     (5u << 8)

#define LAPIC_TIMER_DIV_1       0x0B
#define LAPIC_TIMER_DIV_2       0x00
#define LAPIC_TIMER_DIV_4       0x01
#define LAPIC_TIMER_DIV_8       0x02
#define LAPIC_TIMER_DIV_16      0x03
#define LAPIC_TIMER_DIV_32      0x08
#define LAPIC_TIMER_DIV_64      0x09
#define LAPIC_TIMER_DIV_128     0x0A

#define MSR_APIC_BASE           0x1B
#define MSR_APIC_BASE_ENABLE    (1 << 11)
#define MSR_APIC_BASE_EXTD      (1 << 10)
#define MSR_APIC_BASE_BSP       (1 << 8)
#define MSR_APIC_BASE_ADDR_MASK 0xFFFFF000ULL

#define MSR_IA32_TSC_DEADLINE   0x6E0

#define MSR_X2APIC_BASE         0x800
#define MSR_X2APIC_APICID       0x802
#define MSR_X2APIC_ICR          0x830

void lapic_init(uintptr_t base_addr);
void lapic_send_eoi(void);
uint32_t lapic_get_id(void);
void lapic_enable(void);
void lapic_disable(void);
bool lapic_is_enabled(void);
uintptr_t lapic_get_base(void);
bool lapic_is_mapped(void);

void lapic_timer_init(uint8_t vector, uint32_t frequency_hz);
void lapic_timer_stop(void);

void lapic_timer_rearm(void);

uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t value);

#define LAPIC_IPI_INIT          0x00004500
#define LAPIC_IPI_SIPI          0x00004600

#define LAPIC_ICR_SEND_PENDING  (1 << 12)

void lapic_send_ipi(uint32_t dest_lapic_id, uint8_t vector);
void lapic_send_ipi_all_excluding_self(uint8_t vector);

bool lapic_is_x2apic_active(void);

void lapic_icr_write(uint32_t dest_id, uint32_t cmd);

struct madt_info;
void lapic_apply_madt_nmi(const struct madt_info *info,
                          uint8_t acpi_processor_id);

#endif