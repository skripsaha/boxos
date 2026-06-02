#include "cpu_calibrate.h"
#include "cpu.h"
#include "pit.h"
#include "hpet.h"
#include "pmtimer.h"
#include "klib.h"
#include "atomics.h"  // For rdtsc()
#include "cpuid.h"
#include "cpu_caps_page.h"
#include "hypervisor.h"
#include "pvclock.h"
#include "clockboard.h"   // clockboard_set_tsc_freq_khz (periodic recal)

/* TSC frequency calibration — production order of preference.
 *
 * Picked TOP TO BOTTOM; first source with a sane value wins. Cross-checks
 * happen across sources at the end.
 *
 *   0. pvclock      KVM-paravirtualised (kvmclock) gives an EXACT
 *      (kvmclock)   tsc_to_ns conversion. Inverting that yields the
 *                   TSC kHz without any measurement. Survives live
 *                   migration. Highest trust under KVM.
 *
 *   1. CPUID.40h    Hypervisor TSC_KHZ leaf 0x40000010 EAX. Standard
 *      hypervisor   on KVM, VMware, QEMU TCG (with -cpu that exposes
 *      leaf         it), Hyper-V. Trusted because the hypervisor knows
 *                   its own scaling — no measurement involved.
 *
 *   2. CPUID.15h    Intel canonical TSC / Core Crystal Clock ratio.
 *                   On Skylake-and-later this is the bit-exact rate.
 *                   ECX=0 → fall back to a known-crystal lookup keyed
 *                   on family/model (24 MHz on most Core, 19.2 MHz on
 *                   Atom Goldmont).
 *
 *   3. CPUID.16h    Intel base frequency (in MHz). Not exactly TSC
 *                   under turbo, but close enough for sanity checks
 *                   and as a panic fallback. Promoted to a primary
 *                   source ONLY when nothing else is available.
 *
 *   4. HPET counter Free-running HPET main counter is wall-clock
 *      measurement  monotonic; rdtsc() against a HPET-measured 100 ms
 *                   window yields the TSC rate. ONLY usable when the
 *                   guest's TSC is host-clock-bound — under QEMU TCG
 *                   the TSC is an instruction counter and this method
 *                   returns garbage like the user-reported 71/40 kHz.
 *                   Gated by hv_tsc_is_wallclock().
 *
 *   5. PIT channel 0 Legacy 8254 PIT measurement. Same TCG limitation
 *      measurement   as HPET — gated by hv_tsc_is_wallclock(). Only
 *                    runs when HPET is absent (rare on real HW).
 *
 *   6. CPUID.16h    Promoted to fallback even when not selected as
 *      promoted     a primary — best informational guess of TSC rate.
 *
 *   7. 2 GHz panic  Absolute last resort so timer arithmetic doesn't
 *                   divide by zero. Logged at WARNING. We should never
 *                   hit this on any real HW or VM we target.
 */

// TSC frequency (cycles per millisecond). Must be calibrated before timers are used.
static volatile uint64_t tsc_freq_khz = 0;
static volatile int calibrated = 0;

/* Absolute panic fallback. Chosen so downstream timer arithmetic that
 * divides by tsc_freq_khz cannot divide by zero. Value is arbitrary —
 * if we hit this fallback timing is wrong anyway; a 2 GHz guess is
 * representative of mid-range silicon and yields plausible-looking
 * (but inaccurate) sleep durations rather than instant or hours-long
 * waits. */
#define TSC_PANIC_FALLBACK_KHZ  2000000ULL

/* HPET TSC measurement window — 5 samples × 20 ms = 100 ms total.
 * Median-of-five evens out one spurious IRQ landing in any single
 * sample without paying for a longer window. */
#define HPET_TSC_SAMPLE_COUNT   5
#define HPET_TSC_SAMPLE_MS      20

/* PIT busy-wait window for last-resort calibration (ms). 100 ms gives
 * comfortable 0.1 % relative error against the 8254's 1.193182 MHz
 * crystal. */
#define PIT_TSC_WINDOW_MS       100

