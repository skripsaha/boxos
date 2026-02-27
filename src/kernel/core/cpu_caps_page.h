#ifndef CPU_CAPS_PAGE_H
#define CPU_CAPS_PAGE_H

#include "ktypes.h"
#include "boxos_magic.h"
#include "boxos_addresses.h"

// CPU Capabilities Page Magic
#define CPU_CAPS_PAGE_MAGIC 0x43505543  // "CPUC"

// CPU Capabilities Page (read-only, mapped at 0x7FFFF000 in userspace)
// Address chosen to avoid conflict with code (0x3000+) and before stack (0x7FFFFFFFE000)
typedef struct __packed {
    uint32_t magic;             // 0x43505543 "CPUC"
    bool has_waitpkg;           // UMONITOR/UMWAIT support
    bool has_invariant_tsc;     // Invariant TSC support
    uint8_t _reserved[4090];    // Reserved for future features
} cpu_caps_page_t;

STATIC_ASSERT(sizeof(cpu_caps_page_t) == 4096, "CPU caps page must be exactly 4096 bytes");

// Global CPU caps page (physical address)
extern uint64_t g_cpu_caps_page_phys;

// Initialize CPU caps page
void cpu_caps_page_init(void);

#endif // CPU_CAPS_PAGE_H
