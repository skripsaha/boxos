#include "amp.h"
#include "per_core.h"
#include "lapic.h"
#include "idt.h"
#include "irqchip.h"
#include "klib.h"
#include "scheduler.h"
#include "idle.h"
#include "kcore.h"
#include "cpuid.h"

void ap_entry_c(uint64_t core_index, uint64_t stack_top) {
    // per_core_init_ap sets up:
    //   - Per-core GDT (with per-core TSS descriptor)
    //   - Per-core TSS (rsp0, IST stacks with guard pages)
    //   - FPU/SSE/AVX
    //   - SYSCALL MSRs (EFER.SCE+NXE, STAR, LSTAR, SFMASK)
    //   - PerCpuData + MSR_KERNEL_GS_BASE (for swapgs)
    //   - LAPIC enable + LAPIC timer (100Hz periodic)
    per_core_init_ap((uint8_t)core_index, stack_top);

    /* Apply MADT Local APIC NMI entries to this AP's LVT (mirrors the
     * BSP step in irqchip_init). Without it, NMI watchdogs only fire on
     * the BSP. Safe no-op when running on PIC fallback. */
    irqchip_apply_lapic_nmi_self();

    // IDT is shared across all cores (single static table)
    idt_load();

    // Per-core scheduler + idle process.
    // MUST be before sti — LAPIC timer is already counting and will fire
    // as soon as interrupts are enabled, calling schedule().
    scheduler_init_core((uint8_t)core_index);
    idle_process_init_core((uint8_t)core_index);

    /* Publish online=true with __ATOMIC_RELEASE so the BSP's amp_boot_aps
     * acquire-load — and every peer that asks amp_core_online() — observes a
     * fully-initialized per-core state (GDT/TSS/IST/LAPIC/timer/notify MSRs)
     * before they see this flag set. On x86 TSO the prior plain stores are
     * already ordered, but the explicit release pairs with the explicit
     * acquire on read sites so the discipline is portable and machine-
     * checkable rather than implicit. */
    __atomic_store_n(&g_amp.cores[core_index].online, (uint8_t)1, __ATOMIC_RELEASE);

    kprintf("[AMP] Core %u online (LAPIC ID %u, role=%s)\n",
            (uint32_t)core_index, lapic_get_id(),
            g_amp.cores[core_index].is_kcore ? "K-Core" : "App-Core");

    /* Per-AP CPU identity + microcode revision log. Operator-visible
     * record of exactly what silicon services this core; lets the boot
     * log surface heterogeneous packages (different family/model on
     * different cores) and stale microcode on individual sockets. The
     * cpu_intersect_features_ap call earlier emits a separate "feature
     * drop" line if this AP forced any kernel-wide capability off. */
    char prefix[24];
    ksnprintf(prefix, sizeof(prefix), "AP %u [%c]",
              (unsigned)core_index,
              g_amp.cores[core_index].is_kcore ? 'K' : 'A');
    cpu_log_identity(prefix);

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
