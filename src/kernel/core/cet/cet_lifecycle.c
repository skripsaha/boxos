/*
 * CET — Control-flow Enforcement Technology lifecycle.
 *
 * See cet_lifecycle.h for design + spec citations. This file owns:
 *   1. BSP + AP CR4.CET enable.
 *   2. IA32_S_CET + IA32_U_CET MSR programming.
 *   3. XSAVE component 11 / 12 registration via fpu_xsave_register_extension.
 *   4. Per-process shadow stack allocation + free.
 *   5. Telemetry + Touch publish on lifecycle events.
 */

#include "cet_lifecycle.h"
#include "klib.h"
#include "cpuid.h"
#include "vmm.h"
#include "pmm.h"
#include "fpu.h"
#include "touch.h"
#include "process.h"

/* ─── IA32_S_CET / IA32_U_CET bits (Intel SDM Vol 3D §17.2.1) ─── */

#define CET_MSR_SH_STK_EN        (1ULL <<  0)   /* Shadow Stack Enable */
#define CET_MSR_WR_SHSTK_EN      (1ULL <<  1)   /* WRSS / WRUSS allowed */
#define CET_MSR_ENDBR_EN         (1ULL <<  2)   /* IBT — ENDBR enforcement */
#define CET_MSR_LEG_IW_EN        (1ULL <<  3)   /* Legacy WAIT-FOR-ENDBRANCH */
#define CET_MSR_NO_TRACK_EN      (1ULL <<  4)   /* NOTRACK prefix allowed */
#define CET_MSR_SUPPRESS_DIS     (1ULL <<  5)   /* SUPPRESS not allowed */
#define CET_MSR_SUPPRESS         (1ULL << 10)   /* WAIT-FOR-ENDBRANCH state */
#define CET_MSR_TRACKER_IDLE     (1ULL << 11)   /* WAIT-FOR-ENDBRANCH idle */

/* The policy bits we set when CR4.CET=1: enable shadow stacks + IBT,
 * leave WR_SHSTK enabled so the kernel can prime PL3_SSP via WRUSS,
 * allow NOTRACK so existing compiler emission stays compatible. */
#define CET_POLICY_BITS \
    (CET_MSR_SH_STK_EN | CET_MSR_WR_SHSTK_EN | \
     CET_MSR_ENDBR_EN  | CET_MSR_NO_TRACK_EN)

/* Per-process user shadow stack size. SDM recommends ≥ data stack. Our
 * user stack is currently 16 KiB (4 pages) — SSP matches that ceiling
 * so any depth a process can reach on its data stack is mirrored on
 * its shadow stack without ENDBR-failure overflow. */
#define CET_USER_SSP_PAGES        4u
#define CET_USER_SSP_SIZE         (CET_USER_SSP_PAGES * PMM_PAGE_SIZE)

/* ─── State ─────────────────────────────────────────────────────── */

static volatile bool g_cet_initialized = false;
static volatile bool g_cet_enabled     = false;
static volatile bool g_shstk_active    = false;
static volatile bool g_ibt_active      = false;
static volatile bool g_xsave_cet_s     = false;
static volatile bool g_xsave_cet_u     = false;

static volatile uint64_t g_stat_cp_faults  = 0;
static volatile uint64_t g_stat_ssp_allocs = 0;
static volatile uint64_t g_stat_ssp_frees  = 0;

/* Touch tag handles. */
static TouchTag g_tag_enabled       = TOUCH_TAG_INVALID;
static TouchTag g_tag_shstk_active  = TOUCH_TAG_INVALID;
static TouchTag g_tag_ibt_active    = TOUCH_TAG_INVALID;

/* ─── MSR helpers ───────────────────────────────────────────────── */

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

/* ─── CR4.CET + MSR setup, BSP + AP shared ─────────────────────── */