/* Tiered sanity bounds.
 *
 * Real-HW silicon ships ~600 MHz (slowest modern Atom) to ~5.5 GHz
 * (top Xeon turbo). VMs and emulators routinely emulate slower CPUs
 * — Bochs's default `ips=50M` config produces a 50 MHz emulated TSC;
 * QEMU TCG `-icount=N` can produce arbitrarily slow rates. We can't
 * apply the real-HW lower bound uniformly without rejecting valid
 * VM measurements and falling into the panic fallback.
 *
 * Strategy: try the strict (real-HW) bound first; if no source
 * passes, fall back to the permissive (VM/emulator) bound and warn.
 * Real hardware always picks via the strict tier; VMs and emulators
 * pick via the permissive tier with a one-line WARN so the user
 * knows the environment isn't bare-metal-grade.
 *
 *   STRICT  ≥ 100 MHz   — never false-rejects real silicon
 *   LOOSE   ≥   1 MHz   — accepts every plausible emulator
 *   ABS     0           — anything non-zero, panic fallback gate
 *   MAX    10 GHz       — fixed; covers any plausible turbo + 2× slack
 *
 * The actual hardcoded numbers are physics-grounded: there is no real
 * x86 CPU below 600 MHz, and no plausible emulator below 1 MHz that
 * could boot a kernel in finite host wallclock. Both bounds are
 * stable forever — they do not need to track silicon evolution. */
#define TSC_SANE_STRICT_MIN_KHZ   100000ULL   /* real HW */
#define TSC_SANE_LOOSE_MIN_KHZ      1000ULL   /* VM / emulator */
#define TSC_SANE_MAX_KHZ        10000000ULL

/* Periodic recalibration interval. 10 seconds is a balance between
 *   - long enough to amortise the 20 ms measurement cost (0.2% CPU)
 *   - short enough to catch P-state drift, VM live-migration scale
 *     change, or invariant-TSC silicon bugs before user-visible time
 *     skew accumulates
 * Linux clocksource watchdog uses 0.5 s; we go longer because BoxOS
 * has no inotify-style propagation to userspace cached timing yet. */
#define TSC_RECAL_INTERVAL_US   (10ULL * 1000ULL * 1000ULL)

/* HPET measurement window used by the periodic path. Shorter than the
 * boot calibration (which is 100 ms) because (a) we're not the only
 * code wanting the CPU and (b) we already have a good initial value
 * to compare against — a 20 ms sample is accurate to ~0.05 % which
 * is below our drift-update threshold. */
#define TSC_RECAL_HPET_MS       20

/* Drift threshold to publish an update. 1% chosen to filter the
 * inevitable measurement jitter (~0.05 % at 20 ms windows + IRQ
 * coincidences) and only update when a real clock-rate change has
 * occurred. */
#define TSC_RECAL_DRIFT_PCT     1

/* Loud-log threshold. A drift above this is logged via kprintf so it
 * appears in every boot log without DEBUG=on — indicates either a
 * pathological host (frequency scaling on a non-invariant CPU) or a
 * real-HW errata we want to know about. */
#define TSC_RECAL_LOUD_DRIFT_PCT  5

/* Pending flag: PIT IRQ sets, idle context consumes (with CAS to
 * prevent a second IRQ from double-firing). Last-recal timestamp is
 * single-writer (idle context only) so a plain store suffices. */
static volatile uint8_t  s_recal_pending  = 0;
static volatile uint64_t s_recal_last_us  = 0;

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
        /* Haswell, Broadwell — including Bochs's default
         * corei7_haswell_4770 emulation. Pre-Skylake, the canonical
         * crystal was 100 MHz BCLK (TSC = 100 MHz × ratio_from_CPUID.15h).
         * Real Haswell silicon doesn't always publish CPUID.15h with
         * useful values; this lookup is the canonical fallback. */
        case 0x3C: case 0x45: case 0x46:    /* Haswell */
        case 0x3F:                          /* Haswell EP/EX */
        case 0x3D: case 0x47:               /* Broadwell client */
        case 0x4F: case 0x56:               /* Broadwell EP/DE */
            return 100000000u;         /* 100 MHz BCLK */
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

