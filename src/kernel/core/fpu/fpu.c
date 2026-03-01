#include "ktypes.h"
#include "fpu.h"
#include "klib.h"

void enable_fpu(void) {
    uint64_t cr0, cr4;

    // CR0: enable FPU
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2); // EM = 0 (enable FPU emulation off)
    cr0 |=  (1 << 1); // MP = 1
    asm volatile("mov %0, %%cr0" :: "r"(cr0));

    // CR4: enable SSE and FXSR
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);  // OSFXSR — enable fxsave/fxrstor support
    cr4 |= (1 << 10); // OSXMMEXCPT — enable SSE exceptions
    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    // Reset FPU to clean state
    asm volatile("fninit");
}

void fpu_init_state(uint8_t* state) {
    uint8_t* p = fpu_align(state);
    memset(p, 0, 512);
    // FCW (FPU Control Word) at offset 0: default 0x037F
    // All exceptions masked, double precision, round to nearest
    p[0] = 0x7F;
    p[1] = 0x03;
    // MXCSR at offset 24: default 0x1F80
    // All SSE exceptions masked, round to nearest, no flags
    p[24] = 0x80;
    p[25] = 0x1F;
}
