#ifndef FPU_H
#define FPU_H

#include "ktypes.h"

extern bool g_use_xsave;
extern uint32_t g_xsave_area_size;
extern uint64_t g_xsave_mask;

extern volatile uint8_t g_fsgsbase_active;
extern volatile uint8_t g_user_fsbase_used;

static inline uint32_t fpu_alloc_size(void) {
    uint32_t base = g_use_xsave ? g_xsave_area_size : 512;
    return base + 63;
}

static inline uint8_t* fpu_align(uint8_t* raw) {
    return (uint8_t*)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
}

static inline void fpu_save(uint8_t* raw) {
    uint8_t* aligned = fpu_align(raw);
    if (g_use_xsave) {
        uint32_t lo = (uint32_t)g_xsave_mask;
        uint32_t hi = (uint32_t)(g_xsave_mask >> 32);
        __asm__ volatile("xsave (%0)" : : "r"(aligned), "a"(lo), "d"(hi) : "memory");
    } else {
        __asm__ volatile("fxsave (%0)" : : "r"(aligned) : "memory");
    }
}

static inline void fpu_restore(const uint8_t* raw) {
    uint8_t* aligned = fpu_align((uint8_t*)(uintptr_t)raw);
    if (g_use_xsave) {
        uint32_t lo = (uint32_t)g_xsave_mask;
        uint32_t hi = (uint32_t)(g_xsave_mask >> 32);
        __asm__ volatile("xrstor (%0)" : : "r"(aligned), "a"(lo), "d"(hi) : "memory");
    } else {
        __asm__ volatile("fxrstor (%0)" : : "r"(aligned) : "memory");
    }
}

void enable_fpu(void);
void fpu_init_state(uint8_t* raw);

bool fpu_xsave_register_extension(uint64_t xcr0_bit, const char *name);

#endif