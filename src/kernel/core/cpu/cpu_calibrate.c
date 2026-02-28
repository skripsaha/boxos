#include "cpu_calibrate.h"
#include "cpu.h"
#include "pit.h"
#include "klib.h"
#include "atomics.h"  // For rdtsc()

// TSC frequency (cycles per millisecond). Must be calibrated before timers are used.
static volatile uint64_t tsc_freq_khz = 0;
static volatile int calibrated = 0;

void cpu_calibrate_tsc(void) {
    kprintf("\n");
    debug_printf("[CPU] Calibrating TSC frequency...\n");

    // PIT is accurate (1.193182 MHz crystal).
    // Using pit_delay_busy() instead of pit_sleep_ms() because HLT requires interrupts.
    uint64_t start_tsc = rdtsc();

    pit_delay_busy(100);

    uint64_t end_tsc = rdtsc();
    uint64_t elapsed_cycles = end_tsc - start_tsc;
    uint64_t elapsed_ms = 100;
    tsc_freq_khz = elapsed_cycles / elapsed_ms;

    calibrated = 1;

    debug_printf("[CPU] Calibration complete:\n");
    debug_printf("[CPU]   Measured time: %lu ms\n", elapsed_ms);
    debug_printf("[CPU]   TSC cycles: %lu\n", elapsed_cycles);
    debug_printf("[CPU]   TSC frequency: %lu kHz\n", tsc_freq_khz);
    debug_printf("[CPU]   TSC frequency: %lu MHz\n", tsc_freq_khz / 1000);
    uint64_t ghz_integer = (tsc_freq_khz / 1000) / 1000;
    uint64_t ghz_fraction = ((tsc_freq_khz / 1000) % 1000);
    if (ghz_integer > 0) {
        debug_printf("[CPU]   TSC frequency: %lu.%03lu GHz\n", ghz_integer, ghz_fraction);
    }
    kprintf("\n");
}

uint64_t cpu_ms_to_tsc(uint64_t ms) {
    if (!calibrated) {
        debug_printf("[CPU] WARNING: TSC not calibrated! Using fallback 2.4 GHz\n");
        return ms * 2400000;
    }
    return ms * tsc_freq_khz;
}

uint64_t cpu_tsc_to_ms(uint64_t cycles) {
    if (!calibrated) {
        debug_printf("[CPU] WARNING: TSC not calibrated! Using fallback 2.4 GHz\n");
        return cycles / 2400000;
    }

    if (tsc_freq_khz == 0) {
        return 0;
    }

    return cycles / tsc_freq_khz;
}

uint64_t cpu_get_tsc_freq_khz(void) {
    return tsc_freq_khz;
}

uint64_t cpu_get_tsc_freq_mhz(void) {
    return tsc_freq_khz / 1000;
}

int cpu_tsc_is_calibrated(void) {
    return calibrated;
}
