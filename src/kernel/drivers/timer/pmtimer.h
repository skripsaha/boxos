#ifndef PMTIMER_H
#define PMTIMER_H

#include "ktypes.h"
#include "acpi.h"

/*
 * ACPI Power Management Timer — ACPI 6.5 §4.8.3.3.
 *
 * A free-running 24- or 32-bit counter clocked at exactly 3.579545 MHz (the
 * NTSC colour subcarrier, which is why the number is what it is). It is
 * wall-clock-paced with no driver and no interrupt behind it, it is
 * independent of IRQ0 routing, and it is present on machines that have no
 * HPET — which is what makes it the ruler underneath the TSC.
 *
 * The timer's IDENTITY — where it lives, how wide it is — belongs here, in
 * the driver that reads it, not in the ACPI state block. acpi_parse_tables
 * is the code that holds the FADT, so it is the code that HANDS THE TIMER
 * OVER, once, through pmtimer_adopt_from_fadt. After that nothing outside
 * this file knows or needs to know where the counter is.
 */

#define PMTIMER_FREQ_HZ  3579545u   /* NTSC colour subcarrier, ACPI 6.5 §4.8.3.3 */

/* Take the PM Timer from the FADT. Called once, from acpi_parse_tables, the
 * moment g_acpi.fadt is set. Returns true when a counter was accepted; a
 * refusal is logged with its reason and leaves the timer absent rather than
 * leaving a truncated or wrongly-spaced address behind. */
bool pmtimer_adopt_from_fadt(const acpi_fadt_t *fadt);

/* True once a PM Timer has been adopted. */
bool pmtimer_is_present(void);

/* Read the counter. Returns false when there is no timer, and in that case
 * leaves *out untouched — a counter legitimately standing at 0 and a
 * machine with no counter at all are different facts and no longer share a
 * return value. */
bool pmtimer_read(uint32_t *out);

/* The width mask currently in force: 0x00FFFFFF until 32 bits have been
 * PROVEN by observing bits 24-31 change, 0xFFFFFFFF after. Callers that
 * subtract two samples need it to make a wrap subtract correctly. 0 when
 * the timer is absent. */
uint32_t pmtimer_mask(void);

/* Busy-wait `us` microseconds against the PM Timer specifically. Returns
 * false when there is no timer or when the counter died mid-wait — in
 * neither case did the caller get the time it asked for, and in neither
 * case is it told so by silence. Built on HourGlass so the kernel has one
 * waiting mechanism rather than one per driver. */
bool pmtimer_busy_wait_us(uint64_t us);

#endif /* PMTIMER_H */
