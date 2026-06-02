#ifndef BOX_CPU_H
#define BOX_CPU_H

#include "box/types.h"

#define CPU_CAPS_MAGIC  0x43505543

typedef struct PACKED {
    uint32_t magic;
    bool has_waitpkg;
    bool has_invariant_tsc;
    uint16_t _pad0;
    uint64_t tsc_freq_khz;      // Calibrated TSC frequency in kHz
    uint8_t _reserved[4080];
} cpu_caps_page_t;

STATIC_ASSERT(sizeof(cpu_caps_page_t) == 4096, "CPU caps page must be 4096 bytes");

#define CPU_CAPS ((volatile cpu_caps_page_t*)CABIN_CPU_CAPS_ADDR)

INLINE bool cpu_has_waitpkg(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;

    if (caps->magic != CPU_CAPS_MAGIC) {
        return false;
    }

    return caps->has_waitpkg;
}

// Get calibrated TSC frequency in kHz. Returns 0 if not available.
INLINE uint64_t cpu_get_tsc_freq_khz(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;

    if (caps->magic != CPU_CAPS_MAGIC) {
        return 0;
    }

    return caps->tsc_freq_khz;
}

// Convert milliseconds to TSC cycles using calibrated frequency.
// Falls back to 1 GHz estimate if calibration data unavailable.
INLINE uint64_t cpu_ms_to_tsc(uint64_t ms) {
    uint64_t freq_khz = cpu_get_tsc_freq_khz();
    if (freq_khz == 0) {
        freq_khz = 1000000;  // 1 GHz fallback (only if caps page not mapped)
    }
    return ms * freq_khz;
}

/* ---------------------------------------------------------------------------
 * High-precision timing primitives (for benchmarking, not wall-clock).
 *
 * RDTSC reads the CPU Time Stamp Counter — a 64-bit, monotonically-rising
 * cycle counter. On modern Intel/AMD with INVARIANT_TSC it advances at a
 * fixed nominal frequency regardless of P-state / C-state, so converting
 * ticks to nanoseconds via the calibrated freq is accurate.
 *
 * Why not RTC? CMOS RTC is 1-second granularity. PIT-derived uptime is
 * ~2 ms (500 Hz). RDTSC is ~25 cycles per call (sub-ns), the only sane
 * primitive for measuring syscalls (~µs) and short ops (~ns).
 *
 * cpu_rdtsc()      — LFENCE + RDTSC: serialises prior loads, prevents the
 *                    CPU from issuing the rdtsc before pending memory ops
 *                    retire. Use as the START of a measurement window.
 * cpu_rdtsc_end()  — RDTSC + LFENCE: reads the counter, then prevents
 *                    subsequent loads from being reordered before it.
 *                    Use as the END of a measurement window.
 *
 * NB: RDTSCP would be slightly stronger but is opt-in via CPUID and
 * QEMU's default `-cpu qemu64` does NOT expose it (executing it raises
 * #UD). The lfence-bracketed plain rdtsc pair works on every x86_64.
 *
 * Both have a "memory" clobber so the C compiler can't reorder loads or
 * stores across the inline-asm barrier. Together they bracket a code
 * region with the cleanest available start/end semantics in user mode.
 * --------------------------------------------------------------------------- */
INLINE uint64_t cpu_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence\n\trdtsc"
                     : "=a"(lo), "=d"(hi)
                     :
                     : "memory");
    return ((uint64_t)hi << 32) | lo;
}

INLINE uint64_t cpu_rdtsc_end(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc\n\tlfence"
                     : "=a"(lo), "=d"(hi)
                     :
                     : "memory");
    return ((uint64_t)hi << 32) | lo;
}

// Convert TSC ticks to nanoseconds using the calibrated TSC frequency.
// Returns 0 if calibration data is unavailable. Multiplication is done
// before division to preserve precision; with khz ≥ 100 MHz and ticks
// up to ~1e10 (≈3 s @ 3 GHz) the intermediate value (ticks * 1e6) fits
// comfortably inside a uint64_t.
INLINE uint64_t cpu_tsc_to_ns(uint64_t ticks) {
    uint64_t khz = cpu_get_tsc_freq_khz();
    if (khz == 0) return 0;
    return (ticks * 1000000ULL) / khz;
}

#endif
