#include "per_core.h"
#include "amp.h"
#include "lapic.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "irqchip.h"
#include "fpu.h"

PerCoreData g_per_core[MAX_CORES] __attribute__((aligned(64)));
bool g_per_core_active = false;

// ---------------------------------------------------------------------------
// MSR helpers (local to this file)
// ---------------------------------------------------------------------------
#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_SFMASK          0xC0000084
#define MSR_KERNEL_GS_BASE  0xC0000102

#define EFER_SCE            (1ULL << 0)
#define EFER_NXE            (1ULL << 11)
#define SFMASK_VALUE        ((1ULL << 9) | (1ULL << 8) | (1ULL << 10))

static inline uint64_t rdmsr_pc(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr_pc(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr),
                     "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)));
}

// ---------------------------------------------------------------------------
// GDT helpers
// ---------------------------------------------------------------------------

// Write a 16-byte TSS descriptor at GDT entries [5] and [6].
static void per_core_gdt_set_tss(gdt_entry_t* gdt, tss_t* tss) {
    uint64_t base  = (uint64_t)tss;
    uint64_t limit = sizeof(tss_t) - 1;

    // Entry 5: lower half
    gdt[5].limit_low   = limit & 0xFFFF;
    gdt[5].base_low    = base & 0xFFFF;
    gdt[5].base_middle = (base >> 16) & 0xFF;
    gdt[5].access      = 0x89;  // Present, DPL=0, Available 64-bit TSS
    gdt[5].granularity = (limit >> 16) & 0x0F;
    gdt[5].base_high   = (base >> 24) & 0xFF;

    // Entry 6: upper 32 bits of base address
    uint64_t* entry6 = (uint64_t*)&gdt[6];
    *entry6 = (base >> 32);
}

// Load GDT from descriptor pointer, then reload CS via far return and
// all data segment registers.  Identical logic to gdt_load_asm() in gdt.c.
static void per_core_load_gdt(gdt_descriptor_t* desc) {
    __asm__ volatile (
        "lgdt (%0)\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw %w2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss"
        :
        : "r" ((uint64_t)desc),
          "i" ((uint64_t)GDT_KERNEL_CODE),
          "i" ((uint32_t)GDT_KERNEL_DATA)
        : "memory", "rax"
    );
}

static void per_core_load_tss(void) {
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS));
}

// ---------------------------------------------------------------------------
// IST stack allocation (PMM + guard pages)
// ---------------------------------------------------------------------------

static void per_core_alloc_ist(tss_t* tss, uint8_t core_index) {
    vmm_context_t* kctx = vmm_get_kernel_context();

    for (int i = 0; i < IST_COUNT; i++) {
        size_t total_pages = IST_GUARD_PAGES + IST_STACK_PAGES;
        void* phys = pmm_alloc(total_pages);
        if (!phys) {
            kprintf("[PER_CORE] FATAL: IST%d alloc failed for core %u\n",
                    i + 1, core_index);
            while (1) { __asm__ volatile("cli; hlt"); }
        }

        void* virt_base = vmm_phys_to_virt((uintptr_t)phys);

        // Unmap guard page (first page) — access triggers page fault
        pte_t* guard_pte = vmm_get_or_create_pte(kctx, (uintptr_t)virt_base);
        if (!guard_pte) {
            kprintf("[PER_CORE] FATAL: IST%d guard page PTE failed for core %u\n",
                    i + 1, core_index);
            while (1) { __asm__ volatile("cli; hlt"); }
        }
        *guard_pte = 0;
        vmm_flush_tlb_page((uintptr_t)virt_base);

        // Stack grows down: top = base + (guard + data) * PAGE_SIZE - 16
        uint64_t stack_top = (uint64_t)virt_base + (total_pages * 4096) - 16;

        switch (i) {
            case 0: tss->ist1 = stack_top; break;  // Double Fault
            case 1: tss->ist2 = stack_top; break;  // NMI
            case 2: tss->ist3 = stack_top; break;  // Machine Check
            case 3: tss->ist4 = stack_top; break;  // Debug
            case 4: tss->ist5 = stack_top; break;  // Stack Fault
        }
    }
}

// ---------------------------------------------------------------------------
// SYSCALL / Notify MSR setup (per-core)
// ---------------------------------------------------------------------------

