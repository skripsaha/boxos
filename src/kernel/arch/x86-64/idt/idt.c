#include "idt.h"
#include "gdt.h"
#include "tss.h"
#include "klib.h"
#include "io.h"
#include "pic.h"
#include "irqchip.h"
#include "lapic.h"
#include "process.h"
#include "guide.h"
#include "ready_queue.h" // for g_ready_queue
#include "pocket_ring.h"
#include "kring.h"
#include "vmm.h"
#include "atomics.h"
#include "scheduler.h"
#include "context_switch.h"
#include "keyboard.h"
#include "ahci.h"
#include "idle.h"
#include "amp.h"
#include "kcore.h"
#include "xhci_interrupt.h"
#include "linker_symbols.h"
#include "cpu_calibrate.h"  // cpu_tsc_recal_tick (periodic recalibration)
#include "touch_queue.h"
#include "pit.h"
#include "irq_defer.h"

static idt_entry_t idt[IDT_ENTRIES];
static idt_descriptor_t idt_desc;

static irq_callback_t irq_callbacks[IRQ_MAX_COUNT] = {NULL};

static volatile uint64_t exception_count = 0;
static volatile uint64_t irq_count[IRQ_MAX_COUNT] = {0};

static void idt_load_asm(uint64_t idt_desc_addr)
{
    asm volatile("lidt (%0)" : : "r"(idt_desc_addr) : "memory");
}

void idt_load(void)
{
    idt_load_asm((uint64_t)&idt_desc);
}

void idt_set_entry(int index, uint64_t handler, uint16_t selector, uint8_t type_attr, uint8_t ist)
{
    idt[index].offset_low = handler & 0xFFFF;
    idt[index].selector = selector;
    idt[index].ist = ist & 0x07;
    idt[index].type_attr = type_attr;
    idt[index].offset_middle = (handler >> 16) & 0xFFFF;
    idt[index].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[index].reserved = 0;
}

