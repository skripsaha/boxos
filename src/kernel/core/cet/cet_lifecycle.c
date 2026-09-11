
#include "cet_lifecycle.h"
#include "klib.h"
#include "cpuid.h"
#include "vmm.h"
#include "pmm.h"
#include "fpu.h"
#include "touch.h"
#include "logbook.h"
#include "process.h"
#include "per_core.h"
#include "amp.h"

#define VMM_MSR_IA32_INTERRUPT_SSP_TABLE_ADDR   0x6A8U

#define CET_SUPV_IST_LEVELS         5

#define CET_SUPV_TOKEN_MODE_BIT     0x1ULL

static inline void cet_wrssq8(uint64_t val, uintptr_t addr) {
    __asm__ volatile(
        "movq %0, %%rax\n\t"
        "movq %1, %%rbx\n\t"
        ".byte 0x48, 0x0f, 0x38, 0xf6, 0x03\n\t"
        :
        : "r"(val), "r"(addr)
        : "rax", "rbx", "memory"
    );
}

static inline void cet_invlpg(uintptr_t addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}


#define CET_MSR_SH_STK_EN        (1ULL <<  0)
#define CET_MSR_WR_SHSTK_EN      (1ULL <<  1)
#define CET_MSR_ENDBR_EN         (1ULL <<  2)
#define CET_MSR_LEG_IW_EN        (1ULL <<  3)
#define CET_MSR_NO_TRACK_EN      (1ULL <<  4)
#define CET_MSR_SUPPRESS_DIS     (1ULL <<  5)
#define CET_MSR_SUPPRESS         (1ULL << 10)
#define CET_MSR_TRACKER_IDLE     (1ULL << 11)

#define CET_S_POLICY_BITS \
    (CET_MSR_WR_SHSTK_EN | CET_MSR_ENDBR_EN | CET_MSR_NO_TRACK_EN)
#define CET_U_POLICY_BITS \
    (CET_MSR_SH_STK_EN | CET_MSR_ENDBR_EN | CET_MSR_NO_TRACK_EN)

#define CET_USER_SSP_PAGES        4u
#define CET_USER_SSP_SIZE         (CET_USER_SSP_PAGES * PMM_PAGE_SIZE)


static volatile bool g_cet_initialized = false;
static volatile bool g_cet_enabled     = false;
static volatile bool g_shstk_active    = false;
static volatile bool g_ibt_active      = false;
static volatile bool g_xsave_cet_s     = false;
static volatile bool g_xsave_cet_u     = false;

volatile uint8_t g_cet_supv_active __attribute__((aligned(8))) = 0;

static volatile uint64_t g_stat_cp_faults  = 0;
static volatile uint64_t g_stat_ssp_allocs = 0;
static volatile uint64_t g_stat_ssp_frees  = 0;

static TouchTag g_tag_enabled       = TOUCH_TAG_INVALID;
static TouchTag g_tag_shstk_active  = TOUCH_TAG_INVALID;
static TouchTag g_tag_ibt_active    = TOUCH_TAG_INVALID;


static inline uint64_t cet_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cet_wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFFULL);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t cet_read_cr4(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void cet_write_cr4(uint64_t v) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}


static bool cet_program_msrs(const char *who) {
    uint64_t s_policy = 0;
    uint64_t u_policy = 0;
    if (g_cpu_caps.has_shstk) {
        s_policy |= CET_MSR_WR_SHSTK_EN;
        u_policy |= CET_MSR_SH_STK_EN;
    }
    if (g_cpu_caps.has_ibt) {
        s_policy |= CET_MSR_ENDBR_EN | CET_MSR_NO_TRACK_EN;
        u_policy |= CET_MSR_ENDBR_EN | CET_MSR_NO_TRACK_EN;
    }
    if (s_policy == 0 && u_policy == 0) {
        debug_printf("[CET/%s] no SHSTK/IBT — MSR program skipped\n", who);
        return false;
    }

    uint64_t cr4 = cet_read_cr4();
    if (!(cr4 & VMM_CR4_CET_BIT)) {
        cr4 |= VMM_CR4_CET_BIT;
        cet_write_cr4(cr4);
    }

    cet_wrmsr(VMM_MSR_IA32_S_CET, s_policy);
    cet_wrmsr(VMM_MSR_IA32_U_CET, u_policy);

    if (g_cpu_caps.has_shstk) g_shstk_active = true;
    if (g_cpu_caps.has_ibt)   g_ibt_active   = true;
    g_cet_enabled = true;

    debug_printf("[CET/%s] CR4.CET=1, S_CET=0x%016lx U_CET=0x%016lx "
                 "(SHSTK=%d IBT=%d)\n",
                 who, (unsigned long)s_policy, (unsigned long)u_policy,
                 (int)g_cpu_caps.has_shstk, (int)g_cpu_caps.has_ibt);
    return true;
}


