#include "idle.h"
#include "process.h"
#include "klib.h"
#include "gdt.h"
#include "pmm.h"
#include "vmm.h"
#include "ktypes.h"
#include "kernel_config.h"

static process_t g_idle_process;

extern void idle_loop(void);

void idle_process_init(void) {
    memset(&g_idle_process, 0, sizeof(process_t));

    g_idle_process.magic = PROCESS_MAGIC;
    g_idle_process.pid = IDLE_PID;
    g_idle_process.state = PROC_WORKING;
    g_idle_process.ref_count = 0;
    g_idle_process.score = -1000;  // lowest priority

    strncpy(g_idle_process.tags, "idle", PROCESS_TAG_SIZE - 1);
    g_idle_process.tags[PROCESS_TAG_SIZE - 1] = '\0';

    spinlock_init(&g_idle_process.state_lock);

    void* stack_phys = pmm_alloc(2);  // 1 guard + 1 data
    if (!stack_phys) {
        debug_printf("[IDLE] CRITICAL: Failed to allocate idle stack\n");
        while (1) { asm volatile("cli; hlt"); }
    }
    void* stack_virt = vmm_phys_to_virt((uintptr_t)stack_phys);

    // Unmap guard page (first page)
    vmm_context_t* kernel_ctx = vmm_get_kernel_context();
    pte_t* guard_pte = vmm_get_or_create_pte(kernel_ctx, (uintptr_t)stack_virt);
    if (guard_pte) {
        *guard_pte = 0;
        vmm_flush_tlb_page((uintptr_t)stack_virt);
    }

    g_idle_process.kernel_stack_guard_base = stack_virt;
    g_idle_process.kernel_stack = (void*)((uintptr_t)stack_virt + CONFIG_PAGE_SIZE);
    g_idle_process.kernel_stack_top = (void*)((uintptr_t)g_idle_process.kernel_stack + CONFIG_PAGE_SIZE);

    g_idle_process.context.rip = (uint64_t)idle_loop;
    g_idle_process.context.rsp = (uint64_t)g_idle_process.kernel_stack_top;
    g_idle_process.context.rbp = g_idle_process.context.rsp;
    g_idle_process.context.rflags = 0x202;  // IF=1
    g_idle_process.context.cs = GDT_KERNEL_CODE;
    g_idle_process.context.ss = GDT_KERNEL_DATA;
    g_idle_process.context.ds = GDT_KERNEL_DATA;
    g_idle_process.context.es = GDT_KERNEL_DATA;

    __asm__ volatile("mov %%cr3, %0" : "=r"(g_idle_process.context.cr3));

    g_idle_process.cabin = NULL;
    g_idle_process.cabin_info_phys = 0;
    g_idle_process.pocket_ring_phys = 0;
    g_idle_process.result_ring_phys = 0;

    g_idle_process.next = NULL;

    debug_printf("[IDLE] Idle process initialized (PID 0)\n");
    debug_printf("[IDLE]   Guard: 0x%lx\n", (uintptr_t)g_idle_process.kernel_stack_guard_base);
    debug_printf("[IDLE]   Stack: 0x%lx-0x%lx\n",
                 (uintptr_t)g_idle_process.kernel_stack,
                 (uintptr_t)g_idle_process.kernel_stack_top);
    debug_printf("[IDLE]   RIP: 0x%lx (idle_loop)\n", g_idle_process.context.rip);
    debug_printf("[IDLE]   CR3: 0x%lx (kernel)\n", g_idle_process.context.cr3);
}

process_t* idle_process_get(void) {
    return &g_idle_process;
}

bool process_is_idle(process_t* proc) {
    return (proc && proc->pid == IDLE_PID);
}
