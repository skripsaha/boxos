#include "cpu_calibrate.h"
#include "cpu.h"
#include "pit.h"
#include "hpet.h"
#include "pmtimer.h"
#include "klib.h"
#include "atomics.h"
#include "cpuid.h"
#include "cpu_caps_page.h"
#include "hypervisor.h"
#include "pvclock.h"
#include "clockboard.h"


static volatile uint64_t tsc_freq_khz = 0;
static volatile int calibrated = 0;

#define TSC_PANIC_FALLBACK_KHZ  2000000ULL

#define HPET_TSC_SAMPLE_COUNT   5
#define HPET_TSC_SAMPLE_MS      20

#define PIT_TSC_WINDOW_MS       100

#define TSC_SANE_STRICT_MIN_KHZ   100000ULL
#define TSC_SANE_LOOSE_MIN_KHZ      1000ULL
#define TSC_SANE_MAX_KHZ        10000000ULL

#define TSC_RECAL_INTERVAL_US   (10ULL * 1000ULL * 1000ULL)

#define TSC_RECAL_HPET_MS       20

#define TSC_RECAL_DRIFT_PCT     1

#define TSC_RECAL_LOUD_DRIFT_PCT  5

static volatile uint8_t  s_recal_pending  = 0;
static volatile uint64_t s_recal_last_us  = 0;

static uint32_t known_intel_crystal_hz(void) {
    if (g_cpu_caps.max_basic_leaf < 1) return 0;
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    uint32_t family = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
    uint32_t model  = ((eax >> 4) & 0xF) | (((eax >> 16) & 0xF) << 4);

    if (family != 0x06) return 0;

    switch (model) {
        case 0x5C: case 0x5F: case 0x7A: case 0x86:
            return 19200000u;
        case 0x4E: case 0x5E:
        case 0x55:
        case 0x66:
        case 0x7D: case 0x7E:
        case 0x6A: case 0x6C:
        case 0x8C: case 0x8D:
        case 0x8E: case 0x9E:
        case 0xA5: case 0xA6:
            return 24000000u;
        case 0x3C: case 0x45: case 0x46:
        case 0x3F:
        case 0x3D: case 0x47:
        case 0x4F: case 0x56:
            return 100000000u;
        default:
            return 0;
    }
}

uint64_t cpu_tsc_architectural_khz(void) {
    if (g_cpu_caps.max_basic_leaf < 0x15) {
        return 0;
    }

    uint32_t eax, ebx, ecx, edx;
    cpuid(0x15, &eax, &ebx, &ecx, &edx);

    if (eax == 0 || ebx == 0 || ecx == 0) {
        return 0;
    }

    return (((uint64_t)ecx * (uint64_t)ebx) / (uint64_t)eax) / 1000;
}

static uint64_t cpuid_get_tsc_freq_khz(void) {
    uint64_t exact_khz = cpu_tsc_architectural_khz();
    if (exact_khz != 0) {
        return exact_khz;
    }

    if (g_cpu_caps.max_basic_leaf < 0x15) {
        return 0;
    }

    uint32_t eax, ebx, ecx, edx;
    cpuid(0x15, &eax, &ebx, &ecx, &edx);

    if (eax == 0 || ebx == 0) {
        return 0;
    }

    uint64_t crystal_hz = known_intel_crystal_hz();
    if (crystal_hz == 0) return 0;

    uint64_t tsc_hz = (crystal_hz * (uint64_t)ebx) / (uint64_t)eax;
    return tsc_hz / 1000;
}

