#include "per_core.h"
#include "amp.h"
#include "lapic.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "irqchip.h"
#include "fpu.h"
#include "cpuid.h"
#include "atomics.h"
#include "pvclock.h"
#include "cpu_calibrate.h"
#include "pit.h"
#include "cpu_caps_page.h"
#include "memtag.h"
#include "mce.h"

PerCoreData g_per_core[MAX_CORES] __attribute__((aligned(64)));
volatile bool g_per_core_active = false;

volatile uint64_t g_bsp_tsc_anchor     = 0;
volatile uint64_t g_bsp_tsc_anchor_us  = 0;
volatile uint64_t g_bsp_tsc_anchor_khz = 0;

#define AP_TSC_SKEW_THRESHOLD_US   4ULL

void per_core_record_bsp_tsc_anchor(uint64_t now_us)
{
    extern uint64_t cpu_get_tsc_freq_khz(void);
    uint64_t khz_now = cpu_get_tsc_freq_khz();
    __atomic_store_n(&g_bsp_tsc_anchor_us,  now_us,  __ATOMIC_RELAXED);
    __atomic_store_n(&g_bsp_tsc_anchor_khz, khz_now, __ATOMIC_RELAXED);
    __atomic_store_n(&g_bsp_tsc_anchor,     rdtsc(), __ATOMIC_RELEASE);
}

#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_SFMASK          0xC0000084
#define MSR_GS_BASE         0xC0000101
#define MSR_KERNEL_GS_BASE  0xC0000102
#define MSR_IA32_TSC        0x00000010
#define MSR_IA32_TSC_ADJUST 0x0000003B

#define EFER_SCE            (1ULL << 0)
#define EFER_NXE            (1ULL << 11)
#define SFMASK_VALUE        ((1ULL <<  9)  | \
                             (1ULL <<  8)  | \
                             (1ULL << 10)  | \
                             (1ULL << 14)  | \
                             (1ULL << 16)  | \
                             (1ULL << 18) )

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


static void per_core_gdt_set_tss(gdt_entry_t* gdt, tss_t* tss) {
    uint64_t base  = (uint64_t)tss;
    uint64_t limit = sizeof(tss_t) - 1;

    gdt[5].limit_low   = limit & 0xFFFF;
    gdt[5].base_low    = base & 0xFFFF;
    gdt[5].base_middle = (base >> 16) & 0xFF;
    gdt[5].access      = 0x89;
    gdt[5].granularity = (limit >> 16) & 0x0F;
    gdt[5].base_high   = (base >> 24) & 0xFF;

    uint64_t* entry6 = (uint64_t*)&gdt[6];
    *entry6 = (base >> 32);
}

static void per_core_load_gdt(gdt_descriptor_t* desc) {
    __asm__ volatile (
        "lgdt (%0)\n\t"
        "subq $16, %%rsp\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, (%%rsp)\n\t"
        "movw %w1, 8(%%rsp)\n\t"
        ".byte 0x48, 0xff, 0x2c, 0x24\n\t"
        "1:\n\t"
        "addq $16, %%rsp\n\t"
        "movw %w2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
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

        pte_t* guard_pte = vmm_get_or_create_pte(kctx, (uintptr_t)virt_base);
        if (!guard_pte) {
            kprintf("[PER_CORE] FATAL: IST%d guard page PTE failed for core %u\n",
                    i + 1, core_index);
            while (1) { __asm__ volatile("cli; hlt"); }
        }
        *guard_pte = 0;
        vmm_shootdown_page(kctx, (uintptr_t)virt_base);

        uint64_t stack_top = (uint64_t)virt_base + (total_pages * 4096) - 16;

        switch (i) {
            case 0: tss->ist1 = stack_top; break;
            case 1: tss->ist2 = stack_top; break;
            case 2: tss->ist3 = stack_top; break;
            case 3: tss->ist4 = stack_top; break;
            case 4: tss->ist5 = stack_top; break;
        }
    }
}

static void per_core_load_gs(PerCoreData* pc) {
    pc->notify.self       = (uint64_t)&pc->notify;
    pc->notify.core_index = pc->core_index;
    wrmsr_pc(MSR_GS_BASE, (uint64_t)&pc->notify);
    wrmsr_pc(MSR_KERNEL_GS_BASE, 0);
}


