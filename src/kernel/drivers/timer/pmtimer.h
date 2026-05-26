#ifndef PMTIMER_H
#define PMTIMER_H

#include "ktypes.h"

/*
 * ACPI Power Management Timer (PM Timer) — ACPI 6.5 §4.8.3.3.
 *
 * A free-running 24- or 32-bit counter clocked at exactly
 * 3.579545 MHz (= NTSC color subcarrier). The width and I/O port
 * come from FADT (parsed by acpi_tables.c into g_acpi.pm_timer_*).
 *
 * Properties that make it useful for TSC calibration:
 *   - Wall-clock-paced even when HPET is absent or broken
 *   - Independent of IRQ0 routing (no driver/IRQ involvement)
 *   - Hardware-required on every ACPI 2.0+ machine — present where
 *     HPET may be missing (some pre-2010 boards, some Bochs configs)
 *
 * Wrap handling: 24-bit timer wraps every ~14 seconds (16777216 / 3.58M),
 * 32-bit every ~60 minutes. Both well above our 100 ms calibration
 * window, so we just mask delta to the timer width.
 */

#define PMTIMER_FREQ_HZ  3579545u   /* NTSC colour subcarrier */

/* True after acpi_parse_tables found a valid PM Timer block in FADT. */
bool pmtimer_is_present(void);

/* Read the raw counter (24-bit value zero-extended to uint32_t when the
 * timer is 24-bit). Wall-clock monotonic between wraps. */
uint32_t pmtimer_read(void);

/* Busy-wait approximately `us` microseconds against the PM Timer.
 * Used when HPET is absent — same role as hpet_busy_wait_us. */
void pmtimer_busy_wait_us(uint64_t us);

#endif /* PMTIMER_H */
