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

#endif // CPU_CALIBRATE_H