void idt_init(void)
{
    debug_printf("[IDT] Initializing Interrupt Descriptor Table (minimal)...\n");

    memset(idt, 0, sizeof(idt));

    idt_desc.limit = sizeof(idt) - 1;
    idt_desc.base = (uint64_t)idt;

    debug_printf("[IDT] Setting up exception handlers (0-31)...\n");

    for (int i = 0; i < 32; i++)
    {
        uint8_t ist = 0; // No IST by default

        // Critical exceptions use IST
        switch (i)
        {
        case 1: // Debug
            ist = IST_DEBUG;
            break;
        case 2: // NMI
            ist = IST_NMI;
            break;
        case 8: // Double Fault
            ist = IST_DOUBLE_FAULT;
            break;
        case 12: // Stack Fault
            ist = IST_STACK_FAULT;
            break;
        case 18: // Machine Check
            ist = IST_MACHINE_CHECK;
            break;
        }

        idt_set_entry(i, (uint64_t)isr_table[i], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, ist);
    }

    debug_printf("[IDT] Setting up IRQ handlers (32-55)...\n");

    // Vectors 32-55: Hardware IRQs (PIC IRQ 0-15 + IO-APIC GSI 16-23)
    for (int i = 32; i < 56; i++)
    {
        idt_set_entry(i, (uint64_t)isr_table[i], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    }

    debug_printf("[IDT] Setting up syscall handler (INT 0x80)...\n");

    // Syscall handler at 0x80 (Ring 3 accessible)
    idt_set_entry(SYSCALL_VECTOR, (uint64_t)isr_table[SYSCALL_VECTOR], GDT_KERNEL_CODE, IDT_TYPE_USER_INTERRUPT, 0);

    // LAPIC timer vector (254) and spurious vector (255)
    idt_set_entry(LAPIC_TIMER_VECTOR, (uint64_t)isr_table[LAPIC_TIMER_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    idt_set_entry(LAPIC_SPURIOUS_VECTOR, (uint64_t)isr_table[LAPIC_SPURIOUS_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);

    // AHCI MSI vector (0x70) — message-signalled interrupt from the HBA
    idt_set_entry(AHCI_MSI_VECTOR, (uint64_t)isr_table[AHCI_MSI_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);

    // AMP IPI vectors (0xF0-0xF2)
    idt_set_entry(IPI_WAKE_VECTOR, (uint64_t)isr_table[IPI_WAKE_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    idt_set_entry(IPI_SHOOTDOWN_VECTOR, (uint64_t)isr_table[IPI_SHOOTDOWN_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    idt_set_entry(IPI_PANIC_VECTOR, (uint64_t)isr_table[IPI_PANIC_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);

    idt_load_asm((uint64_t)&idt_desc);

    debug_printf("[IDT] IDT loaded successfully at 0x%lx (limit=%d)\n", idt_desc.base, idt_desc.limit);
}

void irq_register_handler(uint8_t irq, irq_callback_t callback)
{
    if (irq >= IRQ_MAX_COUNT)
    {
        debug_printf("[IDT] ERROR: Invalid IRQ/GSI %u (max %u)\n", irq, IRQ_MAX_COUNT - 1);
        return;
    }

    if (irq_callbacks[irq] != NULL)
    {
        debug_printf("[IDT] WARNING: Overwriting existing handler for IRQ %u\n", irq);
    }

    irq_callbacks[irq] = callback;
    debug_printf("[IDT] Registered callback for IRQ %u\n", irq);
}

void irq_unregister_handler(uint8_t irq)
{
    if (irq >= IRQ_MAX_COUNT)
    {
        debug_printf("[IDT] ERROR: Invalid IRQ/GSI %u (max %u)\n", irq, IRQ_MAX_COUNT - 1);
        return;
    }

    irq_callbacks[irq] = NULL;
    debug_printf("[IDT] Unregistered callback for IRQ %u\n", irq);
}

static process_t *find_process_by_kernel_stack_overflow(uint64_t rsp)
{
    // Called from IST exception handlers (double fault / stack fault).
    // process_lock may be held by the interrupted code, so use trylock
    // to avoid deadlock. If we can't get the lock, fall back to unlocked
    // iteration — best-effort crash recovery.
    bool locked = spin_trylock_process_list();

    process_t *proc = process_get_first();
    while (proc)
    {
        if (proc->magic == PROCESS_MAGIC && proc->kernel_stack_guard_base)
        {
            uintptr_t guard_start = (uintptr_t)proc->kernel_stack_guard_base;
            uintptr_t guard_end = guard_start + (CONFIG_KERNEL_STACK_GUARD_PAGES * CONFIG_PAGE_SIZE);
            if (rsp >= guard_start && rsp < guard_end)
            {
                if (locked)
                    process_list_unlock();
                return proc;
            }
        }
        proc = proc->next;
    }

    if (locked)
        process_list_unlock();
    return NULL;
}

/* Friendly mnemonic for the first 32 Intel exception vectors (SDM Vol 3
 * §6.15 Table 6-1). Returned strings are static literals; safe to embed
 * in kprintf without lifetime concerns. Vector >= 32 (= IRQ) returns
 * "INT" — exception_handler is only called for the 0-31 range, but the
 * defensive branch keeps the helper total. */
static const char *exception_mnemonic(uint8_t vector)
{
    switch (vector) {
    case 0:  return "#DE";
    case 1:  return "#DB";
    case 2:  return "NMI";
    case 3:  return "#BP";
    case 4:  return "#OF";
    case 5:  return "#BR";
    case 6:  return "#UD";
    case 7:  return "#NM";
    case 8:  return "#DF";
    case 10: return "#TS";
    case 11: return "#NP";
    case 12: return "#SS";
    case 13: return "#GP";
    case 14: return "#PF";
    case 16: return "#MF";
    /* #AC (vector 17) — Intel SDM Vol 3 §6.15 Table 6-1. Two distinct
     * triggers reach here: (1) classical alignment-check from a
     * misaligned user-mode memory access while CR0.AM=1 and EFLAGS.AC=1,
     * and (2) split-lock detection when IA32_CORE_CAPABILITIES.bit5 +
     * TEST_CTL.bit29 are both set and a LOCK-prefixed instruction spans
     * a cache line. BoxOS clears TEST_CTL.bit29 at every BSP/AP bringup
     * (see cpu_test_ctl_init in cpuid.c), so #AC reaching this handler
     * indicates either a misconfigured boot path, a hostile guest VM
     * lying about CPUID, or a real classical AC — all worth a clear
     * diagnostic instead of an anonymous "Exception #17". */
    case 17: return "#AC";
    case 18: return "#MC";
    case 19: return "#XF";
    case 20: return "#VE";
    case 21: return "#CP";
    default: return "INT";
    }
}

void exception_handler(interrupt_frame_t *frame)
{
    atomic_fetch_add_u64(&exception_count, 1);

    /* Phase 2F — #MC (vector 18) routes to MCE subsystem. The handler
     * runs on the IST_MACHINE_CHECK stack (set by idt_init), walks every
     * IA32_MC<i>_STATUS bank, poisons phys pages, publishes Touch
     * events, and returns true if the error was recoverable (UC=0 or
     * UCR with RIPV=1). On unrecoverable error: fall through to the
     * standard kill-process / system_halt path below. */
    if (frame->vector == 18) {
        extern bool mce_handle(interrupt_frame_t *);
        if (mce_handle(frame)) {
            return;  /* recovered — IRET back to user/kernel */
        }
        /* fatal — drop into the generic exception path. (cs ring tells
         * exception_handler whether to kill the process or halt.) */
    }

    if (frame->vector == 14)
    {
        uint64_t fault_addr;
        __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

        int result = vmm_handle_page_fault(fault_addr, frame->error_code);
        if (result == 0)
        {
            return;
        }

        /* Unhandled kernel-mode PF — full register dump for the FIRST
         * N occurrences, then a single throttle notice, then keep
         * IRET-retrying. The previous "log once globally then
         * silently swallow forever" hid every subsequent real bug
         * (NULL deref, stack overflow) behind the first transient
         * demand-paging miss. Limiting per-occurrence rather than
         * boolean preserves diagnosability while still allowing
         * transient demand-paging races to resolve through retry. */
        if ((frame->cs & 3) == 0) {
            static volatile uint32_t kpf_logged = 0;
            uint32_t n = __atomic_add_fetch(&kpf_logged, 1u, __ATOMIC_RELAXED);
            if (n <= 4) {
                kprintf("\n[VMM] Unhandled kernel PF #%u at 0x%lx err=0x%lx\n",
                        n, fault_addr, frame->error_code);
                kprintf("[VMM]   RIP=0x%lx RSP=0x%lx CS=0x%lx\n",
                        frame->rip, frame->rsp, frame->cs);
                kprintf("[VMM]   RAX=0x%lx RBX=0x%lx RCX=0x%lx RDX=0x%lx\n",
                        frame->rax, frame->rbx, frame->rcx, frame->rdx);
                kprintf("[VMM]   RSI=0x%lx RDI=0x%lx RBP=0x%lx\n",
                        frame->rsi, frame->rdi, frame->rbp);
            } else if (n == 5) {
                kprintf("[VMM] (further unhandled kernel PFs throttled)\n");
            }
            return;
        }
    }

    // User-mode exception: kill the faulting process, don't crash the kernel
    if ((frame->cs & 3) == 3)
    {
        process_t *proc = process_get_current();
        if (proc)
        {
            kprintf("\n");
            kprintf("[EXCEPTION] User-mode %s (vector %u) in PID %u\n",
                    exception_mnemonic((uint8_t)frame->vector),
                    frame->vector, proc->pid);
            kprintf("[EXCEPTION] RIP=0x%lx RSP=0x%lx Error=0x%lx\n",
                    frame->rip, frame->rsp, frame->error_code);
            kprintf("[EXCEPTION] TagBits: 0x%lx\n", proc->tag_bits);
            /* Split-lock #AC hint: if userspace fired #AC with error_code==0
             * while BoxOS was supposed to clear TEST_CTL.bit29, the
             * configuration drifted (BIOS re-asserted bit29 mid-runtime
             * is rare but documented). Surface the hint inline so the
             * operator knows what to investigate. */
            if (frame->vector == 17 && frame->error_code == 0) {
                kprintf("[EXCEPTION] Note: #AC error_code=0 suggests a "
                        "cache-line-spanning LOCK access; check TEST_CTL "
                        "MSR 0x33 bit 29 state on this core.\n");
            }

            if (frame->vector == 14)
            {
                uint64_t fault_addr;
                __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));
                kprintf("[EXCEPTION] Page Fault at 0x%lx (P=%d W=%d U=%d R=%d)\n",
                        fault_addr,
                        (int)(frame->error_code & 0x1),
                        (int)((frame->error_code >> 1) & 0x1),
                        (int)((frame->error_code >> 2) & 0x1),
                        (int)((frame->error_code >> 3) & 0x1));
                /* Full register dump on user page fault — enables remote
                 * debugging of "wild pointer in r10" / similar memory-
                 * corruption bugs without requiring a connected gdb. */
                kprintf("[EXCEPTION]  RAX=%016lx RBX=%016lx RCX=%016lx\n",
                        frame->rax, frame->rbx, frame->rcx);
                kprintf("[EXCEPTION]  RDX=%016lx RSI=%016lx RDI=%016lx\n",
                        frame->rdx, frame->rsi, frame->rdi);
                kprintf("[EXCEPTION]  RBP=%016lx  R8=%016lx  R9=%016lx\n",
                        frame->rbp, frame->r8, frame->r9);
                kprintf("[EXCEPTION]  R10=%016lx R11=%016lx R12=%016lx\n",
                        frame->r10, frame->r11, frame->r12);
                kprintf("[EXCEPTION]  R13=%016lx R14=%016lx R15=%016lx\n",
                        frame->r13, frame->r14, frame->r15);
            }

            kprintf("[EXCEPTION] Killing PID %u and scheduling next process\n", proc->pid);

            // Mark process as crashed so scheduler won't pick it again
            process_set_state(proc, PROC_CRASHED);

            // Force a reschedule via the frame
            schedule(frame);
            return;
        }
    }

    // Kernel stack overflow recovery via double fault / stack fault
    if (frame->vector == 8 || frame->vector == 12)
    {
        process_t *overflow_proc = find_process_by_kernel_stack_overflow(frame->rsp);
        if (!overflow_proc)
        {
            overflow_proc = process_get_current();
            if (overflow_proc && process_is_idle(overflow_proc))
            {
                // idle process stack overflow is fatal — it cannot be killed
                kprintf("\n");
                kprintf("================================================================\n");
                kprintf("FATAL: Idle process (PID 0) kernel stack overflow\n");
                kprintf("  Exception #%u  RSP: 0x%lx  RIP: 0x%lx\n",
                        frame->vector, frame->rsp, frame->rip);
                kprintf("================================================================\n");
                overflow_proc = NULL;
                // fall through to kernel panic
            }
        }

        if (overflow_proc)
        {
            kprintf("\n");
            kprintf("================================================================\n");
            kprintf("KERNEL STACK OVERFLOW: Exception #%u recovered\n", frame->vector);
            kprintf("================================================================\n");
            kprintf("  PID: %u  TagBits: 0x%lx\n", overflow_proc->pid, overflow_proc->tag_bits);
            kprintf("  RSP: 0x%lx  RIP: 0x%lx\n", frame->rsp, frame->rip);
            if (overflow_proc->kernel_stack_guard_base)
            {
                kprintf("  Guard: 0x%lx  Stack: 0x%lx-0x%lx\n",
                        (uintptr_t)overflow_proc->kernel_stack_guard_base,
                        (uintptr_t)overflow_proc->kernel_stack,
                        (uintptr_t)overflow_proc->kernel_stack_top);
            }
            kprintf("  Killing PID %u, system continues.\n", overflow_proc->pid);
            kprintf("================================================================\n");

            process_set_state(overflow_proc, PROC_CRASHED);

            // If the faulting code held the scheduler lock, force-release it
            // so schedule() can acquire it without deadlock.
            // We use spin_force_release (NOT spin_unlock) because we're on an
            // IST stack — the original holder's saved_flags are meaningless here.
            // Safety: scheduler_get_state() returns THIS core's state (IST never
            // migrates to another core), so we always release our own lock.
            scheduler_state_t *sched = scheduler_get_state();
            if (sched->scheduler_lock.locked)
            {
                debug_printf("[IST] Core %u: force-releasing scheduler lock for crashed PID %u\n",
                             amp_get_core_index(), overflow_proc->pid);
                spin_force_release(&sched->scheduler_lock);
            }
            __asm__ volatile("cli");

            schedule(frame);
            return;
        }
    }

    // Kernel-mode exception: this is a real kernel panic
    kprintf("\n");
    kprintf("====================================================================\n");
    kprintf("KERNEL PANIC: %s (vector %u)\n",
            exception_mnemonic((uint8_t)frame->vector), frame->vector);
    kprintf("====================================================================\n");
    kprintf("Error code: 0x%lx\n", frame->error_code);
    if (frame->vector == 17) {
        kprintf("Hint: #AC in kernel mode means BoxOS code emitted a "
                "cache-line-spanning LOCK instruction. Either a regression "
                "in atomic-target alignment or TEST_CTL bit 29 was re-asserted "
                "by firmware after boot. Fix the alignment in the offending "
                "atomic, don't mask the detection.\n");
    }
    kprintf("Exception count: %lu  Core: %u\n", atomic_load_u64(&exception_count), (uint32_t)amp_get_core_index());

    // Full GPR dump
    kprintf("  RAX=%016lx  RBX=%016lx\n", frame->rax, frame->rbx);
    kprintf("  RCX=%016lx  RDX=%016lx\n", frame->rcx, frame->rdx);
    kprintf("  RSI=%016lx  RDI=%016lx\n", frame->rsi, frame->rdi);
    kprintf("  RBP=%016lx  RSP=%016lx\n", frame->rbp, frame->rsp);
    kprintf("  R8 =%016lx  R9 =%016lx\n", frame->r8, frame->r9);
    kprintf("  R10=%016lx  R11=%016lx\n", frame->r10, frame->r11);
    kprintf("  R12=%016lx  R13=%016lx\n", frame->r12, frame->r13);
    kprintf("  R14=%016lx  R15=%016lx\n", frame->r14, frame->r15);
    kprintf("  RIP=%016lx  RFL=%016lx\n", frame->rip, frame->rflags);
    kprintf("  CS=%04lx  SS=%04lx\n", frame->cs, frame->ss);

    uint64_t panic_cr2, panic_cr3;
    __asm__ volatile("mov %%cr2, %0" : "=r"(panic_cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(panic_cr3));
    kprintf("  CR2=%016lx  CR3=%016lx\n", panic_cr2, panic_cr3);

    if (frame->vector == 14)
    {
        kprintf("Page Fault at 0x%lx (P=%d W=%d U=%d R=%d I=%d)\n",
                panic_cr2,
                (int)(frame->error_code & 0x1),
                (int)((frame->error_code >> 1) & 0x1),
                (int)((frame->error_code >> 2) & 0x1),
                (int)((frame->error_code >> 3) & 0x1),
                (int)((frame->error_code >> 4) & 0x1));
    }

    // RBP-chain stack walk
    kprintf("Stack trace:\n");
    uint64_t walk_rbp = frame->rbp;
    for (uint32_t depth = 0; depth < 20; depth++)
    {
        if (walk_rbp == 0 || (walk_rbp & 7) != 0)
            break;
        if (!vmm_is_kernel_addr(walk_rbp))
            break;

        uint64_t *fp = (uint64_t *)walk_rbp;
        uint64_t saved_rbp = fp[0];
        uint64_t ret_addr = fp[1];

        bool in_text = (ret_addr >= (uint64_t)_text_start && ret_addr < (uint64_t)_text_end);
        kprintf("  #%u  %016lx%s\n", depth, ret_addr, in_text ? "" : "  [!]");

        if (saved_rbp <= walk_rbp || saved_rbp == 0)
            break;
        walk_rbp = saved_rbp;
    }

    kprintf("====================================================================\n");
    kprintf("System halted.\n");

    // Halt all cores via IPI_PANIC
    if (g_amp.multicore_active)
    {
        for (uint8_t c = 0; c < g_amp.total_cores; c++)
        {
            if (c == amp_get_core_index())
                continue;
            if (!amp_core_online(&g_amp.cores[c]))
                continue;
            lapic_send_ipi(g_amp.cores[c].lapic_id, IPI_PANIC_VECTOR);
        }
    }

    while (1)
    {
        asm volatile("cli; hlt");
    }
}

void irq_handler(interrupt_frame_t *frame)
{
    uint8_t vector = frame->vector;

    // LAPIC spurious vector: do NOT send EOI
    if (vector == LAPIC_SPURIOUS_VECTOR)
    {
        return;
    }

    /* LAPIC timer vector: per-core tick counter + scheduling.
     *
     * Multi-core layout: BSP is a K-Core running kcore_run_loop, App
     * Cores run user processes. Only App Cores need preemptive
     * `schedule()` — calling it on a K-Core would hijack the kernel
     * loop. Hence the `amp_is_appcore()` gate.
     *
     * Single-core layout (AMP NOT active, or only one core total):
     * the BSP IS the App Core. The previous code never scheduled in
     * that case — processes were started but never switched, the
     * initial process held the CPU forever (audit 2026-04-30, BIOS
     * and UEFI single-core both reproduced).
     *
     * Schedule when either: (a) we're an App Core in multi-core, or
     * (b) AMP is inactive — i.e. there is no separate K-Core. */
    if (vector == LAPIC_TIMER_VECTOR)
    {
        scheduler_state_t *s = scheduler_get_state();
        s->total_ticks++;
        /* Only App Cores reschedule on timer. K-Cores must NEVER call
         * schedule() — it would pick idle and hijack the K-Core's stack.
         *
         * Use total_cores (set ONCE during amp_init() before any AP boots)
         * instead of multicore_active (set LATE, after every AP comes
         * online). The previous `!g_amp.multicore_active` check
         * incorrectly fired for AP K-Cores whose LAPIC timer ticked
         * during the AP-boot window — schedule() ran on the K-Core and
         * permanently hijacked it into the per-core idle process. */
        if (amp_is_appcore() || g_amp.total_cores == 1)
        {
            schedule(frame);
        }
        /* Re-arm the next tick. No-op unless the timer runs in TSC-deadline
         * mode (one-shot per deadline); periodic mode reloads itself. */
        lapic_timer_rearm();
        lapic_send_eoi();
        return;
    }

    // IPI_WAKE: On App Cores, reschedule to pick up newly-woken processes.
    // On K-Cores, just ACK — the interrupt breaks HLT in kcore_run_loop.
    if (vector == IPI_WAKE_VECTOR)
    {
        if (amp_is_appcore() || g_amp.total_cores == 1)
        {
            schedule(frame);
        }
        lapic_send_eoi();
        return;
    }

    if (vector == IPI_SHOOTDOWN_VECTOR)
    {
        vmm_tlb_shootdown_handler();
        lapic_send_eoi();
        return;
    }

    if (vector == IPI_PANIC_VECTOR)
    {
        lapic_send_eoi();
        while (1)
        {
            asm volatile("cli; hlt");
        }
    }

    /* AHCI message-signalled interrupt. MSI is delivered point-to-point to
     * the LAPIC, so it is acknowledged with a LAPIC EOI — never the IOAPIC.
     * The handler itself is allocation-free and defers heavy work. */
    if (vector == AHCI_MSI_VECTOR)
    {
        ahci_irq_handler();
        lapic_send_eoi();
        return;
    }

    // Standard hardware IRQ (vectors 32-55 -> IRQ 0-23)
    uint8_t irq = irq_vector_to_gsi(vector);
    if (irq >= IRQ_MAX_COUNT)
    {
        // Unknown vector outside our IRQ range — send EOI to prevent stuck interrupts
        irqchip_send_eoi(0);
        return;
    }
    atomic_fetch_add_u64(&irq_count[irq], 1);

    if (irq_callbacks[irq] != NULL)
    {
        irq_callbacks[irq]();
        irqchip_send_eoi(irq);
        return;
    }

    switch (irq)
    {
    case 0:
    {
        // Timer IRQ (PIT via PIC or IO-APIC GSI 0)
        scheduler_state_t *sched = scheduler_get_state();
        sched->total_ticks++;
        pit_tick();

        /* Advance the global scheduler clock from the MONOTONIC wall-clock at a
         * FIXED logical rate, decoupled from the adaptive PIT IRQ frequency.
         * The PIT IRQ rate swings 10-500 Hz with load, so a per-IRQ "+1" made a
         * tick's wall-clock duration drift — starvation/affinity thresholds
         * (counted in ticks) wobbled with load. Deriving the tick from
         * pit_get_uptime_us() pins it to SCHEDULER_DEFAULT_TICK_HZ (the rate
         * consumers were tuned for), now stable. Monotonic (uptime only grows);
         * single writer (BSP IRQ0); all consumers use deltas. */
        __atomic_store_n(&g_global_tick,
                         pit_get_uptime_us() / (1000000ULL / SCHEDULER_DEFAULT_TICK_HZ),
                         __ATOMIC_RELAXED);

        /* Periodic scheduler parameter recalculation */
        scheduler_recalc_parameters();

        /* Periodic TSC recalibration tick. Cheap — just stamps a
         * timestamp + sets a pending flag every TSC_RECAL_INTERVAL_US.
         * The actual ~20ms measurement runs in idle context
         * (cpu_tsc_recal_if_pending) — never in IRQ. */
        cpu_tsc_recal_tick(pit_get_uptime_us());

        /* Software key repeat driven by PIT tick */
        keyboard_timer_tick();

        /* Deferred touch event delivery */
        TouchQueueTick(__atomic_load_n(&g_global_tick, __ATOMIC_RELAXED));

        /* xHCI events handled via IRQ; poll only as fallback */
        xhci_poll_events();

        /* Single-core mode: drain irq_defer here, when the PIT IRQ
         * interrupted USERSPACE code.
         *
         * Background: irq_defer is the universal IRQ→K-Core hand-off
         * for bottom-half work that touches kmalloc / tagfs / process
         * tables. In multi-core configurations, K-Cores drain their
         * own rings inside `kcore_run_loop`. Single-core mode does NOT
         * run kcore_run_loop (the BSP runs userspace directly via
         * scheduler), so without an explicit pump nothing ever drains
         * the queued deferred handlers (keyboard Touch, USB Touch,
         * SCI/GPE notifications, write_job completions, ...).
         *
         * Lock-safety: the deferred handlers take heap_lock, tagfs
         * locks, process_lock, etc. If the PIT IRQ interrupted a
         * kernel-mode syscall holding one of those locks, pumping here
         * would deadlock against the interrupted thread on the same
         * core. We gate the pump on `(frame->cs & 3) == 3` — the
         * interrupted code was in user mode (CS=USER_CS, RPL=3) — which
         * guarantees no kernel lock is held. Multi-core takes the
         * `amp_is_appcore()` branch above and does not pump here. */
        if (g_amp.total_cores == 1 && (frame->cs & 3) == 3) {
            irq_defer_pump(0);
        }

        /* PIT IRQ 0: same scheduling rule as the LAPIC timer above —
         * preempt on App Cores or in single-core mode where the BSP
         * itself runs userspace. Use total_cores (set during amp_init
         * before any AP boots) — `multicore_active` is set LATE and was
         * causing AP K-Cores to be hijacked into idle on early ticks. */
        if (amp_is_appcore() || g_amp.total_cores == 1)
        {
            schedule(frame);
        }
        break;
    }

    case 1:
    {
        uint8_t scancode = inb(0x60);
        keyboard_handle_scancode(scancode);
        break;
    }

    default:
        /* Legacy IDE IRQ14/15 are intentionally not dispatched. The
         * PIO driver sets nIEN=1 on every detected drive and leaves
         * the IOAPIC pins masked, so the channel IRQ line never
         * fires. If something in the future re-enables it, falling
         * through here sends a clean EOI without acting on the
         * vector. */
        break;
    }

    irqchip_send_eoi(irq);
}

// ---------------------------------------------------------------------------
// Syscall dispatch: sync (single-core) vs async (multi-core)
// ---------------------------------------------------------------------------

// Sync path: process Pockets immediately on the calling core (N=1).
static void sync_syscall_dispatch(process_t *proc, interrupt_frame_t *frame)
{
    context_save_from_frame(proc, frame);
    ready_queue_push(&g_ready_queue, proc);
    process_set_state(proc, PROC_WAITING);
    guide();
    schedule(frame);
}

// Async path: non-blocking doorbell to K-Core (N>1).
// Process stays PROC_WORKING and returns to userspace immediately.
// K-Core processes Pockets in parallel, writes Results to ResultRing.
// Process spins in result_wait() with pause until Result appears.
// No state change, no schedule(), no IPI back — true parallelism.
static void async_syscall_dispatch(process_t *proc, interrupt_frame_t *frame)
{
    (void)frame; // not needed — process continues running

    uint8_t core_idx = amp_get_core_index();

    // CAS: only submit if not already queued (dedup)
    if (atomic_cas_u8(&proc->kcore_pending, 0, 1))
    {
        debug_printf("[A%u] ASYNC submit PID %u to K-Core\n", core_idx, proc->pid);
        kcore_submit(proc);
    }
    else
    {
        debug_printf("[A%u] ASYNC skip PID %u (already pending)\n", core_idx, proc->pid);
    }
    // Return to userspace — iretq restores frame, process keeps running.
}

// Function pointer set at boot — never changes at runtime.
static void (*g_syscall_dispatch)(process_t *, interrupt_frame_t *) = sync_syscall_dispatch;

void idt_set_syscall_mode(bool multicore)
{
    if (multicore)
    {
        g_syscall_dispatch = async_syscall_dispatch;
        debug_printf("[IDT] Syscall mode: ASYNC (multi-core, non-blocking)\n");
    }
    else
    {
        g_syscall_dispatch = sync_syscall_dispatch;
        debug_printf("[IDT] Syscall mode: SYNC (single-core)\n");
    }
}

// ---------------------------------------------------------------------------
// syscall_handler — unified entry point for SYSCALL and INT 0x80
// ---------------------------------------------------------------------------

void syscall_handler(interrupt_frame_t *frame)
{
    scheduler_state_t *sched_state = scheduler_get_state();

    spin_lock(&sched_state->scheduler_lock);
    process_t *proc = sched_state->current_process;
    if (!proc || proc->magic != PROCESS_MAGIC)
    {
        spin_unlock(&sched_state->scheduler_lock);
        frame->rax = (uint64_t)-1;
        return;
    }
    spin_unlock(&sched_state->scheduler_lock);

    frame->rax = 0;

    /* Yield short-circuit. Cooperative-scheduling hint: pockets tagged
     * with POCKET_FLAG_YIELD skip guide() — the process stays WORKING
     * and just gives up its timeslice, returning to the run queue on
     * the next tick. Must be checked BEFORE the async dispatch path
     * to avoid a kcore_pending re-arm race. */
    Pocket *peek = KPocketPeek(proc);

    if (peek && (peek->flags & POCKET_FLAG_YIELD))
    {
        KPocketPop(proc);
        context_save_from_frame(proc, frame);
        schedule(frame);
        return;
    }

    // Dispatch: sync blocks + schedules, async returns immediately.
    g_syscall_dispatch(proc, frame);
}
