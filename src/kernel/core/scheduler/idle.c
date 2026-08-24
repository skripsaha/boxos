#include "idle.h"
#include "xhci_hub.h"
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
#include "fpu.h"
#include "cpuid.h"          // g_cpu_caps.has_monitor (MWAIT idle)
#include "cpu_calibrate.h"  // cpu_tsc_recal_if_pending — periodic TSC recal

// BSP idle process (static, PID 0)
static process_t g_idle_process;

// Per-core idle process table.
// BSP (index 0) points to &g_idle_process.
// AP cores point to dynamically allocated idle processes.
static process_t* g_core_idle[MAX_CORES];

// Common helper: set up a process_t as an idle process with its own kernel stack.
static void idle_setup(process_t* idle, uint8_t core_index) {
    memset(idle, 0, sizeof(process_t));

    idle->magic = PROCESS_MAGIC;
    idle->pid = IDLE_PID;
    idle->state = PROC_WORKING;
    idle->ref_count = 0;
    idle->score = -1000;
    idle->home_core = core_index;
    idle->on_cpu = -1;            /* never claimed (schedule skips idle), kept consistent */

    spinlock_init(&idle->state_lock);

    // CONFIG_KERNEL_STACK_TOTAL_PAGES: 1 guard (unmapped) + CONFIG_KERNEL_STACK_PAGES
    // data — same geometry as every other kernel stack so REACT headroom is uniform.
    void* stack_phys = pmm_alloc(CONFIG_KERNEL_STACK_TOTAL_PAGES);
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
        /* Cross-core shootdown so other cores observe the cleared
         * guard PTE through the freshly demoted 4 KB leaf instead of
         * their cached 1 GB Pull-Map entry. */
        vmm_shootdown_page(kernel_ctx, (uintptr_t)stack_virt);
    }

    idle->kernel_stack_guard_base = stack_virt;
    idle->kernel_stack = (void*)((uintptr_t)stack_virt + CONFIG_PAGE_SIZE);
    idle->kernel_stack_top = (void*)((uintptr_t)idle->kernel_stack + CONFIG_KERNEL_STACK_PAGES * CONFIG_PAGE_SIZE);

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
    kprintf("[IDLE] FATAL: No idle process for core %u — init order bug\n", idx);
    while (1) { asm volatile("cli; hlt"); }
    __builtin_unreachable();
}

bool process_is_idle(process_t* proc) {
    return (proc && proc->pid == IDLE_PID);
}

// One idle wait, executed each iteration of idle_loop (idle_loop.asm).
void cpu_idle(void) {
    /* Nightwatch: this core has nothing to run. Also its re-check point — an
     * idle core wakes on every tick, so no timer of its own is needed. */
    nightwatch_core_idle(amp_get_core_index());

    /* Periodic TSC recalibration. Runs out of IRQ context so the
     * 20ms HPET measurement window inside is harmless to interrupt
     * latency. Gated by an internal pending flag that's set every
     * TSC_RECAL_INTERVAL_US by the PIT IRQ — most idle iterations
     * see no work to do and return in nanoseconds. Only meaningful
     * on the BSP (PIT IRQ delivery target); App-Core idle calls fall
     * through to the cheap "no work" path because the flag is never
     * set there. */
    cpu_tsc_recal_if_pending();

    /* USB hubs, when one has said something. Finding out what a hub means by
     * "something changed on my ports" takes control transfers, and control
     * transfers have to be waited for — which cannot happen in the interrupt
     * handler that received the report. This is the nearest place that is
     * allowed to wait, and the check is a single atomic load when there is
     * nothing to do, which is almost always. */
    xhci_hub_service_if_pending();

    /* USB devices that have been unplugged. Taking one down means waiting for
     * the controller to confirm it has let go of the device context, and for
     * whatever was mid-transfer to come out — neither of which can be waited
     * for where the unplug was noticed. Same shape as the hubs above, and the
     * same single atomic load when there is nothing to do. */
    xhci_slot_service_if_pending();

    if (g_cpu_caps.has_monitor) {
        /* MWAIT idle. Arm MONITOR on a per-core stack address (each idle
         * process has its own stack), then MWAIT into the deepest
         * SAFE C-state.
         *
         * C-state selection — Intel SDM Vol 3A §10.5.4.1 / §17.16.4:
         *   - C1 always keeps the LAPIC timer ticking.
         *   - C2+ stops the LAPIC timer counter UNLESS ARAT
         *     (CPUID.06H:EAX[2]) is set.
         *
         * Encoding of MWAIT EAX hint:
         *   bits 7:4  target C-state (0 = C0, 1 = C1, 2 = C2, ...)
         *   bits 3:0  sub-C-state index (0 = first sub-state)
         *
         * Policy:
         *   has_arat == 0  →  C1 (0x00) — never deeper, would stop timer
         *   has_arat == 1  →  C2 (0x10) — modest power saving, IPI/timer
         *                     still deliverable, exit latency typically
         *                     under 10 µs.
         *
         * C3+ deliberately not picked: requires PAT/cache flush
         * coordination per SDM §17.16.4.2 and exit latency jumps
         * to ~100 µs — premature for a kernel that targets desktop /
         * server workloads. Revisit for battery targets. */
        /* Default to C1 (0x00) — always safe, LAPIC timer always ticks.
         *
         * C2 (0x10) hint when ARAT available was tried but caused
         * intermittent timing regressions on QEMU TCG -cpu max: TCG's
         * MWAIT emulation latency for C2 wakeup is markedly higher
         * than for C1, enough to time out the stress-matrix command
         * delivery window. Revert until either (a) TCG emulation is
         * not in scope, or (b) we add per-environment idle policy
         * (e.g. detect QEMU TCG vendor + force C1). */
        uint32_t mwait_eax = 0x00;
        (void)g_cpu_caps.has_arat;  /* placeholder for future C2 path */

        volatile uint8_t monitor_cell;
        __asm__ volatile(".byte 0x0f,0x01,0xc8"        /* monitor rax,rcx,rdx */
                         :
                         : "a"(&monitor_cell), "c"(0), "d"(0)
                         : "memory");
        __asm__ volatile("sti; .byte 0x0f,0x01,0xc9"   /* sti; mwait eax,ecx   */
                         :
                         : "a"(mwait_eax), "c"(0u)
                         : "memory");
    } else {
        /* No MWAIT: HLT is C1 and always keeps the LAPIC timer running. */
        __asm__ volatile("sti; hlt");
    }
}