/* HPET-counter-based TSC measurement — multi-sample.
 *
 * Reads rdtsc twice across an N-millisecond window measured by the
 * HPET main counter, repeats SAMPLE_COUNT times, returns the median.
 *
 * The single-sample version (one rdtsc pair across a 100 ms window)
 * was vulnerable to one spurious interrupt landing in the window —
 * which under QEMU TCG and the heavily-instrumented BoxOS boot path
 * happens often enough to wobble the calibration result. Median of
 * five 20 ms samples evens that out and still finishes in 100 ms.
 *
 * Note: on default QEMU TCG (no `-icount`) rdtsc returns host CPU
 * cycles and this measurement works correctly. With `-icount`
 * (deterministic emulation) rdtsc is an instruction counter and the
 * measurement returns garbage in the hundreds-of-kHz range — those
 * values fail freq_is_sane() (< 100 MHz) and the selection logic
 * falls through to the next source. The earlier strict hv_tsc_is_
 * wallclock() gate was too aggressive and disabled this method on
 * default TCG where it works perfectly. */
static uint64_t hpet_measure_tsc_khz(void) {
    if (!hpet_is_present())      return 0;

    uint64_t fs_per_tick = hpet_period_fs();
    if (fs_per_tick == 0 || fs_per_tick > 0x05F5E100ULL) return 0;

    uint64_t samples[HPET_TSC_SAMPLE_COUNT];

    for (int i = 0; i < HPET_TSC_SAMPLE_COUNT; i++) {
        /* rdtsc_serialized: lfence;rdtsc forces program-order visibility
         * around the timestamp so the cycle count over the HPET-paced
         * window can't be inflated/shortened by speculative execution. */
        uint64_t tsc_start = rdtsc_serialized();
        hpet_busy_wait_us((uint64_t)HPET_TSC_SAMPLE_MS * 1000ULL);
        uint64_t cycles = rdtsc_serialized() - tsc_start;
        samples[i] = cycles / (uint64_t)HPET_TSC_SAMPLE_MS;
    }

    /* Insertion sort — HPET_TSC_SAMPLE_COUNT is tiny. */
    for (int i = 1; i < HPET_TSC_SAMPLE_COUNT; i++) {
        uint64_t v = samples[i];
        int j = i - 1;
        while (j >= 0 && samples[j] > v) {
            samples[j + 1] = samples[j];
            j--;
        }
        samples[j + 1] = v;
    }
    return samples[HPET_TSC_SAMPLE_COUNT / 2];
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

/* Real-HW grade: passes the strict (100 MHz) lower bound. */
static int freq_is_strict_sane(uint64_t freq_khz) {
    return (freq_khz >= TSC_SANE_STRICT_MIN_KHZ &&
            freq_khz <= TSC_SANE_MAX_KHZ);
}

/* VM/emulator-permissive: passes the loose (1 MHz) lower bound. */
static int freq_is_loose_sane(uint64_t freq_khz) {
    return (freq_khz >= TSC_SANE_LOOSE_MIN_KHZ &&
            freq_khz <= TSC_SANE_MAX_KHZ);
}

/* Legacy alias kept for the periodic recalibration path which only
 * cares about a single bound. Periodic recal must NOT accept loose
 * values on real HW (that would let a one-off measurement glitch
 * skew tsc_freq_khz from 3 GHz to 50 MHz). */
static int freq_is_sane(uint64_t freq_khz) {
    return freq_is_strict_sane(freq_khz);
}

void cpu_calibrate_tsc(void) {
    kprintf("\n");
    kprintf("[CPU] Calibrating TSC frequency... (hypervisor: %s)\n",
            hv_vendor_name());

    /* Method 0: pvclock (kvmclock). Exact — derived algebraically from
     * the (mul, shift) conversion the hypervisor publishes. Highest
     * trust under KVM. */
    uint64_t pvclock_khz = pvclock_is_available() ? pvclock_tsc_khz() : 0;

    /* Method 1a: hypervisor CPUID leaf 0x40000010 EAX (TSC kHz). Direct
     * read from the hypervisor — no measurement, no rdtsc, immune to
     * TCG instruction-counter quirks. Standard on KVM / VMware / QEMU
     * TCG with the right `-cpu`. */
    uint64_t hv_khz = (uint64_t)hv_tsc_khz();

    /* Method 1b: Hyper-V MSR_TSC_FREQUENCY (0x40000022). Hyper-V doesn't
     * expose CPUID leaf 0x40000010 — it publishes the frequency through
     * a dedicated MSR gated on CPUID 0x40000003:EDX[8]. Same trust
     * level as CPUID.40000010h elsewhere. */
    uint64_t hv_msr_khz = hv_hyperv_tsc_khz_from_msr();

    /* Method 2: CPUID.15h (Intel canonical TSC ratio). */
    uint64_t cpuid15_khz = cpuid_get_tsc_freq_khz();

    /* Method 3: CPUID.16h base frequency (not exactly TSC under turbo,
     * but useful for cross-check and as a primary source of last
     * resort when nothing else works). */
    uint64_t cpuid16_khz = cpuid_get_base_freq_khz();

    /* Method 4: HPET-counter measurement. Multi-sample median; rejects
     * itself when no HPET is present. */
    uint64_t hpet_khz = hpet_measure_tsc_khz();

    /* Method 4b: ACPI PM Timer measurement. Same algorithm as HPET but
     * against the 3.579545 MHz PM Timer counter — useful when HPET is
     * absent or broken (some pre-2010 server boards, some Bochs
     * configs). Single sample is sufficient: the 24-bit width permits
     * a clean 50 ms window with margin (24-bit wrap is ~14 s). */
    uint64_t pmt_khz = 0;
    if (pmtimer_is_present()) {
        uint64_t tsc_start = rdtsc_serialized();
        pmtimer_busy_wait_us(50000ULL);
        pmt_khz = (rdtsc_serialized() - tsc_start) / 50ULL;
    }

    /* Method 5: PIT channel-0 measurement. Only runs when HPET isn't
     * sourcing IRQ0 (legacy 8254 idle otherwise). */
    uint64_t pit_khz = 0;
    if (!hpet_tick_active()) {
        uint64_t start_tsc = rdtsc_serialized();
        pit_delay_busy(PIT_TSC_WINDOW_MS);
        pit_khz = (rdtsc_serialized() - start_tsc) / PIT_TSC_WINDOW_MS;
    }

    /* Single user-visible summary line per source. Critical for
     * Bochs / niche-HW debugging where the user needs to see which
     * source(s) actually responded. */
    kprintf("[CPU] Sources: pvclock=%lu cpuid40=%lu hv-msr=%lu cpuid15=%lu cpuid16=%lu hpet=%lu pmt=%lu pit=%lu kHz\n",
            (unsigned long)pvclock_khz, (unsigned long)hv_khz,
            (unsigned long)hv_msr_khz, (unsigned long)cpuid15_khz,
            (unsigned long)cpuid16_khz, (unsigned long)hpet_khz,
            (unsigned long)pmt_khz,    (unsigned long)pit_khz);

    /* Two-tier selection.
     *
     * Pass 1 — STRICT (real-HW bound, 100 MHz+):
     *   Top-down by trust. If any source reports a real-HW-grade
     *   frequency, pick the most trusted. This is the only path real
     *   silicon will EVER traverse.
     *
     * Pass 2 — LOOSE (1 MHz+):
     *   If pass 1 found nothing, we're on an emulator (Bochs) or a
     *   VM with throttled CPU. Accept any plausible measurement and
     *   emit a one-line WARN so the user knows the timing isn't
     *   real-HW-grade.
     *
     * Pass 3 — panic fallback (2 GHz hardcoded). Only reached when
     *   NO source produced even a 1 MHz reading — at that point
     *   nothing about timing is going to be correct, and a non-zero
     *   constant keeps division alive. */
    struct { uint64_t khz; const char *name; } candidates[] = {
        { pvclock_khz, "pvclock (kvmclock)"              },
        { hv_khz,      "hypervisor CPUID.40000010h"      },
        { hv_msr_khz,  "Hyper-V MSR_TSC_FREQUENCY"       },
        { cpuid15_khz, "CPUID.15h"                       },
        { hpet_khz,    "HPET measurement"                },
        { pmt_khz,     "PM Timer (ACPI) measurement"     },
        { pit_khz,     "PIT measurement"                 },
        { cpuid16_khz, "CPUID.16h (base freq fallback)"  },
    };
    const int N = sizeof(candidates) / sizeof(candidates[0]);
    const char *source = NULL;

    /* Pass 1: strict. */
    for (int i = 0; i < N; i++) {
        if (freq_is_strict_sane(candidates[i].khz)) {
            tsc_freq_khz = candidates[i].khz;
            source       = candidates[i].name;
            break;
        }
    }
    /* Pass 2: loose (only if strict pass found nothing). */
    if (!source) {
        for (int i = 0; i < N; i++) {
            if (freq_is_loose_sane(candidates[i].khz)) {
                tsc_freq_khz = candidates[i].khz;
                source       = candidates[i].name;
                kprintf("[CPU] WARNING: No real-HW-grade source (>=100 MHz); "
                        "accepting permissive %s = %lu kHz. Likely VM/emulator.\n",
                        source, (unsigned long)tsc_freq_khz);
                break;
            }
        }
    }
    /* Pass 3: panic fallback. */
    if (!source) {
        tsc_freq_khz = TSC_PANIC_FALLBACK_KHZ;
        source       = "panic fallback";
        kprintf("[CPU] WARNING: All calibration methods failed; "
                "using %lu kHz panic fallback. Timing will be inaccurate.\n",
                (unsigned long)TSC_PANIC_FALLBACK_KHZ);
    }
    /* User-visible kprintf so the selected source appears in every
     * boot log (debug_printf gates on DEBUG=on). Critical for
     * diagnosing why Bochs reports 2 GHz: this line tells you whether
     * it's a real CPUID.15h read or the panic fallback. */
    kprintf("[CPU] TSC source: %s — %lu kHz (%lu.%03lu GHz)\n",
            source, tsc_freq_khz,
            tsc_freq_khz / 1000000ULL,
            (tsc_freq_khz / 1000ULL) % 1000ULL);

    /* Cross-check: if two trusted sources disagree by >5 %, warn. The
     * pair we trust is (pvclock OR hypervisor TSC_KHZ OR CPUID.15h)
     * vs. (HPET measurement). Skip when comparing against the TCG-
     * poisoned methods (their value is 0 anyway). */
    uint64_t trusted = pvclock_khz ? pvclock_khz
                       : hv_khz ? hv_khz
                       : cpuid15_khz;
    if (freq_is_sane(trusted) && freq_is_sane(hpet_khz)) {
        uint64_t diff = (trusted > hpet_khz) ? (trusted - hpet_khz)
                                              : (hpet_khz - trusted);
        uint64_t pct = (diff * 100) / hpet_khz;
        if (pct > 5) {
            debug_printf("[CPU] WARNING: trusted-vs-HPET divergence %lu%% "
                         "(trusted=%lu, HPET=%lu) — HPET likely throttled\n",
                         pct, trusted, hpet_khz);
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
        kprintf("[CPU] WARNING: TSC not invariant — timekeeping may drift with P-states.\n");
    }
    if (!g_cpu_caps.has_arat) {
        /* Without ARAT (CPUID.6:EAX[2]) the LAPIC timer can stop in
         * C3+ deep idle. cpu_idle stays at C1 unconditionally — see
         * idle.c — so this is not fatal, but operators should know
         * the CPU can't safely enter deeper idle states. */
        kprintf("[CPU] WARNING: ARAT not advertised — LAPIC timer may stop in deep idle. "
                "cpu_idle pinned to C1 (HLT/MWAIT C1) for safety.\n");
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

/* ----------------------------------------------------------------
 * Periodic TSC recalibration
 * ---------------------------------------------------------------- */

/* Read a fresh TSC frequency from the most-trusted available source.
 * Returns 0 if no source is usable (no pvclock, no HPET, or
 * measurement returned garbage). Cheap-O(1) when pvclock is the
 * source; ~20 ms blocking when HPET is the source. Must NOT run in
 * IRQ context — the HPET busy-wait would delay other interrupts. */
static uint64_t cpu_tsc_recal_sample(const char **out_src)
{
    /* Prefer pvclock when available — exact, no measurement,
     * survives live migration. */
    if (pvclock_is_available()) {
        uint64_t khz = pvclock_tsc_khz();
        if (freq_is_sane(khz)) {
            if (out_src) *out_src = "pvclock";
            return khz;
        }
    }

    /* Fall back to HPET-counter measurement. */
    if (hpet_is_present()) {
        uint64_t tsc_start = rdtsc_serialized();
        hpet_busy_wait_us((uint64_t)TSC_RECAL_HPET_MS * 1000ULL);
        uint64_t cycles = rdtsc_serialized() - tsc_start;
        uint64_t khz = cycles / (uint64_t)TSC_RECAL_HPET_MS;
        if (freq_is_sane(khz)) {
            if (out_src) *out_src = "HPET";
            return khz;
        }
    }

    return 0;
}

bool cpu_tsc_recal_now(void)
{
    if (!calibrated) return false;

    /* INVARIANT TSC guarantees a constant frequency — the architectural
     * promise is that the counter advances at the nominal rate
     * regardless of P-state / C-state / thermal throttling. Recal would
     * only "catch" measurement noise from the HPET sample window, which
     * we'd then publish as a phantom freq change. Don't. */
    if (g_cpu_caps.has_invariant_tsc) return true;

    /* QEMU TCG advertises NO INVARIANT_TSC under default qemu64 but its
     * TSC is in fact a constant emulated counter (TCG fakes a 1 GHz
     * clock). HPET-based measurement under TCG is busy-wait simulation
     * that easily produces > 1 % apparent drift from sample to sample
     * (TCG instruction-step timing isn't synchronized to wall time).
     * Publishing those samples back into tsc_freq_khz makes per-process
     * timing wobble on each recal cycle — observed as a stress_matrix
     * flake when bench prints a "TSC freq:" outside the 1 GHz band on
     * the second invocation. Skip — TCG's TSC is stable by emulator
     * construction, no recal needed. */
    if (hv_vendor() == HV_VENDOR_TCG) return true;

    const char *src = NULL;
    uint64_t new_khz = cpu_tsc_recal_sample(&src);
    if (new_khz == 0) return false;

    uint64_t cur_khz = tsc_freq_khz;
    if (cur_khz == 0) return false;

    uint64_t diff = (new_khz > cur_khz) ? (new_khz - cur_khz)
                                         : (cur_khz - new_khz);
    uint64_t pct  = (diff * 100ULL) / cur_khz;

    if (pct >= TSC_RECAL_DRIFT_PCT) {
        /* Atomic publish so userspace readers of cpu_caps_page see
         * either old or new value, never torn. Single-writer
         * (this function, called only from idle context) means we
         * don't need cmpxchg — a release store is sufficient. */
        __atomic_store_n(&tsc_freq_khz, new_khz, __ATOMIC_RELEASE);
        cpu_caps_page_set_tsc_freq(new_khz);
        clockboard_set_tsc_freq_khz(new_khz);

        if (pct >= TSC_RECAL_LOUD_DRIFT_PCT) {
            kprintf("[CPU] TSC recal: %lu → %lu kHz (drift %lu%%, src=%s)\n",
                    (unsigned long)cur_khz, (unsigned long)new_khz,
                    (unsigned long)pct, src);
        } else {
            debug_printf("[CPU] TSC recal: %lu → %lu kHz (drift %lu%%, src=%s)\n",
                         (unsigned long)cur_khz, (unsigned long)new_khz,
                         (unsigned long)pct, src);
        }
    }
    return true;
}

void cpu_tsc_recal_tick(uint64_t now_us)
{
    if (!calibrated) return;

    /* First call seeds the timestamp without firing — we don't want
     * an immediate recalibration on the very first PIT tick post-
     * calibration since calibration just ran. */
    uint64_t last = __atomic_load_n(&s_recal_last_us, __ATOMIC_RELAXED);
    if (last == 0) {
        __atomic_store_n(&s_recal_last_us, now_us, __ATOMIC_RELAXED);
        return;
    }

    if (now_us - last >= TSC_RECAL_INTERVAL_US) {
        /* Set pending. Idle context will consume. Last-update
         * timestamp is updated by the consumer to keep us-pending
         * and us-completed serialised through one writer. */
        __atomic_store_n(&s_recal_pending, 1u, __ATOMIC_RELEASE);
    }
}

bool cpu_tsc_recal_if_pending(void)
{
    /* CAS-style claim — only one path actually runs the (long) recal
     * even if PIT IRQ fires a second time during the 20 ms window. */
    uint8_t expected = 1u;
    if (!__atomic_compare_exchange_n(&s_recal_pending, &expected, 0u,
                                      false,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return false;
    }

    bool performed = cpu_tsc_recal_now();

    /* Re-stamp regardless of result so a wedged HPET (returning 0)
     * doesn't burn the CPU re-triggering every PIT tick. The next
     * fire happens TSC_RECAL_INTERVAL_US from now. */
    __atomic_store_n(&s_recal_last_us, pit_get_uptime_us(), __ATOMIC_RELAXED);
    return performed;
}
