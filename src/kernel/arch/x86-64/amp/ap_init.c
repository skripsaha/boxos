#include "amp.h"
#include "per_core.h"
#include "lapic.h"
#include "idt.h"
#include "klib.h"

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

    g_amp.cores[core_index].online = true;

    kprintf("[AMP] Core %u online (LAPIC ID %u)\n",
            (uint32_t)core_index, lapic_get_id());

    __asm__ volatile("sti");
    while (1) {
        __asm__ volatile("hlt");
    }
}