static void per_core_setup_notify_msrs(PerCoreData* pc) {
    uint64_t efer = rdmsr_pc(MSR_EFER);
    efer |= EFER_SCE;
    if (g_cpu_caps.has_nx) efer |= EFER_NXE;
    wrmsr_pc(MSR_EFER, efer);

    uint64_t star = ((uint64_t)GDT_KERNEL_DATA << 48) |
                    ((uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr_pc(MSR_STAR, star);

    wrmsr_pc(MSR_LSTAR, (uint64_t)notify_entry);

    wrmsr_pc(MSR_SFMASK, SFMASK_VALUE);

    pc->notify.kernel_rsp = pc->kernel_stack_top;
    pc->notify.user_rsp   = 0;
    pc->notify.self        = (uint64_t)&pc->notify;
}


void per_core_init_bsp(void) {
    uint8_t bsp_idx = g_amp.bsp_index;
    PerCoreData* pc = &g_per_core[bsp_idx];

    kprintf("[PER_CORE] Initializing BSP per-core data (core %u)...\n", bsp_idx);

    memset(pc, 0, sizeof(PerCoreData));
    pc->core_index = bsp_idx;
    pc->lapic_id   = g_amp.bsp_lapic_id;
    pc->is_kcore   = g_amp.cores[bsp_idx].is_kcore;

    per_core_load_gs(pc);

    gdt_copy_entries(pc->gdt, PER_CORE_GDT_ENTRIES);

    tss_t* bsp_tss = tss_get_ptr();
    memcpy(&pc->tss, bsp_tss, sizeof(tss_t));

    pc->kernel_stack_top = pc->tss.rsp0;

    per_core_gdt_set_tss(pc->gdt, &pc->tss);

    pc->gdt_desc.limit = sizeof(pc->gdt) - 1;
    pc->gdt_desc.base  = (uint64_t)&pc->gdt;

    per_core_load_gdt(&pc->gdt_desc);

    per_core_load_tss();

    per_core_setup_notify_msrs(pc);

    pc->initialized = true;
    __atomic_store_n(&g_per_core_active, true, __ATOMIC_RELEASE);

    kprintf("[PER_CORE] BSP ready: GDT=0x%lx TSS=0x%lx KernelGSBASE=0x%lx\n",
            (uint64_t)&pc->gdt, (uint64_t)&pc->tss, (uint64_t)&pc->notify);
}

void per_core_init_ap(uint8_t core_index, uint64_t stack_top) {
    cpu_intersect_features_ap();

    cpu_caps_page_refresh_features();

    if (!g_cpu_caps.has_fsgsbase) g_fsgsbase_active = 0;

    (void)MemTagVerifyPteMetadataBits();

    cpu_umwait_control_init(cpu_get_tsc_freq_khz());

    cpu_test_ctl_init();

    PerCoreData* pc = &g_per_core[core_index];

    memset(pc, 0, sizeof(PerCoreData));
    pc->core_index       = core_index;
    pc->lapic_id         = g_amp.cores[core_index].lapic_id;
    pc->is_kcore         = g_amp.cores[core_index].is_kcore;
    pc->kernel_stack_top = stack_top;
    uint64_t stack_top_page = (stack_top + CONFIG_PAGE_SIZE - 1) &
                              ~((uint64_t)CONFIG_PAGE_SIZE - 1);
    pc->kernel_stack_floor = stack_top_page -
                             (uint64_t)CONFIG_KERNEL_STACK_PAGES * CONFIG_PAGE_SIZE;

    per_core_load_gs(pc);

    PerCoreData* bsp = &g_per_core[g_amp.bsp_index];
    memcpy(pc->gdt, bsp->gdt, sizeof(gdt_entry_t) * 5);

    memset(&pc->tss, 0, sizeof(tss_t));
    pc->tss.rsp0       = stack_top;
    pc->tss.iomap_base = sizeof(tss_t);

    per_core_alloc_ist(&pc->tss, core_index);

    per_core_gdt_set_tss(pc->gdt, &pc->tss);

    pc->gdt_desc.limit = sizeof(pc->gdt) - 1;
    pc->gdt_desc.base  = (uint64_t)&pc->gdt;

    per_core_load_gdt(&pc->gdt_desc);

    per_core_load_tss();

    enable_fpu();

    vmm_pat_init();

    (void)MemTagVerifyPatMsr();

    mce_ap_init();

    vmm_pku_ap_init();

    vmm_lam_ap_probe();

    vmm_tme_ap_probe();

    vmm_cet_ap_probe();

    {
        extern void cet_lifecycle_init_ap(void);
        cet_lifecycle_init_ap();
    }

    {
        extern error_t cet_lifecycle_init_supervisor_ssp(uint8_t);
        (void)cet_lifecycle_init_supervisor_ssp(core_index);
    }

    if (vmm_pcid_active()) {
        uint64_t cr4_pcid;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4_pcid));
        cr4_pcid |= (1ULL << 17);
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4_pcid) : "memory");
    }

    if (g_cpu_caps.has_tsc_adjust && g_bsp_tsc_anchor != 0) {
        uint64_t bsp_anchor_us  = __atomic_load_n(&g_bsp_tsc_anchor_us,  __ATOMIC_RELAXED);
        uint64_t bsp_anchor_khz = __atomic_load_n(&g_bsp_tsc_anchor_khz, __ATOMIC_RELAXED);
        uint64_t bsp_anchor     = __atomic_load_n(&g_bsp_tsc_anchor,     __ATOMIC_ACQUIRE);
        uint64_t now_us         = pit_get_uptime_us();

        if (bsp_anchor != 0 && bsp_anchor_khz != 0 && now_us > bsp_anchor_us) {
            uint64_t elapsed_us  = now_us - bsp_anchor_us;
            uint64_t expected    = bsp_anchor + (elapsed_us * bsp_anchor_khz / 1000ULL);
            uint64_t my_tsc      = rdtsc();
            uint64_t cur_adj     = rdmsr_pc(MSR_IA32_TSC_ADJUST);

            uint64_t threshold_cyc = (bsp_anchor_khz * AP_TSC_SKEW_THRESHOLD_US) / 1000ULL;
            if (threshold_cyc < 1000ULL) threshold_cyc = 1000ULL;

            int64_t skew     = (int64_t)(my_tsc - expected);
            int64_t abs_skew = skew < 0 ? -skew : skew;
            if ((uint64_t)abs_skew > threshold_cyc) {
                uint64_t new_adj = cur_adj - (uint64_t)skew;
                wrmsr_pc(MSR_IA32_TSC_ADJUST, new_adj);
                debug_printf("[PER_CORE] Core %u TSC skew %ld cycles "
                             "corrected via IA32_TSC_ADJUST (was 0x%lx → 0x%lx, threshold=%lu)\n",
                             core_index, (long)skew,
                             (unsigned long)cur_adj, (unsigned long)new_adj,
                             (unsigned long)threshold_cyc);
            }
        }
    }

    pvclock_init_ap(core_index);

    per_core_setup_notify_msrs(pc);

    lapic_enable();

    if (pc->is_kcore) {
        lapic_timer_stop();
    } else {
        lapic_timer_init(LAPIC_TIMER_VECTOR, 100);
    }

    pc->initialized = true;

    kprintf("[PER_CORE] Core %u ready: GDT=0x%lx TSS=0x%lx timer=%s %s\n",
            core_index, (uint64_t)&pc->gdt, (uint64_t)&pc->tss,
            pc->is_kcore ? "masked" : "100Hz",
            pc->is_kcore ? "[K-Core]" : "[App Core]");
}

void per_core_set_kernel_rsp(uint64_t top, uint64_t floor) {
    if (!__atomic_load_n(&g_per_core_active, __ATOMIC_ACQUIRE)) {
        tss_set_rsp0(top);
        notify_set_kernel_rsp(top);
        return;
    }

    uint8_t idx = amp_get_core_index();
    PerCoreData* pc = &g_per_core[idx];
    pc->tss.rsp0           = top;
    pc->notify.kernel_rsp  = top;
    pc->kernel_stack_top   = top;
    pc->kernel_stack_floor = floor;
}