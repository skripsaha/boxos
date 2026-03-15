#include "amp.h"
#include "per_core.h"
#include "lapic.h"
#include "idt.h"
#include "klib.h"
#include "scheduler.h"
#include "idle.h"
#include "kcore.h"

void ap_entry_c(uint64_t core_index, uint64_t stack_top) {
    // per_core_init_ap sets up:
    //   - Per-core GDT (with per-core TSS descriptor)
    //   - Per-core TSS (rsp0, IST stacks with guard pages)
    //   - FPU/SSE/AVX
    //   - SYSCALL MSRs (EFER.SCE+NXE, STAR, LSTAR, SFMASK)
    //   - PerCpuData + MSR_KERNEL_GS_BASE (for swapgs)
    //   - LAPIC enable + LAPIC timer (100Hz periodic)
    per_core_init_ap((uint8_t)core_index, stack_top);

    // IDT is shared across all cores (single static table)
    idt_load();

    // Per-core scheduler + idle process.
    // MUST be before sti — LAPIC timer is already counting and will fire
    // as soon as interrupts are enabled, calling schedule().
    scheduler_init_core((uint8_t)core_index);
    idle_process_init_core((uint8_t)core_index);

    g_amp.cores[core_index].online = true;

    kprintf("[AMP] Core %u online (LAPIC ID %u, role=%s)\n",
            (uint32_t)core_index, lapic_get_id(),
            g_amp.cores[core_index].is_kcore ? "K-Core" : "App-Core");

    // K-Cores enter the guide loop — processes Pockets from MPSC queue.
    // App Cores idle until the scheduler assigns user processes.
    if (g_amp.cores[core_index].is_kcore) {
        kcore_run_loop();  // never returns
    }

    // App Core: enable interrupts and wait for scheduling
    __asm__ volatile("sti");
    while (1) {
        __asm__ volatile("hlt");
    }
}
