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
    /* Real-HW feature bits published from g_cpu_caps after AP intersect.
     * Userspace gates the corresponding ISA usage on these bytes — no
     * syscall, no inline CPUID, no AP-divergence surprises. */
    bool has_pku;               // CPUID.07H.0:ECX[3]
    bool has_pks;               // CPUID.07H.0:ECX[31]
    bool has_lam;               // CPUID.07H.1:EAX[26]
    bool has_cet;               // CPUID.07H.0:ECX[7] (shadow stack) OR EDX[20] (IBT)
    bool has_tme;               // CPUID.07H.0:ECX[13]
    uint8_t _pad1[3];           // align next field to 8 bytes
    uint8_t _reserved[4072];    // Reserved for future features
} cpu_caps_page_t;

STATIC_ASSERT(sizeof(cpu_caps_page_t) == 4096, "CPU caps page must be exactly 4096 bytes");

extern uint64_t g_cpu_caps_page_phys;

void cpu_caps_page_init(void);
void cpu_caps_page_set_tsc_freq(uint64_t freq_khz);

/* Re-publish post-intersect feature bits into the userspace caps page.
 *
 * MUST be called after cpu_intersect_features_ap() on every AP. The
 * initial cpu_caps_page_init() captures the BSP's values; AP intersect
 * may flip 1 → 0 on a heterogeneous CPU (Alder/Raptor Lake P+E without
 * WAITPKG on E-cores; future Intel hybrid that disables PKU on a
 * core class). Without this refresh, userspace reads the stale BSP
 * value and #UDs on an E-core. */
void cpu_caps_page_refresh_features(void);

#endif // CPU_CAPS_PAGE_H