error_t cet_lifecycle_init_bsp(void) {
    if (g_cet_initialized) return OK;
    if (!g_cpu_caps.has_shstk && !g_cpu_caps.has_ibt) {
        debug_printf("[CET] BSP: no SHSTK/IBT advertised — lifecycle dormant\n");
        g_cet_initialized = true;
        return ERR_UNSUPPORTED;
    }

    g_tag_enabled      = TouchLogbookIntern("cet:enabled");
    g_tag_shstk_active = TouchLogbookIntern("shstk:enabled");
    g_tag_ibt_active   = TouchLogbookIntern("ibt:enabled");

    if (g_cpu_caps.has_shstk) {
        bool s_ok = fpu_xsave_register_extension(VMM_XCR0_CET_S_BIT, "CET_S");
        bool u_ok = fpu_xsave_register_extension(VMM_XCR0_CET_U_BIT, "CET_U");
        g_xsave_cet_s = s_ok;
        g_xsave_cet_u = u_ok;
        debug_printf("[CET] BSP: XSAVE registered CET_S=%d CET_U=%d\n",
                     (int)s_ok, (int)u_ok);
    }

    (void)cet_program_msrs("BSP");

    uint8_t pad[64] = {0};
    if (g_tag_enabled != TOUCH_TAG_INVALID) {
        TouchPublishId(g_tag_enabled, pad, sizeof(pad), 0u, 0u);
    }
    if (g_shstk_active && g_tag_shstk_active != TOUCH_TAG_INVALID) {
        TouchPublishId(g_tag_shstk_active, pad, sizeof(pad), 0u, 0u);
    }
    if (g_ibt_active && g_tag_ibt_active != TOUCH_TAG_INVALID) {
        TouchPublishId(g_tag_ibt_active, pad, sizeof(pad), 0u, 0u);
    }

    g_cet_initialized = true;
    return OK;
}


void cet_lifecycle_init_ap(void) {
    if (!g_cet_initialized) return;
    if (!g_cpu_caps.has_shstk && !g_cpu_caps.has_ibt) return;
    (void)cet_program_msrs("AP");
}


error_t cet_process_create(struct process_t *proc) {
    if (!proc) return ERR_INVALID_ARGUMENT;
    if (!g_cet_enabled || !g_cpu_caps.has_shstk) {
        process_set_user_ssp(proc, 0, 0, 0);
        return OK;
    }

    vmm_context_t *ctx = proc->cabin ? proc->cabin->vmm : NULL;
    if (!ctx) return ERR_INVALID_STATE;

    void *ssp_phys = pmm_alloc_zero(CET_USER_SSP_PAGES, PHYS_TAG_USER);
    if (!ssp_phys) ssp_phys = pmm_alloc_zero(CET_USER_SSP_PAGES);
    if (!ssp_phys) {
        debug_printf("[CET] WARN proc=%u SSP alloc failed (%u pages) "
                     "— process runs without SHSTK\n",
                     proc->pid, CET_USER_SSP_PAGES);
        process_set_user_ssp(proc, 0, 0, 0);
        return OK;
    }

    uintptr_t user_ssp_va = process_user_ssp_va_for(proc);
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                     VMM_FLAG_USER    | VMM_PTE_CET_SS_USER;
    for (uint32_t i = 0; i < CET_USER_SSP_PAGES; i++) {
        vmm_map_result_t r = vmm_map_page(ctx,
                                            user_ssp_va + i * PMM_PAGE_SIZE,
                                            (uintptr_t)ssp_phys + i * PMM_PAGE_SIZE,
                                            flags);
        if (!r.success) {
            for (uint32_t k = 0; k < i; k++) {
                vmm_unmap_page(ctx, user_ssp_va + k * PMM_PAGE_SIZE);
            }
            pmm_free(ssp_phys, CET_USER_SSP_PAGES);
            debug_printf("[CET] WARN proc=%u SSP map failed at page %u "
                         "— process runs without SHSTK\n",
                         proc->pid, i);
            process_set_user_ssp(proc, 0, 0, 0);
            return OK;
        }
    }

    uintptr_t ssp_initial = user_ssp_va + CET_USER_SSP_SIZE - 8;
    process_set_user_ssp(proc, (uintptr_t)ssp_phys, ssp_initial, CET_USER_SSP_SIZE);

    __atomic_fetch_add(&g_stat_ssp_allocs, 1, __ATOMIC_RELAXED);
    debug_printf("[CET] proc=%u SSP allocated: phys=0x%lx va=0x%lx ssp=0x%lx (%u B) "
                 "guards={hi=0x%lx lo=0x%lx unmapped}\n",
                 proc->pid, (unsigned long)(uintptr_t)ssp_phys,
                 (unsigned long)user_ssp_va, (unsigned long)ssp_initial,
                 CET_USER_SSP_SIZE,
                 (unsigned long)process_user_ssp_guard_hi_for(proc),
                 (unsigned long)process_user_ssp_guard_lo_for(proc));
    return OK;
}

