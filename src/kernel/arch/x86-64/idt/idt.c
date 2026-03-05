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
#include "events.h"
#include "notify_page.h"
#include "vmm.h"
#include "atomics.h"
#include "scheduler.h"
#include "context_switch.h"
#include "keyboard.h"
#include "ata_dma.h"
#include "idle.h"

static idt_entry_t idt[IDT_ENTRIES];
static idt_descriptor_t idt_desc;

static irq_callback_t irq_callbacks[IRQ_MAX_COUNT] = {NULL};

static uint64_t exception_count = 0;
static uint64_t irq_count[IRQ_MAX_COUNT] = {0};

static void idt_load_asm(uint64_t idt_desc_addr) {
    asm volatile("lidt (%0)" : : "r" (idt_desc_addr) : "memory");
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
    process_t* proc = process_get_first();
    while (proc) {
        if (proc->magic == PROCESS_MAGIC && proc->kernel_stack_guard_base) {
            uintptr_t guard_start = (uintptr_t)proc->kernel_stack_guard_base;
            uintptr_t guard_end = guard_start + (CONFIG_KERNEL_STACK_GUARD_PAGES * CONFIG_PAGE_SIZE);
            if (rsp >= guard_start && rsp < guard_end) {
                return proc;
            }
        }
        proc = proc->next;
    }
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
            kprintf("[EXCEPTION] Tags: %s\n", proc->tags);

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

            // Let the timer IRQ do cleanup; force a reschedule via the frame
            scheduler_yield_from_interrupt(frame);
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
            kprintf("  PID: %u  Tags: %s\n", overflow_proc->pid, overflow_proc->tags);
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
            // so scheduler_yield_from_interrupt can acquire it without deadlock.
            // We use spin_force_release (NOT spin_unlock) because we're on an
            // IST stack — the original holder's saved_flags are meaningless here.
            scheduler_state_t* sched = scheduler_get_state();
            if (sched->scheduler_lock.locked) {
                spin_force_release(&sched->scheduler_lock);
            }
            __asm__ volatile("cli");

            scheduler_yield_from_interrupt(frame);
            return;
        }
    }

    // Kernel-mode exception: this is a real kernel panic
    kprintf("\n");
    kprintf("====================================================================\n");
    kprintf("KERNEL PANIC: Exception #%u\n", frame->vector);
    kprintf("====================================================================\n");
    kprintf("Error code: 0x%lx\n", frame->error_code);
    kprintf("RIP: 0x%lx\n", frame->rip);
    kprintf("RSP: 0x%lx\n", frame->rsp);
    kprintf("RFLAGS: 0x%lx\n", frame->rflags);
    kprintf("CS: 0x%lx  SS: 0x%lx\n", frame->cs, frame->ss);
    kprintf("Exception count: %lu\n", exception_count);

    if (frame->vector == 14) {
        uint64_t fault_addr;
        __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));
        kprintf("Page Fault Address (CR2): 0x%lx\n", fault_addr);
        kprintf("Page Fault Error Code:\n");
        kprintf("  Present: %s\n", (frame->error_code & 0x1) ? "YES" : "NO");
        kprintf("  Write: %s\n", (frame->error_code & 0x2) ? "YES" : "NO");
        kprintf("  User: %s\n", (frame->error_code & 0x4) ? "YES" : "NO");
        kprintf("  Reserved: %s\n", (frame->error_code & 0x8) ? "YES" : "NO");
        kprintf("  Instruction Fetch: %s\n", (frame->error_code & 0x10) ? "YES" : "NO");
    }

    kprintf("\n");
    kprintf("System halted.\n");
    kprintf("====================================================================\n");

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

    // LAPIC timer vector: handle as timer tick
    if (vector == LAPIC_TIMER_VECTOR) {
        scheduler_state_t* sched = scheduler_get_state();
        sched->total_ticks++;

        extern void xhci_process_events(void);
        xhci_process_events();

        spin_lock(&sched->scheduler_lock);
        process_t* current = sched->current_process;
        spin_unlock(&sched->scheduler_lock);

        if (current && process_get_state(current) == PROC_WORKING) {
            scheduler_yield_from_interrupt(frame);
        } else if (!current || process_get_state(current) != PROC_WORKING) {
            process_t* next = scheduler_select_next();
            if (next) {
                spin_lock(&sched->scheduler_lock);
                sched->current_process = next;
                spin_unlock(&sched->scheduler_lock);

                if (process_get_state(next) == PROC_CREATED) process_set_state(next, PROC_WORKING);
                next->last_run_time = sched->total_ticks;

                tss_set_rsp0((uint64_t)next->kernel_stack_top);
                context_restore_to_frame(next, frame);
            }
        }

        lapic_send_eoi();
        return;
    }

    // Standard hardware IRQ (vectors 32-55 -> GSI 0-23)
    uint8_t irq = irq_vector_to_gsi(vector);
    if (irq >= IRQ_MAX_COUNT) return;
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

            extern void xhci_process_events(void);
            xhci_process_events();

            spin_lock(&sched->scheduler_lock);
            process_t* current = sched->current_process;
            spin_unlock(&sched->scheduler_lock);

            if (current && process_get_state(current) == PROC_WORKING) {
                scheduler_yield_from_interrupt(frame);
            } else if (!current || process_get_state(current) != PROC_WORKING) {
                process_t* next = scheduler_select_next();
                if (next) {
                    spin_lock(&sched->scheduler_lock);
                    sched->current_process = next;
                    spin_unlock(&sched->scheduler_lock);

                    if (process_get_state(next) == PROC_CREATED) process_set_state(next, PROC_WORKING);
                    next->last_run_time = sched->total_ticks;

                    tss_set_rsp0((uint64_t)next->kernel_stack_top);
                    context_restore_to_frame(next, frame);
                }
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

void syscall_handler(interrupt_frame_t* frame) {
    scheduler_state_t* sched_state = scheduler_get_state();

    spin_lock(&sched_state->scheduler_lock);
    process_t* proc = sched_state->current_process;

    if (!proc) {
        spin_unlock(&sched_state->scheduler_lock);
        frame->rax = (uint64_t)-1;
        return;
    }

    // Increment ref_count before releasing lock to prevent proc from being destroyed
    process_ref_inc(proc);

    if (proc->magic != PROCESS_MAGIC) {
        process_ref_dec(proc);
        spin_unlock(&sched_state->scheduler_lock);
        frame->rax = (uint64_t)-1;
        return;
    }

    spin_unlock(&sched_state->scheduler_lock);

    process_state_t state = process_get_state(proc);
    if (state != PROC_WORKING && state != PROC_WAITING) {
        process_ref_dec(proc);
        frame->rax = (uint64_t)-1;
        return;
    }

    uint64_t notify_phys = proc->notify_page_phys;
    if (!notify_phys) {
        process_ref_dec(proc);
        frame->rax = (uint64_t)-1;
        return;
    }

    notify_page_t* notify_virt = (notify_page_t*)vmm_phys_to_virt(notify_phys);
    if (!notify_virt) {
        process_ref_dec(proc);
        frame->rax = (uint64_t)-1;
        return;
    }

    uint32_t magic = notify_virt->magic;
    if (magic != NOTIFY_PAGE_MAGIC) {
        process_ref_dec(proc);
        frame->rax = (uint64_t)-1;
        return;
    }

    if (notify_virt->flags & NOTIFY_FLAG_YIELD) {
        notify_virt->status = NOTIFY_STATUS_OK;
        frame->rax = 0;

        context_save_from_frame(proc, frame);
        process_ref_dec(proc);

        scheduler_yield_from_interrupt(frame);
        return;
    }

    uint8_t prefix_count = notify_virt->prefix_count;
    if (prefix_count > NOTIFY_MAX_PREFIXES) {
        process_ref_dec(proc);
        frame->rax = (uint64_t)-1;
        return;
    }

    // Clamp prefix_count to EVENT_MAX_PREFIXES (truncation is intentional)
    // If notify_virt has more prefixes than EVENT_MAX_PREFIXES, extras are silently dropped
    if (prefix_count > EVENT_MAX_PREFIXES) {
        prefix_count = EVENT_MAX_PREFIXES;
    }

    Event event;
    event_init(&event, proc->pid, guide_alloc_event_id());
    event.prefix_count = prefix_count;
    event.current_prefix_idx = 0;
    event.timestamp = rdtsc();

    for (uint8_t i = 0; i < prefix_count; i++) {
        event.prefixes[i] = notify_virt->prefixes[i];
    }

    // Add null terminator if there's space (after truncation)
    if (prefix_count < EVENT_MAX_PREFIXES) {
        event.prefixes[prefix_count] = 0x0000;
    }

    size_t data_copy_size = EVENT_DATA_SIZE;
    if (NOTIFY_DATA_SIZE < data_copy_size) {
        data_copy_size = NOTIFY_DATA_SIZE;
    }
    memcpy(event.data, notify_virt->data, data_copy_size);

    event.route_target = notify_virt->route_target;
    memcpy(event.route_tag, notify_virt->route_tag, 32);
    event.route_tag[31] = '\0';

    event_push_result_t result = event_ring_push_priority(
        kernel_event_ring,
        &event,
        EVENT_PRIORITY_USER  // User notify() calls
    );

    if (result == EVENT_PUSH_FULL_BLOCKED) {
        notify_virt->status = NOTIFY_STATUS_RING_FULL;
        notify_virt->flags |= NOTIFY_FLAG_CHECK_STATUS;
        __sync_synchronize();

        context_save_from_frame(proc, frame);

        proc->wait_reason = WAIT_RING_FULL;
        proc->wait_start_time = rdtsc();

        guide_wait_on_event_ring(proc);

        process_set_state(proc, PROC_WAITING);
        process_ref_dec(proc);

        scheduler_yield_from_interrupt(frame);
        return;
    }

    notify_virt->status = NOTIFY_STATUS_OK;
    notify_virt->flags &= ~NOTIFY_FLAG_CHECK_STATUS;
    __sync_synchronize();

    guide_wake();

    frame->rax = event.event_id;

    context_save_from_frame(proc, frame);
    process_set_state(proc, PROC_WAITING);

    // Decrement before scheduler_yield to avoid ref leak if process is destroyed
    process_ref_dec(proc);

    scheduler_yield_from_interrupt(frame);
}