static uint64_t hpet_measure_tsc_khz(void) {
    if (!hpet_is_present())      return 0;

    uint64_t fs_per_tick = hpet_period_fs();
    if (fs_per_tick == 0 || fs_per_tick > 0x05F5E100ULL) return 0;

    uint64_t samples[HPET_TSC_SAMPLE_COUNT];

    for (int i = 0; i < HPET_TSC_SAMPLE_COUNT; i++) {
        uint64_t tsc_start = rdtsc_serialized();
        hpet_busy_wait_us((uint64_t)HPET_TSC_SAMPLE_MS * 1000ULL);
        uint64_t cycles = rdtsc_serialized() - tsc_start;
        samples[i] = cycles / (uint64_t)HPET_TSC_SAMPLE_MS;
    }

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

static uint64_t cpuid_get_base_freq_khz(void) {
    if (g_cpu_caps.max_basic_leaf < 0x16) {
        return 0;
    }

    uint32_t eax, ebx, ecx, edx;
    cpuid(0x16, &eax, &ebx, &ecx, &edx);

    if (eax == 0) {
        return 0;
    }

    return (uint64_t)eax * 1000;
}

static int freq_is_strict_sane(uint64_t freq_khz) {
    return (freq_khz >= TSC_SANE_STRICT_MIN_KHZ &&
            freq_khz <= TSC_SANE_MAX_KHZ);
}

static int freq_is_loose_sane(uint64_t freq_khz) {
    return (freq_khz >= TSC_SANE_LOOSE_MIN_KHZ &&
            freq_khz <= TSC_SANE_MAX_KHZ);
}

static int freq_is_sane(uint64_t freq_khz) {
    return freq_is_strict_sane(freq_khz);
}

void cpu_calibrate_tsc(void) {
    kprintf("\n");
    kprintf("[CPU] Calibrating TSC frequency... (hypervisor: %s)\n",
            hv_vendor_name());

    uint64_t pvclock_khz = pvclock_is_available() ? pvclock_tsc_khz() : 0;

    uint64_t hv_khz = (uint64_t)hv_tsc_khz();

    uint64_t hv_msr_khz = hv_hyperv_tsc_khz_from_msr();

    uint64_t cpuid15_khz = cpuid_get_tsc_freq_khz();

    uint64_t cpuid16_khz = cpuid_get_base_freq_khz();

    uint64_t hpet_khz = hpet_measure_tsc_khz();

    uint64_t pmt_khz = 0;
    if (pmtimer_is_present()) {
        uint64_t samples[HPET_TSC_SAMPLE_COUNT];
        bool measured = true;
        for (int i = 0; i < HPET_TSC_SAMPLE_COUNT; i++) {
            uint64_t tsc_start = rdtsc_serialized();
            if (!pmtimer_busy_wait_us((uint64_t)HPET_TSC_SAMPLE_MS * 1000ULL)) {
                kprintf("[CPU] PM Timer did not keep time during calibration "
                        "— dropping it as a source\n");
                measured = false;
                break;
            }
            uint64_t cycles = rdtsc_serialized() - tsc_start;
            samples[i] = cycles / (uint64_t)HPET_TSC_SAMPLE_MS;
        }
        if (measured) {
            for (int i = 1; i < HPET_TSC_SAMPLE_COUNT; i++) {
                uint64_t v = samples[i];
                int j = i - 1;
                while (j >= 0 && samples[j] > v) {
                    samples[j + 1] = samples[j];
                    j--;
                }
                samples[j + 1] = v;
            }
            pmt_khz = samples[HPET_TSC_SAMPLE_COUNT / 2];
        }
    }

    uint64_t pit_khz = 0;
    if (!hpet_tick_active()) {
        uint64_t start_tsc = rdtsc_serialized();
        pit_delay_busy(PIT_TSC_WINDOW_MS);
        pit_khz = (rdtsc_serialized() - start_tsc) / PIT_TSC_WINDOW_MS;
    }

    kprintf("[CPU] Sources: pvclock=%lu cpuid40=%lu hv-msr=%lu cpuid15=%lu cpuid16=%lu hpet=%lu pmt=%lu pit=%lu kHz\n",
            (unsigned long)pvclock_khz, (unsigned long)hv_khz,
            (unsigned long)hv_msr_khz, (unsigned long)cpuid15_khz,
            (unsigned long)cpuid16_khz, (unsigned long)hpet_khz,
            (unsigned long)pmt_khz,    (unsigned long)pit_khz);

    const bool tcg = (hv_vendor() == HV_VENDOR_TCG);
    typedef struct { uint64_t khz; const char *name; } tsc_cand_t;
    tsc_cand_t candidates_real[] = {
        { pvclock_khz, "pvclock (kvmclock)"              },
        { hv_khz,      "hypervisor CPUID.40000010h"      },
        { hv_msr_khz,  "Hyper-V MSR_TSC_FREQUENCY"       },
        { cpuid15_khz, "CPUID.15h"                       },
        { hpet_khz,    "HPET measurement"                },
        { pmt_khz,     "PM Timer (ACPI) measurement"     },
        { pit_khz,     "PIT measurement"                 },
        { cpuid16_khz, "CPUID.16h (base freq fallback)"  },
    };
    tsc_cand_t candidates_tcg[] = {
        { pvclock_khz, "pvclock (kvmclock)"              },
        { hv_khz,      "hypervisor CPUID.40000010h"      },
        { hv_msr_khz,  "Hyper-V MSR_TSC_FREQUENCY"       },
        { cpuid15_khz, "CPUID.15h"                       },
        { pmt_khz,     "PM Timer (ACPI) measurement"     },
        { hpet_khz,    "HPET measurement"                },
        { pit_khz,     "PIT measurement"                 },
        { cpuid16_khz, "CPUID.16h (base freq fallback)"  },
    };
    tsc_cand_t *candidates = tcg ? candidates_tcg : candidates_real;
    const int N = sizeof(candidates_real) / sizeof(candidates_real[0]);
    const char *source = NULL;

    for (int i = 0; i < N; i++) {
        if (freq_is_strict_sane(candidates[i].khz)) {
            tsc_freq_khz = candidates[i].khz;
            source       = candidates[i].name;
            break;
        }
    }
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
    if (!source) {
        tsc_freq_khz = TSC_PANIC_FALLBACK_KHZ;
        source       = "panic fallback";
        kprintf("[CPU] WARNING: All calibration methods failed; "
                "using %lu kHz panic fallback. Timing will be inaccurate.\n",
                (unsigned long)TSC_PANIC_FALLBACK_KHZ);
    }
    kprintf("[CPU] TSC source: %s — %lu kHz (%lu.%03lu GHz)\n",
            source, tsc_freq_khz,
            tsc_freq_khz / 1000000ULL,
            (tsc_freq_khz / 1000ULL) % 1000ULL);

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
        kprintf("[CPU] WARNING: ARAT not advertised — LAPIC timer may stop in deep idle. "
                "cpu_idle pinned to C1 (HLT/MWAIT C1) for safety.\n");
    }
    kprintf("\n");
}