void cet_process_destroy(struct process_t *proc) {
    if (!proc) return;
    uintptr_t phys = process_get_user_ssp_phys(proc);
    if (phys == 0) return;
    vmm_context_t *ctx = proc->cabin ? proc->cabin->vmm : NULL;
    uintptr_t va = process_get_user_ssp_va(proc) & ~(uintptr_t)0xFFF;
    uintptr_t va_base = (process_get_user_ssp_va(proc) + 8) - CET_USER_SSP_SIZE;
    if (ctx) {
        for (uint32_t i = 0; i < CET_USER_SSP_PAGES; i++) {
            vmm_unmap_page(ctx, va_base + i * PMM_PAGE_SIZE);
        }
    }
    pmm_free((void *)phys, CET_USER_SSP_PAGES);
    process_set_user_ssp(proc, 0, 0, 0);
    __atomic_fetch_add(&g_stat_ssp_frees, 1, __ATOMIC_RELAXED);
    (void)va;
}

bool cet_is_enabled(void) { return g_cet_enabled; }


void cet_load_user_ssp_for_iretq(struct process_t *proc) {
    if (!proc) return;
    if (!g_cet_enabled) return;
    if (!g_cpu_caps.has_shstk) return;
    uintptr_t ssp = process_get_user_ssp_va(proc);
    if (ssp == 0) return;
    if ((ssp & 0x7u) != 0) {
        debug_printf("[CET] proc=%u SSP misaligned (0x%lx) — skipping WRMSR\n",
                     proc->pid, (unsigned long)ssp);
        return;
    }
    cet_wrmsr(VMM_MSR_IA32_PL3_SSP, (uint64_t)ssp);
}


void cet_lifecycle_get_stats(cet_lifecycle_stats_t *out) {
    if (!out) return;
    out->enabled       = g_cet_enabled;
    out->shstk_active  = g_shstk_active;
    out->ibt_active    = g_ibt_active;
    out->xsave_cet_s   = g_xsave_cet_s;
    out->xsave_cet_u   = g_xsave_cet_u;
    out->s_cet_msr     = g_cet_enabled ? cet_rdmsr(VMM_MSR_IA32_S_CET) : 0;
    out->u_cet_msr     = g_cet_enabled ? cet_rdmsr(VMM_MSR_IA32_U_CET) : 0;
    out->cp_faults     = __atomic_load_n(&g_stat_cp_faults,  __ATOMIC_RELAXED);
    out->ssp_allocs    = __atomic_load_n(&g_stat_ssp_allocs, __ATOMIC_RELAXED);
    out->ssp_frees     = __atomic_load_n(&g_stat_ssp_frees,  __ATOMIC_RELAXED);
}

void cet_lifecycle_dump(void) {
    cet_lifecycle_stats_t s;
    cet_lifecycle_get_stats(&s);
    debug_printf("[CET] stats: enabled=%d SHSTK=%d IBT=%d XSAVE{S=%d U=%d} "
                 "S_CET=0x%lx U_CET=0x%lx cp_faults=%lu ssp{alloc=%lu free=%lu}\n",
                 (int)s.enabled, (int)s.shstk_active, (int)s.ibt_active,
                 (int)s.xsave_cet_s, (int)s.xsave_cet_u,
                 (unsigned long)s.s_cet_msr, (unsigned long)s.u_cet_msr,
                 (unsigned long)s.cp_faults,
                 (unsigned long)s.ssp_allocs,
                 (unsigned long)s.ssp_frees);
}

