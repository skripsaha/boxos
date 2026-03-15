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
#include "pocket_ring.h"
#include "vmm.h"
#include "atomics.h"
#include "scheduler.h"
#include "context_switch.h"
#include "keyboard.h"
#include "ata_dma.h"
#include "idle.h"
#include "amp.h"
#include "kcore.h"

static idt_entry_t idt[IDT_ENTRIES];
static idt_descriptor_t idt_desc;

static irq_callback_t irq_callbacks[IRQ_MAX_COUNT] = {NULL};

static uint64_t exception_count = 0;
static uint64_t irq_count[IRQ_MAX_COUNT] = {0};

static void idt_load_asm(uint64_t idt_desc_addr) {
    asm volatile("lidt (%0)" : : "r" (idt_desc_addr) : "memory");
}

void idt_load(void) {
    idt_load_asm((uint64_t)&idt_desc);
}

void idt_set_entry(int index, uint64_t handler, uint16_t selector, uint8_t type_attr, uint8_t ist) {
    idt[index].offset_low = handler & 0xFFFF;
    idt[index].selector = selector;
    idt[index].ist = ist & 0x07;
    idt[index].type_attr = type_attr;
    idt[index].offset_middle = (handler >> 16) & 0xFFFF;
    idt[index].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[index].reserved = 0;
}

