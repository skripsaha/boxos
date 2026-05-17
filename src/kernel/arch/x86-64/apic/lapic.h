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

// LVT flags (Intel SDM Vol 3 §11.5.1)
#define LAPIC_LVT_MASKED            (1u << 16)   // Interrupt masked
#define LAPIC_LVT_TIMER_PERIODIC    (1u << 17)   // Periodic timer mode
#define LAPIC_LVT_TRIGGER_LEVEL     (1u << 15)   // 1 = level, 0 = edge
#define LAPIC_LVT_REMOTE_IRR        (1u << 14)   // RO; for level-triggered
#define LAPIC_LVT_PIN_POLARITY_LOW  (1u << 13)   // 1 = active low, 0 = active high
#define LAPIC_LVT_DELIVERY_FIXED    (0u << 8)
#define LAPIC_LVT_DELIVERY_SMI      (2u << 8)
#define LAPIC_LVT_DELIVERY_NMI      (4u << 8)
#define LAPIC_LVT_DELIVERY_EXTINT   (7u << 8)
#define LAPIC_LVT_DELIVERY_INIT     (5u << 8)

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
#define MSR_APIC_BASE_EXTD      (1 << 10)   // x2APIC enable bit
#define MSR_APIC_BASE_BSP       (1 << 8)
#define MSR_APIC_BASE_ADDR_MASK 0xFFFFF000ULL

// x2APIC MSR layout (Intel SDM Vol 3A §10.12.1)
#define MSR_X2APIC_BASE         0x800       // x2APIC MSR window starts here
#define MSR_X2APIC_ICR          0x830       // ICR: single 64-bit MSR write

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

// IPI (Inter-Processor Interrupt) command values for ICR
#define LAPIC_IPI_INIT          0x00004500  // INIT assert
#define LAPIC_IPI_INIT_DEASSERT 0x00008500  // INIT de-assert (level-triggered)
#define LAPIC_IPI_SIPI          0x00004600  // Startup IPI (OR with page number in low byte)

// ICR delivery status bit
#define LAPIC_ICR_SEND_PENDING  (1 << 12)

// IPI delivery
void lapic_send_ipi(uint8_t dest_lapic_id, uint8_t vector);
void lapic_send_ipi_all_excluding_self(uint8_t vector);

/* Returns true if the local APIC is currently running in x2APIC mode
 * (IA32_APIC_BASE bit 10 = EXTD). Sampled once at lapic_init and cached;
 * BoxOS never disables x2APIC at runtime so the cached value is stable.
 * Used by lapic_icr_write to pick the right register access path. */
bool lapic_is_x2apic_active(void);

/* Write the LAPIC Interrupt Command Register in a mode-correct way.
 *
 *   xAPIC:  busy-poll delivery-status bit, write ICR_HIGH (dest<<24),
 *           write ICR_LOW (cmd) — the LOW write triggers the send.
 *   x2APIC: single wrmsr(0x830, (dest << 32) | cmd) — write is atomic
 *           and the CPU serialises delivery, no busy-poll required.
 *
 * Writing the legacy MMIO offsets (ICR_HIGH=0x310, ICR_LOW=0x300) on an
 * x2APIC-enabled CPU is reserved (Intel SDM Vol 3A §10.12.9) and faults
 * with #GP — every IPI path must go through this helper after x2APIC
 * has been activated. */
void lapic_icr_write(uint32_t dest_id, uint32_t cmd);

/* Apply MADT Local APIC NMI entries to the currently-running CPU's LVT.
 *
 * ACPI 6.5 §5.2.12.7: each Local APIC NMI Structure specifies which LINT
 * pin (LINT0 or LINT1) is wired to NMI on a given processor (acpi_proc_id),
 * or on all processors (0xFFu). OSPM is responsible for programming the
 * LVT to deliver NMI for those pins. Without this step the firmware-
 * provided NMI source (watchdog, IPMI alert, server PSU fault) never
 * reaches the OS.
 *
 * `acpi_processor_id` is the ACPI processor ID of THIS CPU (from the
 * matching MADT_TYPE_LOCAL_APIC entry); the function only programs
 * entries whose processor_id is 0xFF or matches.
 */
struct madt_info; /* forward decl — full definition in acpi_madt.h */
void lapic_apply_madt_nmi(const struct madt_info *info,
                          uint8_t acpi_processor_id);

#endif // LAPIC_H