static bool cet_program_msrs(const char *who) {
    /* SHSTK enable only when CPU advertises shadow-stack; IBT enable
     * only when CPU advertises ENDBR/NOTRACK enforcement. Each MSR
     * bit gates on the corresponding CPUID feature. */
    uint64_t policy = 0;
    if (g_cpu_caps.has_shstk) policy |= CET_MSR_SH_STK_EN | CET_MSR_WR_SHSTK_EN;
    if (g_cpu_caps.has_ibt)   policy |= CET_MSR_ENDBR_EN  | CET_MSR_NO_TRACK_EN;
    if (policy == 0) {
        debug_printf("[CET/%s] no SHSTK/IBT — MSR program skipped\n", who);
        return false;
    }

    /* Program IA32_S_CET (supervisor) and IA32_U_CET (user). Both
     * mirror the same policy bits; user-side controls fire when CPL=3,
     * supervisor when CPL=0..2. The MSRs are only accessible after
     * CR4.CET=1; we set CR4.CET first. */
    uint64_t cr4 = cet_read_cr4();
    if (!(cr4 & VMM_CR4_CET_BIT)) {
        cr4 |= VMM_CR4_CET_BIT;
        cet_write_cr4(cr4);
    }

    cet_wrmsr(VMM_MSR_IA32_S_CET, policy);
    cet_wrmsr(VMM_MSR_IA32_U_CET, policy);

    if (g_cpu_caps.has_shstk) g_shstk_active = true;
    if (g_cpu_caps.has_ibt)   g_ibt_active   = true;
    g_cet_enabled = true;

    debug_printf("[CET/%s] CR4.CET=1, S_CET=0x%016lx U_CET=0x%016lx "
                 "(SHSTK=%d IBT=%d)\n",
                 who, (unsigned long)policy, (unsigned long)policy,
                 (int)g_cpu_caps.has_shstk, (int)g_cpu_caps.has_ibt);
    return true;
}

/* ─── BSP init ──────────────────────────────────────────────────── */