void idt_init(void) {
    debug_printf("[IDT] Initializing Interrupt Descriptor Table (minimal)...\n");

    memset(idt, 0, sizeof(idt));

    idt_desc.limit = sizeof(idt) - 1;
    idt_desc.base = (uint64_t)idt;

    debug_printf("[IDT] Setting up exception handlers (0-31)...\n");

    for (int i = 0; i < 32; i++) {
        uint8_t ist = 0;  // No IST by default

        // Critical exceptions use IST
        switch(i) {
            case 1:   // Debug
                ist = IST_DEBUG;
                break;
            case 2:   // NMI
                ist = IST_NMI;
                break;
            case 8:   // Double Fault
                ist = IST_DOUBLE_FAULT;
                break;
            case 12:  // Stack Fault
                ist = IST_STACK_FAULT;
                break;
            case 18:  // Machine Check
                ist = IST_MACHINE_CHECK;
                break;
        }

        idt_set_entry(i, (uint64_t)isr_table[i], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, ist);
    }

    debug_printf("[IDT] Setting up IRQ handlers (32-55)...\n");

    // Vectors 32-55: Hardware IRQs (PIC IRQ 0-15 + IO-APIC GSI 16-23)
    for (int i = 32; i < 56; i++) {
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

    // AMP IPI vectors (0xF0-0xF2)
    idt_set_entry(IPI_WAKE_VECTOR,      (uint64_t)isr_table[IPI_WAKE_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    idt_set_entry(IPI_SHOOTDOWN_VECTOR, (uint64_t)isr_table[IPI_SHOOTDOWN_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    idt_set_entry(IPI_PANIC_VECTOR,     (uint64_t)isr_table[IPI_PANIC_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);

    idt_load_asm((uint64_t)&idt_desc);

    debug_printf("[IDT] IDT loaded successfully at 0x%lx (limit=%d)\n", idt_desc.base, idt_desc.limit);
}

void irq_register_handler(uint8_t irq, irq_callback_t callback) {
    if (irq >= IRQ_MAX_COUNT) {
        debug_printf("[IDT] ERROR: Invalid IRQ/GSI %u (max %u)\n", irq, IRQ_MAX_COUNT - 1);
        return;
    }

    if (irq_callbacks[irq] != NULL) {
        debug_printf("[IDT] WARNING: Overwriting existing handler for IRQ %u\n", irq);
    }

    irq_callbacks[irq] = callback;
    debug_printf("[IDT] Registered callback for IRQ %u\n", irq);
}

void irq_unregister_handler(uint8_t irq) {
    if (irq >= IRQ_MAX_COUNT) {
        debug_printf("[IDT] ERROR: Invalid IRQ/GSI %u (max %u)\n", irq, IRQ_MAX_COUNT - 1);
        return;
    }

    irq_callbacks[irq] = NULL;
    debug_printf("[IDT] Unregistered callback for IRQ %u\n", irq);
}

static process_t* find_process_by_kernel_stack_overflow(uint64_t rsp) {
    // Called from IST exception handlers (double fault / stack fault).
    // process_lock may be held by the interrupted code, so use trylock
    // to avoid deadlock. If we can't get the lock, fall back to unlocked
    // iteration — best-effort crash recovery.
    bool locked = spin_trylock_process_list();

    process_t* proc = process_get_first();
    while (proc) {
        if (proc->magic == PROCESS_MAGIC && proc->kernel_stack_guard_base) {
            uintptr_t guard_start = (uintptr_t)proc->kernel_stack_guard_base;
            uintptr_t guard_end = guard_start + (CONFIG_KERNEL_STACK_GUARD_PAGES * CONFIG_PAGE_SIZE);
            if (rsp >= guard_start && rsp < guard_end) {
                if (locked) process_list_unlock();
                return proc;
            }
        }
        proc = proc->next;
    }

    if (locked) process_list_unlock();
    return NULL;
}

void exception_handler(interrupt_frame_t* frame) {
    exception_count++;

    if (frame->vector == 14) {
        uint64_t fault_addr;
        __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

        int result = vmm_handle_page_fault(fault_addr, frame->error_code);
        if (result == 0) {
            return;
        }
    }

    // User-mode exception: kill the faulting process, don't crash the kernel
    if ((frame->cs & 3) == 3) {
        process_t* proc = process_get_current();
        if (proc) {
            kprintf("\n");
            kprintf("[EXCEPTION] User-mode exception #%u in PID %u\n",
                    frame->vector, proc->pid);
            kprintf("[EXCEPTION] RIP=0x%lx RSP=0x%lx Error=0x%lx\n",
                    frame->rip, frame->rsp, frame->error_code);
            kprintf("[EXCEPTION] TagBits: 0x%lx\n", proc->tag_bits);

            if (frame->vector == 14) {
                uint64_t fault_addr;
                __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));
                kprintf("[EXCEPTION] Page Fault at 0x%lx (P=%d W=%d U=%d R=%d)\n",
                        fault_addr,
                        (int)(frame->error_code & 0x1),
                        (int)((frame->error_code >> 1) & 0x1),
                        (int)((frame->error_code >> 2) & 0x1),
                        (int)((frame->error_code >> 3) & 0x1));
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
    if (frame->vector == 8 || frame->vector == 12) {
        process_t* overflow_proc = find_process_by_kernel_stack_overflow(frame->rsp);
        if (!overflow_proc) {
            overflow_proc = process_get_current();
            if (overflow_proc && process_is_idle(overflow_proc)) {
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

        if (overflow_proc) {
            kprintf("\n");
            kprintf("================================================================\n");
            kprintf("KERNEL STACK OVERFLOW: Exception #%u recovered\n", frame->vector);
            kprintf("================================================================\n");
            kprintf("  PID: %u  TagBits: 0x%lx\n", overflow_proc->pid, overflow_proc->tag_bits);
            kprintf("  RSP: 0x%lx  RIP: 0x%lx\n", frame->rsp, frame->rip);
            if (overflow_proc->kernel_stack_guard_base) {
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
            scheduler_state_t* sched = scheduler_get_state();
            if (sched->scheduler_lock.locked) {
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
    kprintf("KERNEL PANIC: Exception #%u\n", frame->vector);
    kprintf("====================================================================\n");
    kprintf("Error code: 0x%lx\n", frame->error_code);
    kprintf("Exception count: %lu  Core: %u\n", exception_count, (uint32_t)amp_get_core_index());

    // Full GPR dump
    kprintf("  RAX=%016lx  RBX=%016lx\n", frame->rax, frame->rbx);
    kprintf("  RCX=%016lx  RDX=%016lx\n", frame->rcx, frame->rdx);
    kprintf("  RSI=%016lx  RDI=%016lx\n", frame->rsi, frame->rdi);
    kprintf("  RBP=%016lx  RSP=%016lx\n", frame->rbp, frame->rsp);
    kprintf("  R8 =%016lx  R9 =%016lx\n", frame->r8,  frame->r9);
    kprintf("  R10=%016lx  R11=%016lx\n", frame->r10, frame->r11);
    kprintf("  R12=%016lx  R13=%016lx\n", frame->r12, frame->r13);
    kprintf("  R14=%016lx  R15=%016lx\n", frame->r14, frame->r15);
    kprintf("  RIP=%016lx  RFL=%016lx\n", frame->rip, frame->rflags);
    kprintf("  CS=%04lx  SS=%04lx\n", frame->cs, frame->ss);

    uint64_t panic_cr2, panic_cr3;
    __asm__ volatile("mov %%cr2, %0" : "=r"(panic_cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(panic_cr3));
    kprintf("  CR2=%016lx  CR3=%016lx\n", panic_cr2, panic_cr3);

    if (frame->vector == 14) {
        kprintf("Page Fault at 0x%lx (P=%d W=%d U=%d R=%d I=%d)\n",
                panic_cr2,
                (int)(frame->error_code & 0x1),
                (int)((frame->error_code >> 1) & 0x1),
                (int)((frame->error_code >> 2) & 0x1),
                (int)((frame->error_code >> 3) & 0x1),
                (int)((frame->error_code >> 4) & 0x1));
    }

    // RBP-chain stack walk
    extern char _text_start[];
    extern char _text_end[];
    kprintf("Stack trace:\n");
    uint64_t walk_rbp = frame->rbp;
    for (uint32_t depth = 0; depth < 20; depth++) {
        if (walk_rbp == 0 || (walk_rbp & 7) != 0) break;
        if (!vmm_is_kernel_addr(walk_rbp)) break;

        uint64_t *fp = (uint64_t *)walk_rbp;
        uint64_t saved_rbp = fp[0];
        uint64_t ret_addr  = fp[1];

        bool in_text = (ret_addr >= (uint64_t)_text_start && ret_addr < (uint64_t)_text_end);
        kprintf("  #%u  %016lx%s\n", depth, ret_addr, in_text ? "" : "  [!]");

        if (saved_rbp <= walk_rbp || saved_rbp == 0) break;
        walk_rbp = saved_rbp;
    }

    kprintf("====================================================================\n");
    kprintf("System halted.\n");

    // Halt all cores via IPI_PANIC
    if (g_amp.multicore_active) {
        for (uint8_t c = 0; c < g_amp.total_cores; c++) {
            if (c == amp_get_core_index()) continue;
            if (!g_amp.cores[c].online) continue;
            lapic_send_ipi(g_amp.cores[c].lapic_id, IPI_PANIC_VECTOR);
        }
    }

    while (1) {
        asm volatile("cli; hlt");
    }
}

void irq_handler(interrupt_frame_t* frame) {
    uint8_t vector = frame->vector;

    // LAPIC spurious vector: do NOT send EOI
    if (vector == LAPIC_SPURIOUS_VECTOR) {
        return;
    }

    // LAPIC timer vector: per-core tick counter + scheduling.
    // K-Cores run kcore_run_loop — schedule() would hijack them.
    if (vector == LAPIC_TIMER_VECTOR) {
        scheduler_state_t* s = scheduler_get_state();
        s->total_ticks++;
        if (amp_is_appcore()) {
            schedule(frame);
        }
        lapic_send_eoi();
        return;
    }

    // IPI_WAKE: On App Cores, reschedule to pick up newly-woken processes.
    // On K-Cores, just ACK — the interrupt breaks HLT in kcore_run_loop.
    if (vector == IPI_WAKE_VECTOR) {
        if (amp_is_appcore()) {
            schedule(frame);
        }
        lapic_send_eoi();
        return;
    }

    if (vector == IPI_SHOOTDOWN_VECTOR) {
        vmm_tlb_shootdown_handler();
        lapic_send_eoi();
        return;
    }

    if (vector == IPI_PANIC_VECTOR) {
        lapic_send_eoi();
        while (1) { asm volatile("cli; hlt"); }
    }

    // Standard hardware IRQ (vectors 32-55 -> IRQ 0-23)
    uint8_t irq = irq_vector_to_gsi(vector);
    if (irq >= IRQ_MAX_COUNT) {
        // Unknown vector outside our IRQ range — send EOI to prevent stuck interrupts
        irqchip_send_eoi(0);
        return;
    }
    irq_count[irq]++;

    if (irq_callbacks[irq] != NULL) {
        irq_callbacks[irq]();
        irqchip_send_eoi(irq);
        return;
    }

    switch (irq) {
        case 0: {
            // Timer IRQ (PIT via PIC or IO-APIC GSI 0)
            scheduler_state_t* sched = scheduler_get_state();
            sched->total_ticks++;

            /* xHCI events handled via IRQ; poll only as fallback */
            extern void xhci_poll_events(void);
            xhci_poll_events();

            // K-Cores run kcore_run_loop — schedule() would hijack them.
            if (amp_is_appcore()) {
                schedule(frame);
            }
            break;
        }

        case 1: {
            uint8_t scancode = inb(0x60);
            keyboard_handle_scancode(scancode);
            break;
        }

        case 14: {
            ata_dma_irq_handler();
            break;
        }

        default:
            break;
    }

    irqchip_send_eoi(irq);
}

// ---------------------------------------------------------------------------
// Syscall dispatch: sync (single-core) vs async (multi-core)
// ---------------------------------------------------------------------------

// Sync path: process Pockets immediately on the calling core (N=1).
static void sync_syscall_dispatch(process_t* proc, interrupt_frame_t* frame) {
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
static void async_syscall_dispatch(process_t* proc, interrupt_frame_t* frame) {
    (void)frame;  // not needed — process continues running

    // CAS: only submit if not already queued (dedup)
    if (atomic_cas_u8(&proc->kcore_pending, 0, 1)) {
        kcore_submit(proc);
    }
    // Return to userspace — iretq restores frame, process keeps running.
}

// Function pointer set at boot — never changes at runtime.
static void (*g_syscall_dispatch)(process_t*, interrupt_frame_t*) = sync_syscall_dispatch;

void idt_set_syscall_mode(bool multicore) {
    if (multicore) {
        g_syscall_dispatch = async_syscall_dispatch;
        debug_printf("[IDT] Syscall mode: ASYNC (multi-core, non-blocking)\n");
    } else {
        g_syscall_dispatch = sync_syscall_dispatch;
        debug_printf("[IDT] Syscall mode: SYNC (single-core)\n");
    }
}

// ---------------------------------------------------------------------------
// syscall_handler — unified entry point for SYSCALL and INT 0x80
// ---------------------------------------------------------------------------

void syscall_handler(interrupt_frame_t* frame) {
    scheduler_state_t* sched_state = scheduler_get_state();

    spin_lock(&sched_state->scheduler_lock);
    process_t* proc = sched_state->current_process;
    if (!proc || proc->magic != PROCESS_MAGIC) {
        spin_unlock(&sched_state->scheduler_lock);
        frame->rax = (uint64_t)-1;
        return;
    }
    spin_unlock(&sched_state->scheduler_lock);

    frame->rax = 0;

    // Check if this is a yield (cooperative scheduling hint).
    // Yield pockets skip guide() — the process stays WORKING and gives up its
    // timeslice.  It remains schedulable so it will run again on the next tick.
    PocketRing* pring = (PocketRing*)vmm_phys_to_virt(proc->pocket_ring_phys);
    Pocket* peek = pocket_ring_peek(pring);

    if (peek && (peek->flags & POCKET_FLAG_YIELD)) {
        pocket_ring_pop(pring);
        context_save_from_frame(proc, frame);
        schedule(frame);
        return;
    }

    // Dispatch: sync blocks + schedules, async returns immediately.
    g_syscall_dispatch(proc, frame);
}
