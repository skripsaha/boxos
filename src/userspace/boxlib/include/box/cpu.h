#ifndef BOX_CPU_H
#define BOX_CPU_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"

#define CPU_CAPS_MAGIC  0x43505543

typedef struct PACKED {
    uint32_t magic;
    bool has_waitpkg;
    bool has_invariant_tsc;
    uint16_t _pad0;
    uint64_t tsc_freq_khz;
    bool has_pku;
    bool has_pks;
    bool has_lam;
    bool has_cet;
    bool has_tme;
    bool has_fsgsbase;
    bool has_rdrand;
    bool has_rdseed;
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

INLINE bool cpu_has_invariant_tsc(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;
    if (caps->magic != CPU_CAPS_MAGIC) return false;
    return caps->has_invariant_tsc;
}

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
INLINE bool cpu_has_fsgsbase(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;
    if (caps->magic != CPU_CAPS_MAGIC) return false;
    return caps->has_fsgsbase;
}

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

INLINE uint64_t cpu_get_tsc_freq_khz(void) {
    volatile cpu_caps_page_t* caps = CPU_CAPS;

    if (caps->magic != CPU_CAPS_MAGIC) {
        return 0;
    }

    return caps->tsc_freq_khz;
}

INLINE uint64_t cpu_ms_to_tsc(uint64_t ms) {
    uint64_t freq_khz = cpu_get_tsc_freq_khz();
    if (freq_khz == 0) {
        freq_khz = 1000000;
    }
    return ms * freq_khz;
}

INLINE uint64_t cpu_tsc_to_ms(uint64_t ticks) {
    uint64_t freq_khz = cpu_get_tsc_freq_khz();
    if (freq_khz == 0) {
        freq_khz = 1000000;
    }
    return ticks / freq_khz;
}

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

INLINE uint64_t cpu_tsc_to_ns(uint64_t ticks) {
    uint64_t khz = cpu_get_tsc_freq_khz();
    if (khz == 0) return 0;
    return (ticks * 1000000ULL) / khz;
}

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