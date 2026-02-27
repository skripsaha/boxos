#include "ktypes.h"
#include "fpu.h"

void enable_fpu(void) {
    uint64_t cr0, cr4;

    // CR0: разрешаем FPU
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2); // EM = 0 (разрешить FPU)
    cr0 |=  (1 << 1); // MP = 1
    asm volatile("mov %0, %%cr0" :: "r"(cr0));

    // CR4: разрешить SSE и FXSR
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);  // OSFXSR — включить поддержку fxsave/fxrstor
    cr4 |= (1 << 10); // OSXMMEXCPT — разрешить SSE-исключения
    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    // Сбросить FPU
    asm volatile("fninit");
}