uint64_t cpu_ms_to_tsc(uint64_t ms) {
    if (!calibrated) {
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

uint64_t cpu_us_to_tsc(uint64_t us) {
    uint64_t khz = __atomic_load_n(&tsc_freq_khz, __ATOMIC_RELAXED);
    if (khz == 0) return 0;

    uint64_t cycles = (us * khz + 999ULL) / 1000ULL;
    return cycles ? cycles : 1ULL;
}

uint64_t cpu_tsc_to_us(uint64_t cycles) {
    if (tsc_freq_khz == 0) return 0;
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


static uint64_t cpu_tsc_recal_sample(const char **out_src)
{
    if (pvclock_is_available()) {
        uint64_t khz = pvclock_tsc_khz();
        if (freq_is_sane(khz)) {
            if (out_src) *out_src = "pvclock";
            return khz;
        }
    }

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

    if (g_cpu_caps.has_invariant_tsc) return true;

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

    uint64_t last = __atomic_load_n(&s_recal_last_us, __ATOMIC_RELAXED);
    if (last == 0) {
        __atomic_store_n(&s_recal_last_us, now_us, __ATOMIC_RELAXED);
        return;
    }

    if (now_us - last >= TSC_RECAL_INTERVAL_US) {
        __atomic_store_n(&s_recal_pending, 1u, __ATOMIC_RELEASE);
    }
}

bool cpu_tsc_recal_if_pending(void)
{
    uint8_t expected = 1u;
    if (!__atomic_compare_exchange_n(&s_recal_pending, &expected, 0u,
                                      false,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return false;
    }

    bool performed = cpu_tsc_recal_now();

    __atomic_store_n(&s_recal_last_us, pit_get_uptime_us(), __ATOMIC_RELAXED);
    return performed;
}