void cet_lifecycle_record_cp_fault(void) {
    __atomic_fetch_add(&g_stat_cp_faults, 1, __ATOMIC_RELAXED);
}


static error_t cet_supv_alloc_rollback(void *pl0_phys, void *isst_phys,
                                        void *ist_phys[CET_SUPV_IST_LEVELS],
                                        int allocated_ist_count)
{
    for (int i = 0; i < allocated_ist_count; i++) {
        if (ist_phys[i]) pmm_free(ist_phys[i], 1);
    }
    if (isst_phys) pmm_free(isst_phys, 1);
    if (pl0_phys)  pmm_free(pl0_phys, 1);
    return ERR_NO_MEMORY;
}

error_t cet_lifecycle_init_supervisor_ssp(uint8_t core_idx)
{
    if (!g_cet_enabled || !g_cpu_caps.has_shstk) return ERR_UNSUPPORTED;
    if (core_idx >= MAX_CORES)                   return ERR_INVALID_ARGUMENT;

    PerCoreData *pc = &g_per_core[core_idx];
    if (pc->cet_supv_ready) return OK;

    void *pl0_phys  = pmm_alloc_zero(1);
    if (!pl0_phys) return ERR_NO_MEMORY;

    void *isst_phys = pmm_alloc_zero(1);
    if (!isst_phys) {
        void *empty[CET_SUPV_IST_LEVELS] = {0};
        return cet_supv_alloc_rollback(pl0_phys, NULL, empty, 0);
    }

    void *ist_phys[CET_SUPV_IST_LEVELS] = {0};
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        ist_phys[i] = pmm_alloc_zero(1);
        if (!ist_phys[i]) {
            return cet_supv_alloc_rollback(pl0_phys, isst_phys, ist_phys, i);
        }
    }

    uintptr_t pl0_va  = (uintptr_t)vmm_phys_to_virt((uintptr_t)pl0_phys);
    uintptr_t isst_va = (uintptr_t)vmm_phys_to_virt((uintptr_t)isst_phys);
    uintptr_t ist_va[CET_SUPV_IST_LEVELS];
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        ist_va[i] = (uintptr_t)vmm_phys_to_virt((uintptr_t)ist_phys[i]);
    }
    if (!pl0_va || !isst_va) {
        return cet_supv_alloc_rollback(pl0_phys, isst_phys, ist_phys,
                                       CET_SUPV_IST_LEVELS);
    }

    uintptr_t pl0_top = pl0_va + (PMM_PAGE_SIZE - 8);
    *(volatile uint64_t *)pl0_top = (uint64_t)pl0_top | CET_SUPV_TOKEN_MODE_BIT;

    uintptr_t ist_top[CET_SUPV_IST_LEVELS];
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        ist_top[i] = ist_va[i] + (PMM_PAGE_SIZE - 8);
        *(volatile uint64_t *)ist_top[i] =
            (uint64_t)ist_top[i] | CET_SUPV_TOKEN_MODE_BIT;
    }

    vmm_context_t *kctx = vmm_get_kernel_context();
    if (kctx) {
        pte_t *p = vmm_get_or_create_pte(kctx, pl0_va);
        if (p) *p |= VMM_PTE_CET_SS_SUPV;
        cet_invlpg(pl0_va);
        for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
            p = vmm_get_or_create_pte(kctx, ist_va[i]);
            if (p) *p |= VMM_PTE_CET_SS_SUPV;
            cet_invlpg(ist_va[i]);
        }
    }

    uint64_t *table = (uint64_t *)isst_va;
    table[0] = (uint64_t)pl0_top;
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        table[1 + i] = (uint64_t)ist_top[i];
    }
    table[6] = 0;
    table[7] = 0;

    cet_wrmsr(VMM_MSR_IA32_PL0_SSP, (uint64_t)pl0_top);
    cet_wrmsr(VMM_MSR_IA32_INTERRUPT_SSP_TABLE_ADDR, (uint64_t)isst_va);

    pc->pl0_ssp_phys     = (uintptr_t)pl0_phys;
    pc->pl0_ssp_top_va   = pl0_top;
    pc->isst_phys        = (uintptr_t)isst_phys;
    pc->isst_va          = isst_va;
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        pc->ist_ssp_phys[i]   = (uintptr_t)ist_phys[i];
        pc->ist_ssp_top_va[i] = ist_top[i];
    }
    pc->cet_supv_ready = true;

    debug_printf("[CET/core %u] supervisor SSP infra ready: PL0=0x%lx ISST=0x%lx "
                 "(IST 1..5 SSPs allocated, S_CET.SH_STK_EN remains 0 — dormant)\n",
                 core_idx, (unsigned long)pl0_top, (unsigned long)isst_va);
    return OK;
}

