#ifndef BOX_CPU_H
#define BOX_CPU_H

#include "types.h"

#define CPU_CAPS_PAGE_ADDR  0x7FFFF000UL

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

#define CPU_CAPS ((volatile cpu_caps_page_t*)CPU_CAPS_PAGE_ADDR)

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

#endif