error_t cet_lifecycle_init_bsp(void) {
    if (g_cet_initialized) return OK;
    if (!g_cpu_caps.has_shstk && !g_cpu_caps.has_ibt) {
        debug_printf("[CET] BSP: no SHSTK/IBT advertised — lifecycle dormant\n");
        g_cet_initialized = true;
        return ERR_UNSUPPORTED;
    }

    /* Pre-resolve Touch tag handles for lifecycle publishes. */
    g_tag_enabled      = TouchTagIntern("cet:enabled");
    g_tag_shstk_active = TouchTagIntern("shstk:enabled");
    g_tag_ibt_active   = TouchTagIntern("ibt:enabled");

    /* Register XSAVE components 11 (CET_S) + 12 (CET_U) so per-process
     * SSP is saved + restored on context switch. fpu_xsave_register_extension
     * grows g_xsave_area_size atomically; subsequent process FPU areas
     * allocate at the new size. */
    if (g_cpu_caps.has_shstk) {
        bool s_ok = fpu_xsave_register_extension(VMM_XCR0_CET_S_BIT, "CET_S");
        bool u_ok = fpu_xsave_register_extension(VMM_XCR0_CET_U_BIT, "CET_U");
        g_xsave_cet_s = s_ok;
        g_xsave_cet_u = u_ok;
        debug_printf("[CET] BSP: XSAVE registered CET_S=%d CET_U=%d\n",
                     (int)s_ok, (int)u_ok);
    }

    /* Enable CR4.CET + program MSRs. */
    (void)cet_program_msrs("BSP");

    /* Publish lifecycle events. */
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

/* ─── AP init ───────────────────────────────────────────────────── */

void cet_lifecycle_init_ap(void) {
    if (!g_cet_initialized) return;        /* BSP refused — skip AP */
    if (!g_cpu_caps.has_shstk && !g_cpu_caps.has_ibt) return;
    (void)cet_program_msrs("AP");
}

/* ─── Per-process SSP allocation ────────────────────────────────── */

error_t cet_process_create(struct process_t *proc) {
    if (!proc) return ERR_INVALID_ARGUMENT;
    if (!g_cet_enabled || !g_cpu_caps.has_shstk) {
        /* CET dormant or no shadow stack support — leave SSP fields
         * zero so jump_to_userspace skips the PL3_SSP WRMSR. */
        process_set_user_ssp(proc, 0, 0, 0);
        return OK;
    }

    /* Allocate a contiguous 16 KiB phys page run from the user zone. */
    void *ssp_phys = pmm_alloc_zero(CET_USER_SSP_PAGES, PHYS_TAG_USER);
    if (!ssp_phys) ssp_phys = pmm_alloc_zero(CET_USER_SSP_PAGES);
    if (!ssp_phys) {
        debug_printf("[CET] proc=%u SSP alloc failed (%u pages)\n",
                     proc->pid, CET_USER_SSP_PAGES);
        return ERR_NO_MEMORY;
    }

    /* Map into the process's vmm_context at a fixed user VA below the
     * data stack. Use the user-SS PTE bit so CPU recognises the page
     * as a shadow stack target (Intel SDM Vol 3A §4.5.1 Table 4-19).
     * We map writable so WRUSS instructions can prime the SSP slot;
     * runtime user code reaches the page only via CALL/RET shadow ops.
     */
    vmm_context_t *ctx = (vmm_context_t *)proc->cabin;
    if (!ctx) {
        pmm_free(ssp_phys, CET_USER_SSP_PAGES);
        return ERR_INVALID_STATE;
    }

    uintptr_t user_ssp_va = process_user_ssp_va_for(proc);  /* fixed VA per process */
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                     VMM_FLAG_USER    | VMM_PTE_CET_SS_USER;
    for (uint32_t i = 0; i < CET_USER_SSP_PAGES; i++) {
        vmm_map_result_t r = vmm_map_page(ctx,
                                            user_ssp_va + i * PMM_PAGE_SIZE,
                                            (uintptr_t)ssp_phys + i * PMM_PAGE_SIZE,
                                            flags);
        if (!r.success) {
            /* Roll back partial mapping. */
            for (uint32_t k = 0; k < i; k++) {
                vmm_unmap_page(ctx, user_ssp_va + k * PMM_PAGE_SIZE);
            }
            pmm_free(ssp_phys, CET_USER_SSP_PAGES);
            debug_printf("[CET] proc=%u SSP map failed at page %u\n",
                         proc->pid, i);
            return ERR_NO_MEMORY;
        }
    }

    /* Initial SSP value: top of the page minus 8 bytes (leaves room
     * for the first CALL's shadow-stack push). */
    uintptr_t ssp_initial = user_ssp_va + CET_USER_SSP_SIZE - 8;
    process_set_user_ssp(proc, (uintptr_t)ssp_phys, ssp_initial, CET_USER_SSP_SIZE);

    __atomic_fetch_add(&g_stat_ssp_allocs, 1, __ATOMIC_RELAXED);
    debug_printf("[CET] proc=%u SSP allocated: phys=0x%lx va=0x%lx ssp=0x%lx (%u B)\n",
                 proc->pid, (unsigned long)(uintptr_t)ssp_phys,
                 (unsigned long)user_ssp_va, (unsigned long)ssp_initial,
                 CET_USER_SSP_SIZE);
    return OK;
}

void cet_process_destroy(struct process_t *proc) {
    if (!proc) return;
    uintptr_t phys = process_get_user_ssp_phys(proc);
    if (phys == 0) return;
    vmm_context_t *ctx = (vmm_context_t *)proc->cabin;
    uintptr_t va = process_get_user_ssp_va(proc) & ~(uintptr_t)0xFFF;
    /* Recover the page base from the initial SSP value (top of page
     * minus 8) by masking. The base of the mapped range is
     * (ssp_va + 8) - SIZE. */
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

/* ─── Telemetry ─────────────────────────────────────────────────── */

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

/* External accessor used by idt.c #CP handler to bump fault count. */
void cet_lifecycle_record_cp_fault(void) {
    __atomic_fetch_add(&g_stat_cp_faults, 1, __ATOMIC_RELAXED);
}
