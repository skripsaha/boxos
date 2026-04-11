#ifndef CPU_CAPS_PAGE_H
#define CPU_CAPS_PAGE_H

#include "ktypes.h"
#include "boxos_magic.h"
#include "boxos_addresses.h"

#define CPU_CAPS_PAGE_MAGIC 0x43505543  // "CPUC"

// Mapped at 0x7FFFF000 in userspace - avoids conflict with code (0x3000+) and stack
typedef struct __packed {
    uint32_t magic;             // 0x43505543 "CPUC"
    bool has_waitpkg;           // UMONITOR/UMWAIT support
    bool has_invariant_tsc;     // Invariant TSC support
    uint16_t _pad0;             // Alignment padding
    uint64_t tsc_freq_khz;      // Calibrated TSC frequency in kHz (set after boot calibration)
    uint8_t _reserved[4080];    // Reserved for future features
} cpu_caps_page_t;

STATIC_ASSERT(sizeof(cpu_caps_page_t) == 4096, "CPU caps page must be exactly 4096 bytes");

extern uint64_t g_cpu_caps_page_phys;

void cpu_caps_page_init(void);
void cpu_caps_page_set_tsc_freq(uint64_t freq_khz);

#endif // CPU_CAPS_PAGE_H
