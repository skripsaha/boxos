#ifndef BOXLIB_ARCH_X86_64_CPU_WAIT_H
#define BOXLIB_ARCH_X86_64_CPU_WAIT_H

#include "box/defs.h"

static inline void umonitor(volatile void* addr) {
    __asm__ volatile(
        ".byte 0xf3, 0x0f, 0xae, 0xf0"
        :
        : "a"(addr)
        : "memory"
    );
}

static inline int umwait(uint32_t state, uint64_t deadline_tsc) {
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

#endif
