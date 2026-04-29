#include <box/types.h>

/*
 * GCC stack protector support for BoxOS userspace.
 *
 * __stack_chk_guard: canary value. Contains a null byte to stop string overflows.
 * __stack_chk_fail: called on canary corruption — exits the process immediately.
 */

uintptr_t __stack_chk_guard = 0x00000AFF0DEADC0DEULL;

extern void exit_asm(int code) __attribute__((noreturn));

#pragma GCC push_options
#pragma GCC optimize("no-stack-protector")

void __attribute__((noreturn)) __stack_chk_fail(void) {
    exit_asm(139);
    while (1) { __asm__ volatile("hlt"); }
}

#pragma GCC pop_options
