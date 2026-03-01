#ifndef BOX_CPU_H
#define BOX_CPU_H

#include "types.h"

#define CPU_CAPS_PAGE_ADDR  0x7FFFF000UL

#define CPU_CAPS_MAGIC  0x43505543

typedef struct PACKED {
    uint32_t magic;
    bool has_waitpkg;
    bool has_invariant_tsc;
    uint8_t _reserved[4090];
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

#endif
