#ifndef FPU_H
#define FPU_H

#include "ktypes.h"

// Globals set by enable_fpu() at boot time, used by save/restore and asm
extern bool g_use_xsave;           // true if XSAVE/XRSTOR is available
extern uint32_t g_xsave_area_size; // total XSAVE area size (0 = use FXSAVE 512)
extern uint64_t g_xsave_mask;      // XCR0 mask for xsave/xrstor (EDX:EAX)

// Returns the FPU state buffer size needed (includes alignment padding).
// Call only after enable_fpu().
static inline uint32_t fpu_alloc_size(void) {
    uint32_t base = g_use_xsave ? g_xsave_area_size : 512;
    return base + 63; // padding for 64-byte alignment
}

// Align raw buffer to 64-byte boundary (works for both FXSAVE and XSAVE)
static inline uint8_t* fpu_align(uint8_t* raw) {
    return (uint8_t*)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
}

// Save FPU/SSE/AVX state. raw must point to a buffer of fpu_alloc_size() bytes.
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

// Restore FPU/SSE/AVX state. raw must point to a buffer of fpu_alloc_size() bytes.
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

#endif // FPU_H
