#ifndef CPU_CALIBRATE_H
#define CPU_CALIBRATE_H

#include "ktypes.h"

// Calibrate TSC frequency using PIT timer. Must be called after PIT initialization.
void cpu_calibrate_tsc(void);

uint64_t cpu_ms_to_tsc(uint64_t ms);
uint64_t cpu_tsc_to_ms(uint64_t cycles);
uint64_t cpu_tsc_to_us(uint64_t cycles);
uint64_t cpu_get_tsc_freq_khz(void);
uint64_t cpu_get_tsc_freq_mhz(void);
int cpu_tsc_is_calibrated(void);

/* ----------------------------------------------------------------
 * Periodic TSC recalibration.
 *
 * Even with Invariant TSC the calibrated rate can drift in the field:
 *   - non-invariant silicon (pre-Nehalem Intel, some embedded AMD)
 *     changes TSC with P-state transitions
 *   - virtualised hosts (KVM live-migrate, Hyper-V VM pause/resume,
 *     QEMU host time slips) may reset the TSC under us
 *   - the initial calibration window can be poisoned by a single
 *     SMI or IRQ storm and overshoot/undershoot
 *
 * Mitigation: re-measure TSC every TSC_RECAL_INTERVAL_US against the
 * most-trusted available clocksource (pvclock if present, HPET counter
 * otherwise), and atomically swap tsc_freq_khz on drift ≥1%.
 *
 * Trigger model: PIT IRQ stamps the wall-clock time and sets a
 * `pending` flag when the interval elapses. The flag is consumed in
 * BSP idle context (cpu_tsc_recal_if_pending → cpu_tsc_recal_now)
 * outside any IRQ — the 20 ms HPET measurement window inside recal
 * would be unsafe at IRQ priority.
 *
 * Skip conditions:
 *   - tsc_freq_khz has never been calibrated (initial boot not done)
 *   - neither pvclock nor HPET is available (no ground truth)
 *
 * Concurrency: single-reader (BSP idle), single-writer pattern. The
 * pending flag is an atomic CAS so a second trigger from PIT IRQ
 * inside the recal window cannot double-fire.
 */

/* Called once per PIT tick from the BSP IRQ0 handler. Cheap atomic
 * comparison; sets the pending flag when 10+ seconds have passed
 * since the last recalibration. No-op until first cpu_calibrate_tsc(). */
void cpu_tsc_recal_tick(uint64_t now_us);

/* Called from idle context (cpu_idle in idle.c). If pending, perform
 * a 20 ms HPET-measured recalibration and atomically publish the new
 * frequency on drift. Returns true if a recalibration was performed
 * (regardless of whether the frequency was updated). */
bool cpu_tsc_recal_if_pending(void);

/* Force a recalibration now (synchronous). Returns true if a new
 * frequency was successfully measured (whether it was applied
 * depends on the drift threshold). Mostly for diagnostics. */
bool cpu_tsc_recal_now(void);

#endif // CPU_CALIBRATE_H
