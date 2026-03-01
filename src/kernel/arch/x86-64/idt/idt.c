#include "idt.h"
#include "gdt.h"
#include "tss.h"
#include "klib.h"
#include "io.h"
#include "pic.h"
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

static idt_entry_t idt[IDT_ENTRIES];
static idt_descriptor_t idt_desc;

static irq_callback_t irq_callbacks[16] = {NULL};

static uint64_t exception_count = 0;
static uint64_t irq_count[16] = {0};

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
            case 2:   // NMI
                ist = IST_NMI;
                break;
            case 8:   // Double Fault
                ist = IST_DOUBLE_FAULT;
                break;
            case 18:  // Machine Check
                ist = IST_MACHINE_CHECK;
                break;
        }

        idt_set_entry(i, (uint64_t)isr_table[i], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, ist);
    }

    debug_printf("[IDT] Setting up IRQ handlers (32-47)...\n");

    for (int i = 32; i < 48; i++) {
        idt_set_entry(i, (uint64_t)isr_table[i], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    }

    debug_printf("[IDT] Setting up syscall handler (INT 0x80)...\n");

    // Syscall handler at 0x80 (Ring 3 accessible)
    idt_set_entry(SYSCALL_VECTOR, (uint64_t)isr_table[SYSCALL_VECTOR], GDT_KERNEL_CODE, IDT_TYPE_USER_INTERRUPT, 0);

    idt_load_asm((uint64_t)&idt_desc);

    debug_printf("[IDT] IDT loaded successfully at 0x%lx (limit=%d)\n", idt_desc.base, idt_desc.limit);
}

void irq_register_handler(uint8_t irq, irq_callback_t callback) {
    if (irq >= 16) {
        debug_printf("[IDT] ERROR: Invalid IRQ %u (must be 0-15)\n", irq);
        return;
    }

    if (irq_callbacks[irq] != NULL) {
        debug_printf("[IDT] WARNING: Overwriting existing handler for IRQ %u\n", irq);
    }

    irq_callbacks[irq] = callback;
    debug_printf("[IDT] Registered callback for IRQ %u\n", irq);
}

void irq_unregister_handler(uint8_t irq) {
    if (irq >= 16) {
        debug_printf("[IDT] ERROR: Invalid IRQ %u (must be 0-15)\n", irq);
        return;
    }

    irq_callbacks[irq] = NULL;
    debug_printf("[IDT] Unregistered callback for IRQ %u\n", irq);
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

    if ((frame->cs & 3) == 3) {
        process_t* proc = process_get_current();
        if (proc) {
            kprintf("\n");
            kprintf("Faulting Process:\n");
            kprintf("  PID: %u\n", proc->pid);
            kprintf("  State: %d\n", proc->state);
            kprintf("  Tags: %s\n", proc->tags);
        }
    }

    kprintf("\n");
    kprintf("System halted.\n");
    kprintf("====================================================================\n");

    while (1) {
        asm volatile("cli; hlt");
    }
}

void irq_handler(interrupt_frame_t* frame) {
    uint8_t irq = frame->vector - 32;
    if (irq >= 16) return;
    irq_count[irq]++;

    if (irq_callbacks[irq] != NULL) {
        irq_callbacks[irq]();
        pic_send_eoi(irq);
        return;
    }

    switch (irq) {
        case 0: {
            scheduler_state_t* sched = scheduler_get_state();
            sched->total_ticks++;

            extern void xhci_process_events(void);
            xhci_process_events();

            process_t* current = sched->current_process;

            if (current && process_get_state(current) == PROC_RUNNING) {
                scheduler_yield_from_interrupt(frame);
            } else if (!current || process_get_state(current) != PROC_RUNNING) {
                process_t* next = scheduler_select_next();
                if (next) {
                    sched->current_process = next;
                    process_set_state(next, PROC_RUNNING);
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

    pic_send_eoi(irq);
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
    if (state != PROC_RUNNING && state != PROC_BLOCKED) {
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
        process_set_state(proc, PROC_READY);
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

        proc->block_reason = PROC_BLOCK_EVENT_RING_FULL;
        proc->block_start_time = rdtsc();

        guide_block_on_event_ring(proc);

        process_set_state(proc, PROC_BLOCKED);
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
    process_set_state(proc, PROC_BLOCKED);

    // Decrement before scheduler_yield to avoid ref leak if process is destroyed
    process_ref_dec(proc);

    scheduler_yield_from_interrupt(frame);
}
