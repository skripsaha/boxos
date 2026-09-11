#include "idle.h"
#include "boardroom.h"
#include "xhci_hub.h"
#include "hardware_deck.h"
#include "xhci_msd.h"
#include "xhci_enumeration.h"
#include "process.h"
#include "nightwatch.h"
#include "klib.h"
#include "gdt.h"
#include "pmm.h"
#include "vmm.h"
#include "ktypes.h"
#include "kernel_config.h"
#include "amp.h"
#include "baton.h"
#include "fpu.h"
#include "cpuid.h"
#include "cpu_calibrate.h"
#include "tagfs.h"

static process_t g_idle_process;

static process_t* g_core_idle[MAX_CORES];

static void idle_setup(process_t* idle, uint8_t core_index) {
    memset(idle, 0, sizeof(process_t));

    idle->magic = PROCESS_MAGIC;
    idle->pid = IDLE_PID;
    idle->state = PROC_WORKING;
    idle->ref_count = 0;
    idle->score = -1000;
    idle->home_core = core_index;
    idle->on_cpu = -1;

    spinlock_init(&idle->state_lock);

    void* stack_phys = pmm_alloc(CONFIG_KERNEL_STACK_TOTAL_PAGES);
    if (!stack_phys) {
        kprintf("[IDLE] FATAL: Failed to allocate idle stack for core %u\n", core_index);
        while (1) { asm volatile("cli; hlt"); }
    }
    void* stack_virt = vmm_phys_to_virt((uintptr_t)stack_phys);

    vmm_context_t* kernel_ctx = vmm_get_kernel_context();
    pte_t* guard_pte = vmm_get_or_create_pte(kernel_ctx, (uintptr_t)stack_virt);
    if (guard_pte) {
        *guard_pte = 0;
        vmm_shootdown_page(kernel_ctx, (uintptr_t)stack_virt);
    }

    idle->kernel_stack_guard_base = stack_virt;
    idle->kernel_stack = (void*)((uintptr_t)stack_virt + CONFIG_PAGE_SIZE);
    idle->kernel_stack_top = (void*)((uintptr_t)idle->kernel_stack + CONFIG_KERNEL_STACK_PAGES * CONFIG_PAGE_SIZE);

    idle->context.rip = (uint64_t)idle_loop;
    idle->context.rsp = (uint64_t)idle->kernel_stack_top;
    idle->context.rbp = idle->context.rsp;
    idle->context.rflags = 0x202;
    idle->context.cs = GDT_KERNEL_CODE;
    idle->context.ss = GDT_KERNEL_DATA;
    idle->context.ds = GDT_KERNEL_DATA;
    idle->context.es = GDT_KERNEL_DATA;

    __asm__ volatile("mov %%cr3, %0" : "=r"(idle->context.cr3));

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
    for (int i = 0; i < MAX_CORES; i++) {
        g_core_idle[i] = NULL;
    }

    idle_setup(&g_idle_process, g_amp.bsp_index);
    g_core_idle[g_amp.bsp_index] = &g_idle_process;

    debug_printf("[IDLE] BSP idle process (core %u, PID 0)\n", g_amp.bsp_index);
    debug_printf("[IDLE]   Stack: 0x%lx-0x%lx\n",
                 (uintptr_t)g_idle_process.kernel_stack,
                 (uintptr_t)g_idle_process.kernel_stack_top);
}

void idle_process_init_core(uint8_t core_index) {
    if (g_core_idle[core_index]) return;

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
    kprintf("[IDLE] FATAL: No idle process for core %u — init order bug\n", idx);
    while (1) { asm volatile("cli; hlt"); }
    __builtin_unreachable();
}

bool process_is_idle(process_t* proc) {
    return (proc && proc->pid == IDLE_PID);
}

void cpu_idle(void) {
    nightwatch_core_idle(amp_get_core_index());

    BatonPump(amp_get_core_index());

    cpu_tsc_recal_if_pending();

    xhci_hub_service_if_pending();

    xhci_slot_service_if_pending();

    xhci_recover_if_needed();

    HardwareDeckUsbRecoverProof();

    xhci_msd_watchdog();

    BoardroomAttendIfPending();

    TagFSServiceIfPending();

    if (g_cpu_caps.has_monitor) {
        uint32_t mwait_eax = 0x00;
        (void)g_cpu_caps.has_arat;

        volatile uint8_t monitor_cell;
        __asm__ volatile(".byte 0x0f,0x01,0xc8"
                         :
                         : "a"(&monitor_cell), "c"(0), "d"(0)
                         : "memory");
        __asm__ volatile("sti; .byte 0x0f,0x01,0xc9"
                         :
                         : "a"(mwait_eax), "c"(0u)
                         : "memory");
    } else {
        __asm__ volatile("sti; hlt");
    }
}