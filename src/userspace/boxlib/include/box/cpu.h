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
    bool has_rdrand;            // ring-3 RDRAND usable (random_device)
    bool has_rdseed;            // ring-3 RDSEED usable
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

/* True iff the TSC is invariant (rate fixed across P-/C-states) on every
 * online core. Only then is RDTSC→ns conversion via the calibrated freq
 * meaningful — boxcxx steady_clock gates its TSC path on this and falls
 * back to the ClockBoard uptime when it is false. */
INLINE bool cpu_has_invariant_tsc(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;
    if (caps->magic != CPU_CAPS_MAGIC) return false;
    return caps->has_invariant_tsc;
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

/* True iff RDRAND (CPUID.01H:ECX[30]) / RDSEED (CPUID.07H.0:EBX[18]) are
 * supported on every online core. Gate cpu_rdrand64/cpu_rdseed64 on these —
 * executing the instruction without support raises #UD. Backs the on-chip
 * entropy path of std::random_device. */
INLINE bool cpu_has_rdrand(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;
    if (caps->magic != CPU_CAPS_MAGIC) return false;
    return caps->has_rdrand;
}
INLINE bool cpu_has_rdseed(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;
    if (caps->magic != CPU_CAPS_MAGIC) return false;
    return caps->has_rdseed;
}

/* Draw one 64-bit hardware random word. RDRAND/RDSEED set CF=1 on success;
 * a transient 0 means the on-chip DRBG was momentarily drained, so retry a
 * bounded number of times. Returns false if no word materialised. CALLER
 * MUST gate on cpu_has_rdrand()/cpu_has_rdseed() — the raw instruction #UDs
 * on CPUs without the feature. */
INLINE bool cpu_rdrand64(uint64_t* out) {
    for (int i = 0; i < 10; ++i) {
        uint64_t v;
        uint8_t  ok;
        __asm__ volatile("rdrand %0\n\tsetc %1"
                         : "=r"(v), "=qm"(ok)
                         :
                         : "cc");
        if (ok) { *out = v; return true; }
    }
    return false;
}
INLINE bool cpu_rdseed64(uint64_t* out) {
    for (int i = 0; i < 32; ++i) {
        uint64_t v;
        uint8_t  ok;
        __asm__ volatile("rdseed %0\n\tsetc %1"
                         : "=r"(v), "=qm"(ok)
                         :
                         : "cc");
        if (ok) { *out = v; return true; }
        __asm__ volatile("pause");
    }
    return false;
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

// Convert TSC cycles to whole milliseconds — the exact inverse of
// cpu_ms_to_tsc, sharing its 1 GHz fallback. Truncates toward zero; callers
// that must not collapse a sub-ms-but-positive wait to 0 (== "forever" in the
// boxlib blocking-wait functions) floor the result to 1 themselves.
INLINE uint64_t cpu_tsc_to_ms(uint64_t ticks) {
    uint64_t freq_khz = cpu_get_tsc_freq_khz();
    if (freq_khz == 0) {
        freq_khz = 1000000;  // 1 GHz fallback (only if caps page not mapped)
    }
    return ticks / freq_khz;
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
 * feature raises #UD.
 *
 * These were written as raw .byte sequences "for assembler-version
 * portability", and that reason has expired: the binutils shipped with
 * x86_64-elf-gcc 15.2 assembles both mnemonics. Spelling them out is not
 * tidiness -- a hand-encoded ModRM byte names a register that the asm
 * constraints cannot see, and the two disagreed here for as long as this
 * file existed (see umwait below).
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
        "umonitor %[p]"
        :
        : [p] "r"(addr)
        : "memory"
    );
}

INLINE int umwait(uint32_t state, uint64_t deadline_tsc) {
    uint32_t eax = (uint32_t)deadline_tsc;
    uint32_t edx = (uint32_t)(deadline_tsc >> 32);
    uint8_t cf;

    /* ‼ UMWAIT r32 takes the preferred C-state in r32 and the TSC deadline in
     * EDX:EAX. Bits 31:1 of the control are RESERVED, and a set bit is #GP(0)
     * (SDM Vol 2B, UMWAIT). So the control register must not be one of the two
     * the deadline already occupies -- and until 2026-08-23 it WAS: the raw
     * encoding here read 0xf0, which is `umwait %eax`, so the control word was
     * the deadline's own low half and #GP fired whenever any of those bits was
     * set, which is essentially always.
     *
     * The mnemonic makes that unrepeatable: GCC picks the register, and it
     * cannot pick %eax or %edx because the constraints below already bind
     * them. Measured -- it emits `umwait %edi`.
     *
     * QEMU does not raise on the reserved bits, so the STRICT matrix passed
     * with -cpu max (WAITPKG present) for as long as this existed. Bochs does
     * raise, and so does silicon: found by booting the arrow_lake model, where
     * the shell and PID 2 both died of a user-mode #GP inside
     * touch_wait_umwait. The same image boots clean on tigerlake, whose model
     * reports waitpkg=0.
     */
    __asm__ volatile(
        "umwait %[st]\n\t"
        "setc %[cf]"
        : [cf] "=r"(cf), "+d"(edx), "+a"(eax)
        : [st] "r"(state)
        : "cc", "memory"
    );

    return cf;
}

#ifdef __cplusplus
}
#endif

#endif
