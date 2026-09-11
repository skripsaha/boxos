#ifndef CPU_CALIBRATE_H
#define CPU_CALIBRATE_H

#include "ktypes.h"

void cpu_calibrate_tsc(void);

uint64_t cpu_ms_to_tsc(uint64_t ms);
uint64_t cpu_tsc_to_ms(uint64_t cycles);
uint64_t cpu_tsc_to_us(uint64_t cycles);
uint64_t cpu_get_tsc_freq_khz(void);
uint64_t cpu_get_tsc_freq_mhz(void);
int cpu_tsc_is_calibrated(void);

uint64_t cpu_us_to_tsc(uint64_t us);

uint64_t cpu_tsc_architectural_khz(void);


void cpu_tsc_recal_tick(uint64_t now_us);

bool cpu_tsc_recal_if_pending(void);

bool cpu_tsc_recal_now(void);

#endif