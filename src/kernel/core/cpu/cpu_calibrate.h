#ifndef CPU_CALIBRATE_H
#define CPU_CALIBRATE_H

#include "ktypes.h"

// ============================================================================
// CPU FREQUENCY CALIBRATION
// ============================================================================
//
// Dynamically detects CPU TSC frequency at boot time.
// This is CRITICAL for accurate timing on different hardware.
//
// ============================================================================

// Calibrate TSC frequency using PIT timer
// MUST be called early in boot (after PIT initialization)
void cpu_calibrate_tsc(void);

// Convert milliseconds to TSC cycles (accurate after calibration)
uint64_t cpu_ms_to_tsc(uint64_t ms);

// Convert TSC cycles to milliseconds (accurate after calibration)
uint64_t cpu_tsc_to_ms(uint64_t cycles);

// Get TSC frequency in kHz (cycles per millisecond)
uint64_t cpu_get_tsc_freq_khz(void);

// Get TSC frequency in MHz (for display)
uint64_t cpu_get_tsc_freq_mhz(void);

// Check if TSC is calibrated
int cpu_tsc_is_calibrated(void);

#endif // CPU_CALIBRATE_H
