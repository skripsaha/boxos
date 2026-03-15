#include "idle.h"
#include "process.h"
#include "klib.h"
#include "gdt.h"
#include "pmm.h"
#include "vmm.h"
#include "ktypes.h"
#include "kernel_config.h"
#include "amp.h"
#include "fpu.h"

// BSP idle process (static, PID 0)
static process_t g_idle_process;

// Per-core idle process table.
// BSP (index 0) points to &g_idle_process.
// AP cores point to dynamically allocated idle processes.
static process_t* g_core_idle[MAX_CORES];

extern void idle_loop(void);

// Common helper: set up a process_t as an idle process with its own kernel stack.
static void idle_setup(process_t* idle, uint8_t core_index) {
    memset(idle, 0, sizeof(process_t));

    idle->magic = PROCESS_MAGIC;
    idle->pid = IDLE_PID;
    idle->state = PROC_WORKING;
    idle->ref_count = 0;
    idle->score = -1000;
    idle->home_core = core_index;

    spinlock_init(&idle->state_lock);

    // 2 pages: 1 guard (unmapped) + 1 data
    void* stack_phys = pmm_alloc(2);
    if (!stack_phys) {
        kprintf("[IDLE] FATAL: Failed to allocate idle stack for core %u\n", core_index);
        while (1) { asm volatile("cli; hlt"); }
    }
    void* stack_virt = vmm_phys_to_virt((uintptr_t)stack_phys);

    // Unmap guard page
    vmm_context_t* kernel_ctx = vmm_get_kernel_context();
    pte_t* guard_pte = vmm_get_or_create_pte(kernel_ctx, (uintptr_t)stack_virt);
    if (guard_pte) {
        *guard_pte = 0;
        vmm_flush_tlb_page((uintptr_t)stack_virt);
    }

    idle->kernel_stack_guard_base = stack_virt;
    idle->kernel_stack = (void*)((uintptr_t)stack_virt + CONFIG_PAGE_SIZE);
    idle->kernel_stack_top = (void*)((uintptr_t)idle->kernel_stack + CONFIG_PAGE_SIZE);

    idle->context.rip = (uint64_t)idle_loop;
    idle->context.rsp = (uint64_t)idle->kernel_stack_top;
    idle->context.rbp = idle->context.rsp;
    idle->context.rflags = 0x202;  // IF=1
    idle->context.cs = GDT_KERNEL_CODE;
    idle->context.ss = GDT_KERNEL_DATA;
    idle->context.ds = GDT_KERNEL_DATA;
    idle->context.es = GDT_KERNEL_DATA;

    __asm__ volatile("mov %%cr3, %0" : "=r"(idle->context.cr3));

    // Allocate FPU state buffer so context_save_from_frame doesn't fault
    uint32_t fpu_buf = fpu_alloc_size();
    idle->context.fpu_state = kmalloc(fpu_buf);
    if (idle->context.fpu_state) {
        fpu_init_state(idle->context.fpu_state);
    }
    idle->context.fpu_initialized = false;

    idle->cabin = NULL;
    idle->next = NULL;
}

void idle_process_init(void) {
    // Clear table
    for (int i = 0; i < MAX_CORES; i++) {
        g_core_idle[i] = NULL;
    }

    // BSP idle process
    idle_setup(&g_idle_process, g_amp.bsp_index);
    g_core_idle[g_amp.bsp_index] = &g_idle_process;

    debug_printf("[IDLE] BSP idle process (core %u, PID 0)\n", g_amp.bsp_index);
    debug_printf("[IDLE]   Stack: 0x%lx-0x%lx\n",
                 (uintptr_t)g_idle_process.kernel_stack,
                 (uintptr_t)g_idle_process.kernel_stack_top);
}

void idle_process_init_core(uint8_t core_index) {
    if (g_core_idle[core_index]) return;  // already initialized

    process_t* idle = kmalloc(sizeof(process_t));
    if (!idle) {
        kprintf("[IDLE] FATAL: Failed to allocate idle struct for core %u\n", core_index);
        while (1) { asm volatile("cli; hlt"); }
    }

    idle_setup(idle, core_index);
    g_core_idle[core_index] = idle;

    debug_printf("[IDLE] Core %u idle process ready (stack 0x%lx-0x%lx)\n",
                 core_index,
                 (uintptr_t)idle->kernel_stack,
                 (uintptr_t)idle->kernel_stack_top);
}

process_t* idle_process_get(void) {
    uint8_t idx = amp_get_core_index();
    process_t* idle = g_core_idle[idx];
    if (idle) return idle;
    // Fallback to BSP idle (should never happen if init order is correct)
    return &g_idle_process;
}

bool process_is_idle(process_t* proc) {
    return (proc && proc->pid == IDLE_PID);
}
