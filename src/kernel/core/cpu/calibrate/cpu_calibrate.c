#include "cpu_calibrate.h"
#include "cpu.h"
#include "pit.h"
#include "klib.h"
#include "atomics.h"  // For rdtsc()
#include "cpuid.h"
#include "cpu_caps_page.h"

// TSC frequency (cycles per millisecond). Must be calibrated before timers are used.
static volatile uint64_t tsc_freq_khz = 0;
static volatile int calibrated = 0;

// Try to get TSC frequency from CPUID leaf 0x15 (Intel: TSC/Core Crystal Clock)
// Returns frequency in kHz, or 0 if not available
static uint64_t cpuid_get_tsc_freq_khz(void) {
    if (g_cpu_caps.max_basic_leaf < 0x15) {
        return 0;
    }

    uint32_t eax, ebx, ecx, edx;
    cpuid(0x15, &eax, &ebx, &ecx, &edx);

    // eax = denominator, ebx = numerator, ecx = crystal clock frequency (Hz)
    // TSC freq = ecx * ebx / eax
    if (eax == 0 || ebx == 0) {
        return 0;
    }

    uint64_t crystal_hz = ecx;

    // If crystal frequency not reported, try known values based on CPU family
    if (crystal_hz == 0) {
        // Intel: leaf 0x15 ecx=0 means crystal freq not enumerated
        // Known crystal frequencies for Intel families:
        //   Skylake/Kaby Lake: 24 MHz
        //   Atom Goldmont:     19.2 MHz
        // We can't reliably guess, so skip CPUID method
        return 0;
    }

    // TSC frequency = crystal_hz * numerator / denominator
    uint64_t tsc_hz = (crystal_hz * (uint64_t)ebx) / (uint64_t)eax;
    return tsc_hz / 1000;  // Convert Hz to kHz
}

// Try to get TSC frequency from CPUID leaf 0x16 (Intel: Processor Frequency Information)
// Returns frequency in kHz, or 0 if not available
static uint64_t cpuid_get_base_freq_khz(void) {
    if (g_cpu_caps.max_basic_leaf < 0x16) {
        return 0;
    }

    uint32_t eax, ebx, ecx, edx;
    cpuid(0x16, &eax, &ebx, &ecx, &edx);

    // eax = base frequency in MHz
    if (eax == 0) {
        return 0;
    }

    return (uint64_t)eax * 1000;  // MHz to kHz
}

// Sanity check: frequency should be between 100 MHz and 10 GHz
static int freq_is_sane(uint64_t freq_khz) {
    return (freq_khz >= 100000 && freq_khz <= 10000000);
}

void cpu_calibrate_tsc(void) {
    kprintf("\n");
    debug_printf("[CPU] Calibrating TSC frequency...\n");

    // Method 1: Try CPUID leaf 0x15 (most accurate on modern Intel)
    uint64_t cpuid15_khz = cpuid_get_tsc_freq_khz();
    if (cpuid15_khz > 0) {
        debug_printf("[CPU]   CPUID.15h: %lu kHz\n", cpuid15_khz);
    }

    // Method 2: Try CPUID leaf 0x16 (base frequency, may differ from TSC)
    uint64_t cpuid16_khz = cpuid_get_base_freq_khz();
    if (cpuid16_khz > 0) {
        debug_printf("[CPU]   CPUID.16h: %lu kHz (base freq)\n", cpuid16_khz);
    }

    // Method 3: PIT calibration (universal, works on all x86-64)
    // PIT crystal is 1.193182 MHz — hardware standard since IBM PC.
    // Using pit_delay_busy() instead of pit_sleep_ms() because HLT requires interrupts.
    uint64_t start_tsc = rdtsc();
    pit_delay_busy(100);
    uint64_t end_tsc = rdtsc();

    uint64_t elapsed_cycles = end_tsc - start_tsc;
    uint64_t pit_khz = elapsed_cycles / 100;

    debug_printf("[CPU]   PIT calibration: %lu kHz (%lu cycles / 100 ms)\n", pit_khz, elapsed_cycles);

    // Select best source:
    // - CPUID 0x15 with crystal freq is authoritative (no measurement error)
    // - PIT calibration is the universal fallback
    // - CPUID 0x16 is informational only (base freq != TSC freq with turbo/SpeedStep)
    if (freq_is_sane(cpuid15_khz)) {
        tsc_freq_khz = cpuid15_khz;
        debug_printf("[CPU]   Using CPUID.15h value\n");
    } else if (freq_is_sane(pit_khz)) {
        tsc_freq_khz = pit_khz;
        debug_printf("[CPU]   Using PIT calibration value\n");
    } else {
        // Both methods gave insane results — use PIT anyway with warning
        tsc_freq_khz = pit_khz;
        debug_printf("[CPU] WARNING: TSC frequency looks abnormal: %lu kHz\n", pit_khz);
    }

    // Cross-check: if both CPUID and PIT available, warn on large divergence (>5%)
    if (freq_is_sane(cpuid15_khz) && freq_is_sane(pit_khz)) {
        uint64_t diff;
        if (cpuid15_khz > pit_khz)
            diff = cpuid15_khz - pit_khz;
        else
            diff = pit_khz - cpuid15_khz;

        uint64_t pct = (diff * 100) / pit_khz;
        if (pct > 5) {
            debug_printf("[CPU] WARNING: CPUID vs PIT divergence: %lu%% (CPUID=%lu, PIT=%lu)\n",
                         pct, cpuid15_khz, pit_khz);
        }
    }

    calibrated = 1;

    // Publish to userspace via CPU capabilities page
    cpu_caps_page_set_tsc_freq(tsc_freq_khz);

    debug_printf("[CPU] Calibration complete:\n");
    debug_printf("[CPU]   TSC frequency: %lu kHz\n", tsc_freq_khz);
    debug_printf("[CPU]   TSC frequency: %lu MHz\n", tsc_freq_khz / 1000);
    uint64_t ghz_integer = (tsc_freq_khz / 1000) / 1000;
    uint64_t ghz_fraction = ((tsc_freq_khz / 1000) % 1000);
    if (ghz_integer > 0) {
        debug_printf("[CPU]   TSC frequency: %lu.%03lu GHz\n", ghz_integer, ghz_fraction);
    }
    if (!g_cpu_caps.has_invariant_tsc) {
        debug_printf("[CPU] WARNING: CPU does not report Invariant TSC! Timekeeping may drift.\n");
    }
    kprintf("\n");
}

uint64_t cpu_ms_to_tsc(uint64_t ms) {
    if (!calibrated) {
        // Not calibrated yet — use PIT busy-wait to measure on-the-fly
        // This is slow but correct. Should only happen during very early boot.
        debug_printf("[CPU] WARNING: TSC not calibrated, measuring on-the-fly\n");
        uint64_t start = rdtsc();
        pit_delay_busy(1);
        uint64_t cycles_per_ms = rdtsc() - start;
        return ms * cycles_per_ms;
    }
    return ms * tsc_freq_khz;
}

uint64_t cpu_tsc_to_ms(uint64_t cycles) {
    if (!calibrated) {
        debug_printf("[CPU] WARNING: TSC not calibrated, measuring on-the-fly\n");
        uint64_t start = rdtsc();
        pit_delay_busy(1);
        uint64_t cycles_per_ms = rdtsc() - start;
        if (cycles_per_ms == 0) return 0;
        return cycles / cycles_per_ms;
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
