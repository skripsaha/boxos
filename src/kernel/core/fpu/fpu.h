#ifndef FPU_H
#define FPU_H

#include "ktypes.h"

void enable_fpu(void);

// Runtime alignment helper for FPU state buffers.
// The buffer must be allocated as 528 bytes (512 + 16) to allow alignment.
// fxsave/fxrstor require 16-byte alignment; kmalloc does not guarantee it.
static inline uint8_t* fpu_align(uint8_t* raw) {
    return (uint8_t*)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
}

// Save current FPU/SSE state. Buffer must be 528 bytes (512 + 16).
static inline void fpu_save(uint8_t* state) {
    __asm__ volatile("fxsave (%0)" : : "r"(fpu_align(state)) : "memory");
}

// Restore FPU/SSE state. Buffer must be 528 bytes (512 + 16).
static inline void fpu_restore(const uint8_t* state) {
    __asm__ volatile("fxrstor (%0)" : : "r"(fpu_align((uint8_t*)(uintptr_t)state)) : "memory");
}

// Initialize a clean FPU state buffer (default FCW and MXCSR values).
// Buffer must be 528 bytes (512 + 16).
void fpu_init_state(uint8_t* state);

#endif // FPU_H