#ifndef BOX_CPU_H
#define BOX_CPU_H

#include "types.h"

#define BOX_CPU_CAPS_PAGE_ADDR  0x7FFFF000UL

#define BOX_CPU_CAPS_MAGIC  0x43505543

typedef struct BOX_PACKED {
    uint32_t magic;
    bool has_waitpkg;
    bool has_invariant_tsc;
    uint8_t _reserved[4090];
} box_cpu_caps_page_t;

BOX_STATIC_ASSERT(sizeof(box_cpu_caps_page_t) == 4096, "CPU caps page must be 4096 bytes");

#define BOX_CPU_CAPS ((volatile box_cpu_caps_page_t*)BOX_CPU_CAPS_PAGE_ADDR)

BOX_INLINE bool box_cpu_has_waitpkg(void) {
    volatile box_cpu_caps_page_t* caps = BOX_CPU_CAPS;

    if (caps->magic != BOX_CPU_CAPS_MAGIC) {
        return false;
    }

    return caps->has_waitpkg;
}

#endif
