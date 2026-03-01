#ifndef FPU_H
#define FPU_H

#include "ktypes.h"

void enable_fpu(void);

// Buffer must be 528 bytes (512 + 16) to allow 16-byte alignment.
// fxsave/fxrstor require 16-byte alignment; kmalloc does not guarantee it.
static inline uint8_t* fpu_align(uint8_t* raw) {
    return (uint8_t*)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
}

// Buffer must be 528 bytes (512 + 16).
static inline void fpu_save(uint8_t* state) {
    __asm__ volatile("fxsave (%0)" : : "r"(fpu_align(state)) : "memory");
}

// Buffer must be 528 bytes (512 + 16).
static inline void fpu_restore(const uint8_t* state) {
    __asm__ volatile("fxrstor (%0)" : : "r"(fpu_align((uint8_t*)(uintptr_t)state)) : "memory");
}

// Buffer must be 528 bytes (512 + 16).
void fpu_init_state(uint8_t* state);

#endif // FPU_H
