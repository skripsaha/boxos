#include "idle.h"
#include "process.h"
#include "klib.h"
#include "gdt.h"
#include "pmm.h"
#include "vmm.h"
#include "ktypes.h"

// Idle process structure (minimal, not in process list)
static process_t g_idle_process;

// External assembly function
extern void idle_loop(void);

void idle_process_init(void) {
    memset(&g_idle_process, 0, sizeof(process_t));

    g_idle_process.magic = PROCESS_MAGIC;
    g_idle_process.pid = IDLE_PID;
    g_idle_process.state = PROC_READY;
    g_idle_process.ref_count = 0;
    g_idle_process.result_there = false;
    g_idle_process.score = -1000;  // Lowest priority

    // Set tags
    strcpy(g_idle_process.tags, "idle");

    // Initialize spinlock
    spinlock_init(&g_idle_process.state_lock);

    // BUG #18 FIX: Use PMM for page-aligned stack (consistent with process.c)
    void* stack_phys = pmm_alloc(1);  // 1 page = 4KB
    if (!stack_phys) {
        debug_printf("[IDLE] CRITICAL: Failed to allocate idle stack\n");
        while (1) { asm volatile("cli; hlt"); }  // Halt system
    }
    void* stack_virt = (void*)stack_phys;  // Identity mapped in kernel space

    // Set up context to run idle_loop in kernel mode
    g_idle_process.context.rip = (uint64_t)idle_loop;
    g_idle_process.context.rsp = (uint64_t)stack_virt + 4096;  // Stack grows down
    g_idle_process.context.rbp = g_idle_process.context.rsp;
    g_idle_process.context.rflags = 0x202;  // IF=1 (interrupts enabled)
    g_idle_process.context.cs = GDT_KERNEL_CODE;
    g_idle_process.context.ss = GDT_KERNEL_DATA;
    g_idle_process.context.ds = GDT_KERNEL_DATA;
    g_idle_process.context.es = GDT_KERNEL_DATA;

    // Use kernel CR3 (no page table switch needed)
    __asm__ volatile("mov %%cr3, %0" : "=r"(g_idle_process.context.cr3));

    // No Cabin, no Notify/Result pages (kernel mode only)
    g_idle_process.cabin = NULL;
    g_idle_process.notify_page_phys = 0;
    g_idle_process.result_page_phys = 0;
    g_idle_process.kernel_stack_top = (void*)((uint64_t)stack_virt + 4096);

    // Not part of process list (don't set ->next)
    g_idle_process.next = NULL;

    debug_printf("[IDLE] Idle process initialized (PID 0)\n");
    debug_printf("[IDLE]   RIP: 0x%lx (idle_loop)\n", g_idle_process.context.rip);
    debug_printf("[IDLE]   RSP: 0x%lx\n", g_idle_process.context.rsp);
    debug_printf("[IDLE]   CR3: 0x%lx (kernel)\n", g_idle_process.context.cr3);
}

process_t* idle_process_get(void) {
    return &g_idle_process;
}

bool process_is_idle(process_t* proc) {
    return (proc && proc->pid == IDLE_PID);
}
