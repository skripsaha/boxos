#ifndef HPET_H
#define HPET_H

#include "ktypes.h"

/*
 * HPET (High Precision Event Timer) driver — Intel HPET Specification 1.0a.
 *
 * BoxOS uses the HPET main counter as a high-resolution monotonic time
 * source. The IA-PC HPET ACPI table tells us where the controller lives;
 * everything else is read from the controller's own General Capabilities
 * register. PIT remains live as the legacy tick source so this driver
 * adds capability without dropping a known-working code path.
 *
 * Register offsets (relative to MMIO base, all 64-bit aligned):
 *   0x000  GCAP_ID    General Capabilities (counter period at bits 63:32,
 *                     vendor at 31:16, LEG_RT_CAP at 15, COUNT_SIZE_CAP at 13,
 *                     NUM_TIM_CAP at 12:8, REV_ID at 7:0)
 *   0x010  GCONF      General Configuration (ENABLE_CNF bit 0, LEG_RT_CNF bit 1)
 *   0x020  GINTR_STA  Interrupt Status (write-1-to-clear, level-trig only)
 *   0x0F0  MCNT       Main counter value (64-bit; reads of upper half on a
 *                     32-bit-only controller require two MMIO accesses)
 *   0x100  T0_CNF     Timer 0 Configuration & Capabilities
 *   0x108  T0_CMP     Timer 0 Comparator
 *   ...    Timer n at 0x100 + 0x20*n
 *
 * The main counter period is reported in femtoseconds (10^-15 s); we
 * convert to a ticks-per-microsecond ratio at init.
 */

#define HPET_REG_GCAP_ID         0x000
#define HPET_REG_GCONF           0x010
#define HPET_REG_GINTR_STA       0x020
#define HPET_REG_MCNT            0x0F0
#define HPET_REG_TIMER_CNF(n)    (0x100 + (uint32_t)(n) * 0x20)
#define HPET_REG_TIMER_CMP(n)    (0x108 + (uint32_t)(n) * 0x20)
#define HPET_REG_TIMER_FSB(n)    (0x110 + (uint32_t)(n) * 0x20)

#define HPET_GCAP_REV_ID_MASK    0xFFu
#define HPET_GCAP_NUM_TIM_SHIFT  8
#define HPET_GCAP_NUM_TIM_MASK   0x1F
#define HPET_GCAP_COUNT_SIZE_CAP (1u << 13)
#define HPET_GCAP_LEG_RT_CAP     (1u << 15)
#define HPET_GCAP_VENDOR_SHIFT   16
#define HPET_GCAP_PERIOD_SHIFT   32      /* fs per tick, must be 1..0x05F5E100 */

#define HPET_GCONF_ENABLE        (1u << 0)
#define HPET_GCONF_LEG_RT        (1u << 1)

#define HPET_TIMER_INT_TYPE      (1u << 1)   /* 0=edge, 1=level */
#define HPET_TIMER_INT_ENB       (1u << 2)
#define HPET_TIMER_TYPE_PERIODIC (1u << 3)
#define HPET_TIMER_PER_INT_CAP   (1u << 4)   /* RO; periodic capable */
#define HPET_TIMER_SIZE_CAP      (1u << 5)   /* RO; 1=64-bit timer */
#define HPET_TIMER_VAL_SET       (1u << 6)   /* "directly set accumulator" */
#define HPET_TIMER_32BIT_MODE    (1u << 8)
#define HPET_TIMER_INT_ROUTE_S   9
#define HPET_TIMER_FSB_EN        (1u << 14)
#define HPET_TIMER_FSB_CAP       (1u << 15)

bool hpet_init(void);
bool hpet_is_present(void);

/* Program HPET timer 0 in periodic mode delivering at `hz` Hz on IRQ 0
 * via the LegacyReplacement routing (timer 0 → ISA IRQ 0, timer 1 →
 * ISA IRQ 8). Returns true on success; false if timer 0 is not
 * periodic-capable or LegacyReplacement is unsupported.
 *
 * After this returns true:
 *   - the kernel system tick (pit_tick) is sourced from HPET
 *   - PIT counter programming becomes redundant and is skipped
 *     in pit_init() to avoid double-counting */
bool hpet_start_legacy_tick(uint32_t hz);

/* True when LegacyReplacement is active and HPET is sourcing IRQ0. */
bool hpet_tick_active(void);

/* Read the main counter (returns nanoseconds-equivalent monotonic counter
 * in microseconds since hpet_init success). Returns 0 if HPET absent. */
uint64_t hpet_now_us(void);

/* Busy-wait for `us` microseconds using the HPET main counter. Returns
 * immediately if HPET is absent — caller should fall back to pit_delay_busy. */
void hpet_busy_wait_us(uint64_t us);

/* Read-only diagnostics. */
uint64_t hpet_period_fs(void);
uint8_t  hpet_num_comparators(void);
bool     hpet_counter_is_64bit(void);

#endif /* HPET_H */
