#include <box/types.h>


uintptr_t __stack_chk_guard = 0x00000AFF0DEADC0DEULL;

extern void exit_asm(int code) __attribute__((noreturn));

#pragma GCC push_options
#pragma GCC optimize("no-stack-protector")

void __attribute__((noreturn)) __stack_chk_fail(void) {
    exit_asm(139);
    while (1) { __asm__ volatile("hlt"); }
}

#pragma GCC pop_options