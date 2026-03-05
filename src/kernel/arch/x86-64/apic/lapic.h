#ifndef LAPIC_H
#define LAPIC_H

#include "ktypes.h"

// Local APIC register offsets (from LAPIC base address)
#define LAPIC_REG_ID            0x020   // Local APIC ID
#define LAPIC_REG_VERSION       0x030   // Local APIC Version
#define LAPIC_REG_TPR           0x080   // Task Priority Register
#define LAPIC_REG_EOI           0x0B0   // End of Interrupt
#define LAPIC_REG_SVR           0x0F0   // Spurious Interrupt Vector Register
#define LAPIC_REG_ISR_BASE      0x100   // In-Service Register (8 x 32-bit)
#define LAPIC_REG_IRR_BASE      0x200   // Interrupt Request Register (8 x 32-bit)
#define LAPIC_REG_ESR           0x280   // Error Status Register
#define LAPIC_REG_ICR_LOW       0x300   // Interrupt Command Register (low 32 bits)
#define LAPIC_REG_ICR_HIGH      0x310   // Interrupt Command Register (high 32 bits)
#define LAPIC_REG_TIMER_LVT     0x320   // Timer LVT Entry
#define LAPIC_REG_LINT0_LVT     0x350   // Local Interrupt 0 LVT
#define LAPIC_REG_LINT1_LVT     0x360   // Local Interrupt 1 LVT
#define LAPIC_REG_TIMER_ICR     0x380   // Timer Initial Count Register
#define LAPIC_REG_TIMER_CCR     0x390   // Timer Current Count Register
#define LAPIC_REG_TIMER_DCR     0x3E0   // Timer Divide Configuration Register

// SVR flags
#define LAPIC_SVR_ENABLE        (1 << 8)    // APIC Software Enable

// LVT flags
#define LAPIC_LVT_MASKED        (1 << 16)   // Interrupt masked
#define LAPIC_LVT_TIMER_PERIODIC (1 << 17)  // Periodic timer mode

// Timer divider values for DCR
#define LAPIC_TIMER_DIV_1       0x0B
#define LAPIC_TIMER_DIV_2       0x00
#define LAPIC_TIMER_DIV_4       0x01
#define LAPIC_TIMER_DIV_8       0x02
#define LAPIC_TIMER_DIV_16      0x03
#define LAPIC_TIMER_DIV_32      0x08
#define LAPIC_TIMER_DIV_64      0x09
#define LAPIC_TIMER_DIV_128     0x0A

// IA32_APIC_BASE MSR
#define MSR_APIC_BASE           0x1B
#define MSR_APIC_BASE_ENABLE    (1 << 11)
#define MSR_APIC_BASE_BSP       (1 << 8)
#define MSR_APIC_BASE_ADDR_MASK 0xFFFFF000ULL

void lapic_init(uintptr_t base_addr);
void lapic_send_eoi(void);
uint32_t lapic_get_id(void);
void lapic_enable(void);
void lapic_disable(void);
bool lapic_is_enabled(void);
uintptr_t lapic_get_base(void);

// LAPIC timer
void lapic_timer_init(uint8_t vector, uint32_t frequency_hz);
void lapic_timer_stop(void);

// Register access
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t value);

#endif // LAPIC_H
