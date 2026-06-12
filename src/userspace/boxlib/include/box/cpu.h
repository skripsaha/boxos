#ifndef BOX_CPU_H
#define BOX_CPU_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"

#define CPU_CAPS_MAGIC  0x43505543

/* MUST stay byte-identical to the kernel-side cpu_caps_page_t in
 * src/kernel/core/cpu_caps/cpu_caps_page.h. The kernel allocates the
 * page, fills it from g_cpu_caps, and maps it read-only at
 * CABIN_CPU_CAPS_ADDR in every cabin. Adding a field here without
 * updating the kernel side (or vice-versa) silently mis-aligns reads. */
typedef struct PACKED {
    uint32_t magic;
    bool has_waitpkg;
    bool has_invariant_tsc;
    uint16_t _pad0;
    uint64_t tsc_freq_khz;      // Calibrated TSC frequency in kHz
    /* Real-HW feature bits published from g_cpu_caps after AP intersect.
     * Userspace gates the corresponding ISA usage on these bytes — no
     * syscall, no inline CPUID, no AP-divergence surprises. */
    bool has_pku;
    bool has_pks;
    bool has_lam;
    bool has_cet;               // shadow stack OR IBT
    bool has_tme;
    bool has_fsgsbase;          // ring-3 WRFSBASE/RDFSBASE usable (TLS)
    uint8_t _pad1[2];           // align next field to 8 bytes
    uint8_t _reserved[4072];
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

/* True iff CPUID.07H.0:ECX[3] (PKU) is supported AND has been preserved
 * through every AP intersect (so RDPKRU/WRPKRU is safe on any cabin
 * scheduling decision). Returns false on systems without PKU and on
 * cabins that started before the kernel published the caps page. */
INLINE bool cpu_has_pku(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;
    if (caps->magic != CPU_CAPS_MAGIC) return false;
    return caps->has_pku;
}
INLINE bool cpu_has_pks(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;
    if (caps->magic != CPU_CAPS_MAGIC) return false;
    return caps->has_pks;
}
INLINE bool cpu_has_lam(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;
    if (caps->magic != CPU_CAPS_MAGIC) return false;
    return caps->has_lam;
}
INLINE bool cpu_has_cet(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;
    if (caps->magic != CPU_CAPS_MAGIC) return false;
    return caps->has_cet;
}
INLINE bool cpu_has_tme(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;
    if (caps->magic != CPU_CAPS_MAGIC) return false;
    return caps->has_tme;
}
/* True iff CR4.FSGSBASE=1 on every online core — boxcxx TLS init issues
 * WRFSBASE directly when set; otherwise it falls back to the kernel
 * SYSTEM_OP_TLS_FSBASE op (+yield to materialize the base). */
INLINE bool cpu_has_fsgsbase(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;
    if (caps->magic != CPU_CAPS_MAGIC) return false;
    return caps->has_fsgsbase;
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

/* ---------------------------------------------------------------------------
 * WAITPKG user-mode wait primitives (Intel SDM Vol 2A UMONITOR/UMWAIT).
 * Gate every use on cpu_has_waitpkg() — executing these without the
 * feature raises #UD. Raw opcodes for assembler-version portability.
 *
 * umonitor() arms a hardware monitor on the cacheline of `addr` (line
 * size from CPUID.05H). Any subsequent store to that line wakes a
 * following umwait() immediately. Only WB memory triggers reliably.
 *
 * umwait(state, deadline_tsc): state 0 ⇒ C0.2 (deeper savings, slower
 * wake), 1 ⇒ C0.1. Returns CF: 1 = OS time limit (IA32_UMWAIT_CONTROL)
 * expired, 0 = monitored write / interrupt / TSC deadline. Callers must
 * re-check their predicate after wake — spurious wakes are expected.
 * --------------------------------------------------------------------------- */
INLINE void umonitor(volatile void* addr) {
    __asm__ volatile(
        ".byte 0xf3, 0x0f, 0xae, 0xf0"
        :
        : "a"(addr)
        : "memory"
    );
}

INLINE int umwait(uint32_t state, uint64_t deadline_tsc) {
    uint32_t eax = (uint32_t)deadline_tsc;
    uint32_t edx = (uint32_t)(deadline_tsc >> 32);
    uint8_t cf;

    __asm__ volatile(
        ".byte 0xf2, 0x0f, 0xae, 0xf0\n"
        "setc %[cf]"
        : [cf] "=r"(cf), "+d"(edx), "+a"(eax)
        : "c"(state)
        : "cc", "memory"
    );

    return cf;
}

#ifdef __cplusplus
}
#endif

#endif
