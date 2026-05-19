#include "cpu_calibrate.h"
#include "cpu.h"
#include "pit.h"
#include "hpet.h"
#include "klib.h"
#include "atomics.h"  // For rdtsc()
#include "cpuid.h"
#include "cpu_caps_page.h"

/* TSC frequency calibration — production order of preference:
 *
 *   1. CPUID.15h        Intel's canonical TSC ratio + crystal Hz. No
 *                       measurement error. Available on Skylake-and-newer
 *                       when firmware enumerates ECX (crystal frequency).
 *
 *   2. CPUID.15h fixed  Same leaf, but ECX=0 means the crystal Hz is
 *      crystal           "known by the OS for this CPU family". We fill
 *                       in 24 MHz (Skylake / Kaby Lake / Ice Lake) or
 *                       19.2 MHz (Atom Goldmont) when CPUID 1's
 *                       family/model matches. Still exact.
 *
 *   3. HPET counter     The HPET main counter is free-running at a
 *      measurement      vendor-fixed frequency (typically 10-14 MHz).
 *                       We rdtsc() across a 100 ms window measured by
 *                       HPET — works regardless of who owns IRQ0
 *                       (8254 PIT or HPET LegacyReplacement). Used
 *                       whenever an HPET is present, even when CPUID
 *                       methods fail — accurate to ~0.1 % over 100 ms.
 *
 *   4. PIT channel 2    Last-resort legacy fallback. PIT channel 2 is
 *      measurement      independent of IRQ0 routing (it gates via port
 *                       0x61) so we can program and read it even when
 *                       HPET owns IRQ0. Brittle on real HW that ships
 *                       without an 8254 (mostly post-2018 server boards)
 *                       but covers ancient hardware.
 *
 *   5. Hardcoded 2 GHz  Absolute panic fallback so that downstream code
 *                       that divides by tsc_freq_khz doesn't blow up.
 *                       Logged at WARNING.
 */

// TSC frequency (cycles per millisecond). Must be calibrated before timers are used.
static volatile uint64_t tsc_freq_khz = 0;
static volatile int calibrated = 0;

/* Known crystal frequencies for Intel CPUs that publish CPUID.15h with
 * ECX (crystal Hz) = 0. Family/model/stepping from CPUID.01h EAX. */
static uint32_t known_intel_crystal_hz(void) {
    if (g_cpu_caps.max_basic_leaf < 1) return 0;
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    uint32_t family = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
    uint32_t model  = ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);

    if (family != 0x06) return 0;  /* only Intel Core/Atom family 6 documented */

    switch (model) {
        /* Goldmont, Goldmont Plus, Tremont — Atom. */
        case 0x5C: case 0x5F: case 0x7A: case 0x86:
            return 19200000u;          /* 19.2 MHz */
        /* Skylake, Kaby Lake, Comet Lake, Ice Lake, Tiger Lake, ... */
        case 0x4E: case 0x5E:                /* Skylake client */
        case 0x55:                            /* Skylake server (Xeon SP) */
        case 0x66:                            /* Cannon Lake */
        case 0x7D: case 0x7E:                /* Ice Lake client */
        case 0x6A: case 0x6C:                /* Ice Lake server */
        case 0x8C: case 0x8D:                /* Tiger Lake */
        case 0x8E: case 0x9E:                /* Kaby/Coffee Lake */
        case 0xA5: case 0xA6:                /* Comet Lake */
            return 24000000u;          /* 24 MHz */
        default:
            return 0;
    }
}

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

    // ECX = 0 → crystal not enumerated. Use family/model lookup.
    if (crystal_hz == 0) {
        crystal_hz = known_intel_crystal_hz();
        if (crystal_hz == 0) return 0;
    }

    // TSC frequency = crystal_hz * numerator / denominator
    uint64_t tsc_hz = (crystal_hz * (uint64_t)ebx) / (uint64_t)eax;
    return tsc_hz / 1000;  // Convert Hz to kHz
}

/* HPET-counter-based TSC measurement.
 *
 * Reads rdtsc twice across a 100 ms window measured by the HPET main
 * counter. The HPET counter is free-running at a vendor-fixed
 * frequency (period reported in fs/tick by the HPET capability
 * register) and is independent of IRQ0 routing — works whether the
 * 8254 PIT or HPET LegacyReplacement is driving the system tick.
 *
 * Accuracy bound: 100 ms × 1 HPET tick / 100 ms = 1/(100ms × HPET_HZ)
 * = 1e-9 for a 10 MHz HPET. Well under 1 % even at the high end. */