static void per_core_setup_notify_msrs(PerCoreData* pc) {
    // EFER: enable SYSCALL + NX
    uint64_t efer = rdmsr_pc(MSR_EFER);
    efer |= EFER_SCE | EFER_NXE;
    wrmsr_pc(MSR_EFER, efer);

    // STAR: kernel CS/SS in [47:32], user base in [63:48]
    //   Entry: CS = STAR[47:32], SS = STAR[47:32]+8
    //   Exit:  SS = STAR[63:48]+8, CS = STAR[63:48]+16
    uint64_t star = ((uint64_t)GDT_KERNEL_DATA << 48) |
                    ((uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr_pc(MSR_STAR, star);

    // LSTAR: SYSCALL entry point
    wrmsr_pc(MSR_LSTAR, (uint64_t)notify_entry);

    // SFMASK: clear IF, TF, DF on SYSCALL entry
    wrmsr_pc(MSR_SFMASK, SFMASK_VALUE);

    // PerCpuData (at offset 0 of PerCoreData)
    pc->notify.kernel_rsp = pc->kernel_stack_top;
    pc->notify.user_rsp   = 0;
    pc->notify.self        = (uint64_t)&pc->notify;

    // KernelGSBASE → PerCpuData so swapgs loads it into GS.base
    wrmsr_pc(MSR_KERNEL_GS_BASE, (uint64_t)&pc->notify);
}

// ===========================================================================
// Public API
// ===========================================================================

void per_core_init_bsp(void) {
    uint8_t bsp_idx = g_amp.bsp_index;
    PerCoreData* pc = &g_per_core[bsp_idx];

    kprintf("[PER_CORE] Initializing BSP per-core data (core %u)...\n", bsp_idx);

    memset(pc, 0, sizeof(PerCoreData));
    pc->core_index = bsp_idx;
    pc->lapic_id   = g_amp.bsp_lapic_id;
    pc->is_kcore   = g_amp.cores[bsp_idx].is_kcore;

    // ---- GDT ----
    // Copy BSP's static GDT entries (segments 0-4 are identical across all cores)
    gdt_copy_entries(pc->gdt, PER_CORE_GDT_ENTRIES);

    // Copy BSP's current TSS (already has dynamic IST stacks from
    // tss_setup_dynamic_stacks(), so we inherit them).
    tss_t* bsp_tss = tss_get_ptr();
    memcpy(&pc->tss, bsp_tss, sizeof(tss_t));

    // Inherit current kernel stack from the static TSS
    pc->kernel_stack_top = pc->tss.rsp0;

    // Point TSS descriptor in per-core GDT to THIS core's TSS.
    // Must set access = 0x89 (available), not 0x8B (busy from prior ltr).
    per_core_gdt_set_tss(pc->gdt, &pc->tss);

    // GDT descriptor
    pc->gdt_desc.limit = sizeof(pc->gdt) - 1;
    pc->gdt_desc.base  = (uint64_t)&pc->gdt;

    // Load per-core GDT (lgdt + reload all segment registers)
    per_core_load_gdt(&pc->gdt_desc);

    // Load per-core TSS (ltr — reads TSS descriptor from the new GDT)
    per_core_load_tss();

    // ---- Notify MSRs + PerCpuData ----
    per_core_setup_notify_msrs(pc);

    pc->initialized = true;
    g_per_core_active = true;

    kprintf("[PER_CORE] BSP ready: GDT=0x%lx TSS=0x%lx KernelGSBASE=0x%lx\n",
            (uint64_t)&pc->gdt, (uint64_t)&pc->tss, (uint64_t)&pc->notify);
}

void per_core_init_ap(uint8_t core_index, uint64_t stack_top) {
    PerCoreData* pc = &g_per_core[core_index];

    memset(pc, 0, sizeof(PerCoreData));
    pc->core_index       = core_index;
    pc->lapic_id         = g_amp.cores[core_index].lapic_id;
    pc->is_kcore         = g_amp.cores[core_index].is_kcore;
    pc->kernel_stack_top = stack_top;

    // ---- GDT ----
    // Copy code/data segments (0-4) from BSP's per-core GDT
    PerCoreData* bsp = &g_per_core[g_amp.bsp_index];
    memcpy(pc->gdt, bsp->gdt, sizeof(gdt_entry_t) * 5);

    // ---- TSS ----
    memset(&pc->tss, 0, sizeof(tss_t));
    pc->tss.rsp0       = stack_top;
    pc->tss.iomap_base = sizeof(tss_t);

    // Allocate per-core IST stacks (Double Fault, NMI, Machine Check, Debug, Stack Fault)
    per_core_alloc_ist(&pc->tss, core_index);

    // Point TSS descriptor to this core's TSS
    per_core_gdt_set_tss(pc->gdt, &pc->tss);

    // GDT descriptor
    pc->gdt_desc.limit = sizeof(pc->gdt) - 1;
    pc->gdt_desc.base  = (uint64_t)&pc->gdt;

    // Load per-core GDT
    per_core_load_gdt(&pc->gdt_desc);

    // Load per-core TSS
    per_core_load_tss();

    // ---- FPU/SSE/AVX ----
    enable_fpu();

    // ---- SYSCALL MSRs + PerCpuData + KernelGSBASE ----
    per_core_setup_notify_msrs(pc);

    // ---- LAPIC ----
    lapic_enable();

    // Calibrate and start LAPIC timer at 100 Hz (periodic)
    lapic_timer_init(LAPIC_TIMER_VECTOR, 100);

    pc->initialized = true;

    kprintf("[PER_CORE] Core %u ready: GDT=0x%lx TSS=0x%lx LAPIC timer=100Hz %s\n",
            core_index, (uint64_t)&pc->gdt, (uint64_t)&pc->tss,
            pc->is_kcore ? "[K-Core]" : "[App Core]");
}

void per_core_set_kernel_rsp(uint64_t rsp) {
    if (!g_per_core_active) {
        // Early boot: use static BSP TSS + PerCpuData
        tss_set_rsp0(rsp);
        notify_set_kernel_rsp(rsp);
        return;
    }

    uint8_t idx = amp_get_core_index();
    PerCoreData* pc = &g_per_core[idx];
    pc->tss.rsp0         = rsp;
    pc->notify.kernel_rsp = rsp;
    pc->kernel_stack_top  = rsp;
}