void cet_lifecycle_release_supervisor_ssp(uint8_t core_idx)
{
    if (core_idx >= MAX_CORES) return;
    PerCoreData *pc = &g_per_core[core_idx];
    if (!pc->cet_supv_ready) return;

    cet_wrmsr(VMM_MSR_IA32_PL0_SSP, 0);
    cet_wrmsr(VMM_MSR_IA32_INTERRUPT_SSP_TABLE_ADDR, 0);

    if (pc->pl0_ssp_phys) pmm_free((void *)pc->pl0_ssp_phys, 1);
    if (pc->isst_phys)    pmm_free((void *)pc->isst_phys, 1);
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        if (pc->ist_ssp_phys[i]) pmm_free((void *)pc->ist_ssp_phys[i], 1);
    }

    pc->pl0_ssp_phys = 0;
    pc->pl0_ssp_top_va = 0;
    pc->isst_phys = 0;
    pc->isst_va = 0;
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        pc->ist_ssp_phys[i] = 0;
        pc->ist_ssp_top_va[i] = 0;
    }
    pc->cet_supv_ready = false;
}


error_t cet_process_create_kernel_ssp(struct process_t *proc, uintptr_t entry_rip)
{
    if (!proc) return ERR_INVALID_ARGUMENT;
    if (!g_cet_enabled || !g_cpu_caps.has_shstk) {
        proc->kernel_ssp_phys = 0;
        proc->kernel_ssp_va_top = 0;
        proc->context.pl0_ssp = 0;
        return OK;
    }

    void *phys = pmm_alloc_zero(1);
    if (!phys) return ERR_NO_MEMORY;

    uintptr_t va = (uintptr_t)vmm_phys_to_virt((uintptr_t)phys);
    if (!va) {
        pmm_free(phys, 1);
        return ERR_NO_MEMORY;
    }
    uintptr_t top = va + (PMM_PAGE_SIZE - 8);

    *(volatile uint64_t *)top = (uint64_t)top | CET_SUPV_TOKEN_MODE_BIT;

    vmm_context_t *kctx = vmm_get_kernel_context();
    if (kctx) {
        pte_t *p = vmm_get_or_create_pte(kctx, va);
        if (p) *p |= VMM_PTE_CET_SS_SUPV;
        cet_invlpg(va);
    }

    cet_wrssq8((uint64_t)entry_rip, top - 8);

    proc->kernel_ssp_phys   = (uintptr_t)phys;
    proc->kernel_ssp_va_top = top;
    proc->context.pl0_ssp   = top - 8;

    debug_printf("[CET] proc=%u kernel SSP allocated: phys=0x%lx top=0x%lx "
                 "pl0_ssp=0x%lx (entry_rip=0x%lx pre-pushed)\n",
                 proc->pid, (unsigned long)(uintptr_t)phys,
                 (unsigned long)top, (unsigned long)(top - 8),
                 (unsigned long)entry_rip);
    return OK;
}

void cet_process_destroy_kernel_ssp(struct process_t *proc)
{
    if (!proc) return;
    if (!proc->kernel_ssp_phys) return;
    pmm_free((void *)proc->kernel_ssp_phys, 1);
    proc->kernel_ssp_phys = 0;
    proc->kernel_ssp_va_top = 0;
    proc->context.pl0_ssp = 0;
}


__attribute__((noreturn))
void cet_supv_shstk_activate_and_jump(void (*target)(void))
{
    if (!g_cet_enabled || !g_cpu_caps.has_shstk) {
        target();
        for (;;) __asm__ volatile("cli; hlt");
    }

    __asm__ volatile(
        "movb $1, g_cet_supv_active(%%rip)\n\t"
        "movl $0x6A2, %%ecx\n\t"
        "rdmsr\n\t"
        "orl  $1, %%eax\n\t"
        "wrmsr\n\t"
        "jmpq *%0\n\t"
        :
        : "r"(target)
        : "rax", "rcx", "rdx", "memory"
    );
    __builtin_unreachable();
}