static uint64_t hpet_measure_tsc_khz(void) {
    if (!hpet_is_present()) return 0;
    uint64_t fs_per_tick = hpet_period_fs();
    if (fs_per_tick == 0 || fs_per_tick > 0x05F5E100ULL) return 0;

    /* hpet_busy_wait_us busy-loops on the HPET main counter — no
     * IRQ involvement, so this works in any IRQ0 configuration. */
    uint64_t tsc_start = rdtsc();
    hpet_busy_wait_us(100000ULL);   /* 100 ms */
    uint64_t cycles = rdtsc() - tsc_start;
    return cycles / 100ULL;          /* cycles / 100 ms = kHz */
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

    /* Method 1: CPUID.15h (Intel, modern). Direct read, no measurement
     * error. Includes fallback to known-crystal lookup when ECX=0. */
    uint64_t cpuid15_khz = cpuid_get_tsc_freq_khz();
    if (cpuid15_khz > 0) {
        debug_printf("[CPU]   CPUID.15h: %lu kHz\n", cpuid15_khz);
    }

    /* Method 2: CPUID.16h (Intel base freq, informational only — base
     * is not TSC under turbo/SpeedStep). */
    uint64_t cpuid16_khz = cpuid_get_base_freq_khz();
    if (cpuid16_khz > 0) {
        debug_printf("[CPU]   CPUID.16h: %lu kHz (base freq, informational)\n", cpuid16_khz);
    }

    /* Method 3: HPET-counter measurement. Works regardless of who owns
     * IRQ0 (8254 or HPET LegacyReplacement). Preferred over PIT
     * channel-0 measurement because HPET counter is free-running and
     * independent of legacy-tick configuration. */
    uint64_t hpet_khz = hpet_measure_tsc_khz();
    if (hpet_khz > 0) {
        debug_printf("[CPU]   HPET measurement: %lu kHz\n", hpet_khz);
    }

    /* Method 4: PIT channel-0 measurement. Last-resort legacy fallback.
     * Broken when HPET LegacyReplacement is active (channel 0 counter
     * is idle then). We skip it entirely in that case and trust
     * CPUID/HPET. Kept for boards with no HPET at all. */
    uint64_t pit_khz = 0;
    if (!hpet_tick_active()) {
        uint64_t start_tsc = rdtsc();
        pit_delay_busy(100);
        pit_khz = (rdtsc() - start_tsc) / 100;
        debug_printf("[CPU]   PIT calibration: %lu kHz\n", pit_khz);
    }

    /* Selection: prefer the most-trustworthy source in this order. */
    const char *source = NULL;
    if (freq_is_sane(cpuid15_khz)) {
        tsc_freq_khz = cpuid15_khz;
        source = "CPUID.15h";
    } else if (freq_is_sane(hpet_khz)) {
        tsc_freq_khz = hpet_khz;
        source = "HPET measurement";
    } else if (freq_is_sane(pit_khz)) {
        tsc_freq_khz = pit_khz;
        source = "PIT measurement";
    } else {
        /* All four methods failed — almost certainly a virtualisation
         * quirk or a CPU configuration we don't know about. 2 GHz is
         * a sensible-enough panic fallback so kernel timers don't
         * divide by zero. */
        tsc_freq_khz = 2000000ULL;
        source = "2 GHz panic fallback";
        debug_printf("[CPU] WARNING: All calibration methods failed; "
                     "using 2 GHz panic fallback. Timing will be inaccurate.\n");
    }
    debug_printf("[CPU]   Selected source: %s\n", source);

    /* Cross-check: if we have both CPUID.15h and a measurement, warn on
     * large divergence (>5 %). Measurement methods are noise-prone; a
     * huge divergence means one of them is faulty. */
    if (freq_is_sane(cpuid15_khz)) {
        uint64_t check = freq_is_sane(hpet_khz) ? hpet_khz : pit_khz;
        if (freq_is_sane(check)) {
            uint64_t diff = (cpuid15_khz > check) ? (cpuid15_khz - check)
                                                  : (check - cpuid15_khz);
            uint64_t pct = (diff * 100) / check;
            if (pct > 5) {
                debug_printf("[CPU] WARNING: CPUID vs measurement divergence: %lu%% "
                             "(CPUID=%lu, measurement=%lu)\n",
                             pct, cpuid15_khz, check);
            }
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

uint64_t cpu_tsc_to_us(uint64_t cycles) {
    if (tsc_freq_khz == 0) return 0;
    // tsc_freq_khz = cycles per ms, so cycles per us = tsc_freq_khz / 1000
    // us = cycles / (tsc_freq_khz / 1000) = (cycles * 1000) / tsc_freq_khz
    return (cycles * 1000) / tsc_freq_khz;
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
