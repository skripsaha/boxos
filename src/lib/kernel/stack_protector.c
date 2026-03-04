#include "klib.h"
#include "process.h"
#include "scheduler.h"

uintptr_t __stack_chk_guard = 0x00000AFF0DEADC0DEULL;

#pragma GCC push_options
#pragma GCC optimize("no-stack-protector")

void __attribute__((noreturn)) __stack_chk_fail(void) {
    process_t* proc = process_get_current();

    if (proc && proc->pid != 0) {
        kprintf("\n[STACK PROTECTOR] Stack smashing detected in PID %u (%s)\n",
                proc->pid, proc->tags);
        process_set_state(proc, PROC_CRASHED);
        scheduler_yield();
    }

    kprintf("\n");
    kprintf("====================================================================\n");
    kprintf("KERNEL PANIC: Stack smashing detected\n");
    kprintf("====================================================================\n");

    while (1) {
        asm volatile("cli; hlt");
    }
}

#pragma GCC pop_options
