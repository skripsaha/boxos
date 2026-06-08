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
#include "per_core.h"      /* PerCoreData fields for PL0_SSP infrastructure */
#include "amp.h"           /* MAX_CORES                                       */

/* IA32_INTERRUPT_SSP_TABLE_ADDR — Intel SDM Vol 3D §17.6 Table 17-1.
 * Not yet defined in vmm.h; lives here until a CET MSR header consolidates
 * the supervisor-side definitions alongside the existing PL{0,3}_SSP. */
#define VMM_MSR_IA32_INTERRUPT_SSP_TABLE_ADDR   0x6A8U

/* Number of IST levels BoxOS uses (1..5 → #DF, NMI, #MC, #DB, #SS). The
 * IA32_INTERRUPT_SSP_TABLE_ADDR table has 8 entries — entry 0 mirrors
 * PL0_SSP, entries 1..7 are per-IST. We populate 1..5 here. */
#define CET_SUPV_IST_LEVELS         5

/* Supervisor SSP token mode bit (Intel SDM Vol 1 §17.2.3). Token format:
 *   bits 63:3  — the SSP value (token's own VA, 8-byte aligned)
 *   bit 2:1    — 0 (reserved)
 *   bit 0      — 1 for supervisor token, 0 for user token */
#define CET_SUPV_TOKEN_MODE_BIT     0x1ULL

/* WRSSQ — Write 8 bytes to a supervisor-shadow-stack page.
 *
 * Intel SDM Vol 2 §WRSSQ: writes a 64-bit value to memory whose PTE has
 * the supervisor-SS bit (60) set. CPL=0 + CR4.CET=1 +
 * IA32_S_CET.WR_SHSTK_EN=1 required.
 *
 * Critical: a NORMAL store to a bit-60 page FAULTS — only WRSSQ /
 * WRUSS / SETSSBSY / RSTORSSP are allowed write paths into shadow stack
 * memory. The kernel must use this primitive when initialising supervisor
 * SSP tokens and when pushing the first synthetic entry during the
 * SH_STK_EN activation handshake.
 *
 * Why raw byte encoding: older binutils don't accept the `wrssq`
 * mnemonic; raw bytes are portable. REX.W + 0F 38 F6 /r is the encoding;
 * here we use a r/m memory operand with a register source. */
static inline void cet_wrssq8(uint64_t val, uintptr_t addr) {
    __asm__ volatile(
        /* wrssq %rax, (%rbx) — REX.W=1, opcode 0F 38 F6 /r, ModR/M=03 */
        "movq %0, %%rax\n\t"
        "movq %1, %%rbx\n\t"
        ".byte 0x48, 0x0f, 0x38, 0xf6, 0x03\n\t"  /* wrssq %rax, (%rbx) */
        :
        : "r"(val), "r"(addr)
        : "rax", "rbx", "memory"
    );
}

/* INVLPG — flush a single page's TLB entry on the current CPU. Needed
 * after toggling PTE.bit60 so subsequent accesses see the new attribute
 * (otherwise stale TLB would let normal stores succeed on a page now
 * marked shadow-stack, or reject WRSSQ on a page seen as non-SS). */
static inline void cet_invlpg(uintptr_t addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

/* ─── IA32_S_CET / IA32_U_CET bits (Intel SDM Vol 3D §17.2.1) ─── */

#define CET_MSR_SH_STK_EN        (1ULL <<  0)   /* Shadow Stack Enable */
#define CET_MSR_WR_SHSTK_EN      (1ULL <<  1)   /* WRSS / WRUSS allowed */
#define CET_MSR_ENDBR_EN         (1ULL <<  2)   /* IBT — ENDBR enforcement */
#define CET_MSR_LEG_IW_EN        (1ULL <<  3)   /* Legacy WAIT-FOR-ENDBRANCH */
#define CET_MSR_NO_TRACK_EN      (1ULL <<  4)   /* NOTRACK prefix allowed */
#define CET_MSR_SUPPRESS_DIS     (1ULL <<  5)   /* SUPPRESS not allowed */
#define CET_MSR_SUPPRESS         (1ULL << 10)   /* WAIT-FOR-ENDBRANCH state */
#define CET_MSR_TRACKER_IDLE     (1ULL << 11)   /* WAIT-FOR-ENDBRANCH idle */

/* CET policy — split S/U side per ring needs (Intel SDM Vol 3D §17.2.1).
 *
 * Supervisor (IA32_S_CET): we INTENTIONALLY omit SH_STK_EN. Enabling
 * supervisor shadow stack would require an allocated supervisor SSP per
 * CPU and an IA32_PL0_SSP MSR write before any kernel RET — without
 * those, the very first ring-0 RET (e.g. the one that returns from
 * cet_program_msrs into per_core_init_bsp) fires #CP and the kernel
 * dies. Per-CPU PL0_SSP infrastructure is deferred to a follow-up.
 * What we KEEP supervisor-side is ENDBR_EN + NO_TRACK_EN: IBT defends
 * the kernel against JOP/COP without needing a supervisor shadow stack.
 * WR_SHSTK_EN here lets us execute WRUSS at CPL=0 if a future caller
 * primes a user SSP token (Intel SDM Vol 2B WRUSS — CPL=0 only,
 * IA32_U_CET.SH_STK_EN must be 1; S_CET.WR_SHSTK_EN gates the access).
 *
 * User (IA32_U_CET): full SHSTK + IBT. User code returns through its
 * own per-process shadow stack, indirect calls land on ENDBR64. WRUSS
 * gate is NOT user-accessible — WRUSS is a CPL=0 instruction. We omit
 * WR_SHSTK_EN here so user code cannot execute WRSS to forge its own
 * shadow-stack entries.
 */
#define CET_S_POLICY_BITS \
    (CET_MSR_WR_SHSTK_EN | CET_MSR_ENDBR_EN | CET_MSR_NO_TRACK_EN)
#define CET_U_POLICY_BITS \
    (CET_MSR_SH_STK_EN | CET_MSR_ENDBR_EN | CET_MSR_NO_TRACK_EN)

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

/* Public flag — read from context_switch.asm SAVE_PL0_SSP / RESTORE_PL0_SSP
 * macros. Set to 1 by cet_supv_shstk_activate_and_jump when S_CET.SH_STK_EN
 * flips on (per BSP / per AP). Stays 0 in two cases that the asm path
 * skips MSR access for:
 *   - CPU lacks SHSTK (TCG, AMD Zen 1/2/3, pre-Tiger Lake Intel)
 *   - has_shstk=true but activation hasn't run yet on this CPU
 * Single byte + RELAXED-OR-ZERO semantics: asm does `cmp byte [..], 0` →
 * no atomics needed, the read becomes consistent at the next context
 * switch on this CPU which is always after the activation completes.
 * 8-byte alignment leaves the surrounding 7 bytes safe for unrelated
 * single-byte loads. */
volatile uint8_t g_cet_supv_active __attribute__((aligned(8))) = 0;

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
     * bit gates on the corresponding CPUID feature, AND each side
     * (S/U) uses its own policy mask (see CET_{S,U}_POLICY_BITS). */
    uint64_t s_policy = 0;
    uint64_t u_policy = 0;
    if (g_cpu_caps.has_shstk) {
        /* Supervisor side: never SH_STK_EN (no PL0_SSP) — only the
         * WRUSS gate. User side: full shadow stack. */
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

    /* Program IA32_S_CET (supervisor) and IA32_U_CET (user). The
     * MSRs are only accessible after CR4.CET=1; set CR4.CET first.
     * The MSR access ordering matters under hardware that strict-
     * checks: writing IA32_*_CET before CR4.CET=1 raises #GP per SDM
     * Vol 4 IA32_S_CET description. */
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

    /* cabin must be ready — we map into the process's vmm_context.
     * Without one the process is structurally invalid; this is the
     * only condition that bubbles up an error (caller is buggy). */
    vmm_context_t *ctx = (vmm_context_t *)proc->cabin;
    if (!ctx) return ERR_INVALID_STATE;

    /* Allocate a contiguous 16 KiB phys page run from the user zone.
     * Falls back to the general pool if the USER zone is exhausted. */
    void *ssp_phys = pmm_alloc_zero(CET_USER_SSP_PAGES, PHYS_TAG_USER);
    if (!ssp_phys) ssp_phys = pmm_alloc_zero(CET_USER_SSP_PAGES);
    if (!ssp_phys) {
        /* SSP alloc failed — degrade silently. The process runs
         * without shadow-stack protection (cet_load_user_ssp_for_iretq
         * sees ssp_va=0 and skips the WRMSR). Logged as a warning so
         * operators can spot CET coverage loss under memory pressure.
         * NOT fatal — refusing to spawn the process is worse for
         * availability than running it CET-less. */
        debug_printf("[CET] WARN proc=%u SSP alloc failed (%u pages) "
                     "— process runs without SHSTK\n",
                     proc->pid, CET_USER_SSP_PAGES);
        process_set_user_ssp(proc, 0, 0, 0);
        return OK;
    }

    /* Map into the process's vmm_context at a fixed user VA below the
     * data stack. Use the user-SS PTE bit so CPU recognises the page
     * as a shadow stack target (Intel SDM Vol 3A §4.5.1 Table 4-19).
     * We map writable so WRUSS instructions can prime the SSP slot;
     * runtime user code reaches the page only via CALL/RET shadow ops.
     */
    uintptr_t user_ssp_va = process_user_ssp_va_for(proc);  /* fixed VA per process */
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                     VMM_FLAG_USER    | VMM_PTE_CET_SS_USER;
    for (uint32_t i = 0; i < CET_USER_SSP_PAGES; i++) {
        vmm_map_result_t r = vmm_map_page(ctx,
                                            user_ssp_va + i * PMM_PAGE_SIZE,
                                            (uintptr_t)ssp_phys + i * PMM_PAGE_SIZE,
                                            flags);
        if (!r.success) {
            /* Roll back partial mapping + degrade silently. */
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

    /* Initial SSP value: top of the SSP region minus 8 bytes — points
     * one slot below the last writable address so the first CALL's
     * shadow-stack push (SSP -= 8; *SSP = retaddr) lands inside the
     * mapped region. 8-byte aligned by construction. */
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

/* ─── PL3_SSP prime for first iretq → ring 3 ──────────────────────── */

void cet_load_user_ssp_for_iretq(struct process_t *proc) {
    /* Skip every dormancy case quietly — no log spam in the hot path. */
    if (!proc) return;
    if (!g_cet_enabled) return;
    if (!g_cpu_caps.has_shstk) return;
    uintptr_t ssp = process_get_user_ssp_va(proc);
    if (ssp == 0) return;
    /* WRMSR IA32_PL3_SSP — must be 8-byte aligned per SDM Vol 3D §17.2.3,
     * which cet_process_create's "base+SIZE-8" guarantees by construction.
     * Asserting cheaply rather than letting a future change drift. */
    if ((ssp & 0x7u) != 0) {
        debug_printf("[CET] proc=%u SSP misaligned (0x%lx) — skipping WRMSR\n",
                     proc->pid, (unsigned long)ssp);
        return;
    }
    cet_wrmsr(VMM_MSR_IA32_PL3_SSP, (uint64_t)ssp);
}

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

/* ─── Supervisor SSP infrastructure (PL0_SSP + ISST) ───────────────── */

/* Roll back a partial supervisor SSP setup. Returns ERR_NO_MEMORY so the
 * caller can propagate. Safe to call with any subset of pointers zeroed. */
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
    if (pc->cet_supv_ready) return OK;  /* idempotent */

    /* ─── Allocate the page set ─── */
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

    /* ─── Resolve kernel-VA aliases ─── */
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

    /* ─── Write supervisor SSP tokens via WRSSQ ───
     *
     * Token placement, ordering, and access path:
     *
     *   1. Write the token via NORMAL store BEFORE the PTE flips to
     *      shadow-stack. A normal store to a non-SS page is unrestricted;
     *      a normal store to a bit-60 page faults (only WRSSQ/RSTORSSP
     *      may write to shadow-stack memory per Intel SDM §17.2.1).
     *      We exploit the pre-flip window so init works without depending
     *      on WRSSQ availability at this stage.
     *
     *   2. Flip PTE bit 60 (VMM_PTE_CET_SS_SUPV) — Intel SDM Vol 3A §4.5
     *      requires the supervisor-SS bit for any page consumed by
     *      RDSSP/RSTORSSP/SETSSBSY/etc.
     *
     *   3. INVLPG to drop any stale TLB entry from earlier kernel writes
     *      (Pull Map zero-fill, etc.). Without the flush, a stale TLB
     *      entry without bit 60 set could let a NORMAL store succeed
     *      after the flip — defeating the protection.
     *
     * Token format (Intel SDM Vol 1 §17.2.3): bits 63:3 = SSP value
     * (token's own VA, 8B-aligned), bit 0 = 1 (supervisor mode bit).
     */
    uintptr_t pl0_top = pl0_va + (PMM_PAGE_SIZE - 8);
    *(volatile uint64_t *)pl0_top = (uint64_t)pl0_top | CET_SUPV_TOKEN_MODE_BIT;

    uintptr_t ist_top[CET_SUPV_IST_LEVELS];
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        ist_top[i] = ist_va[i] + (PMM_PAGE_SIZE - 8);
        *(volatile uint64_t *)ist_top[i] =
            (uint64_t)ist_top[i] | CET_SUPV_TOKEN_MODE_BIT;
    }

    /* Flip PTE bit 60 for every SSP page (token already written above
     * via the pre-flip normal-store window). The ISST page does NOT
     * get the bit — the CPU reads it via normal loads, not via
     * shadow-stack instructions. */
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

    /* ─── Populate IA32_INTERRUPT_SSP_TABLE_ADDR table ───
     *
     * Per Intel SDM Vol 3D §17.6 Table 17-1, the 8 entries map to IST
     * levels 0..7. BoxOS uses 0=PL0_SSP and 1..5=#DF/NMI/#MC/#DB/#SS;
     * entries 6, 7 stay zero — an IST 6/7 vector entering the kernel
     * would land on a zeroed SSP, faulting fast rather than silently
     * corrupting a shared stack.
     *
     * Entry 0 mirrors IA32_PL0_SSP so an interrupt with IST=0 sees the
     * same SSP whether the CPU consults PL0_SSP directly or the table
     * (some micro-architectures choose between the two paths). */
    uint64_t *table = (uint64_t *)isst_va;
    table[0] = (uint64_t)pl0_top;
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        table[1 + i] = (uint64_t)ist_top[i];
    }
    table[6] = 0;
    table[7] = 0;

    /* ─── Program the MSRs ───
     *
     * Writing PL0_SSP and ISST does NOT activate supervisor SHSTK —
     * activation is gated by IA32_S_CET.SH_STK_EN. Until that bit
     * flips in a follow-up commit (after the assembly audit), the MSRs
     * sit dormant. Programming them now means the SH_STK_EN flip is a
     * one-MSR-write change instead of a sequenced page-then-MSR dance. */
    cet_wrmsr(VMM_MSR_IA32_PL0_SSP, (uint64_t)pl0_top);
    cet_wrmsr(VMM_MSR_IA32_INTERRUPT_SSP_TABLE_ADDR, (uint64_t)isst_va);

    /* ─── Persist bookkeeping ─── */
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

    /* Clear the MSRs first so no in-flight transition reads a freed
     * SSP value. SH_STK_EN must already be 0 — the caller is responsible
     * for sequencing this against any prior SHSTK enable. */
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

/* ─── Per-process kernel SSP ──────────────────────────────────────── */

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

    /* Pre-flip window: write supervisor token via NORMAL store before the
     * PTE flips to bit-60 (shadow-stack-only). This avoids a WRSSQ
     * dependency for the very first write. */
    *(volatile uint64_t *)top = (uint64_t)top | CET_SUPV_TOKEN_MODE_BIT;

    /* Flip PTE bit 60 + INVLPG so subsequent WRSSQ recognises the page
     * as supervisor shadow stack. */
    vmm_context_t *kctx = vmm_get_kernel_context();
    if (kctx) {
        pte_t *p = vmm_get_or_create_pte(kctx, va);
        if (p) *p |= VMM_PTE_CET_SS_SUPV;
        cet_invlpg(va);
    }

    /* WRSSQ the pre-pushed entry RIP one slot below the token. The very
     * first task_restore_context for this process does
     *   push entry_rip; RET
     * RET pops entry_rip from the regular stack AND from the shadow
     * stack at [top - 8] — we just wrote entry_rip there. Match → no
     * #CP, process runs at entry_rip. */
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

/* ─── SH_STK_EN activation handshake ──────────────────────────────── */

__attribute__((noreturn))
void cet_supv_shstk_activate_and_jump(void (*target)(void))
{
    /* Dormancy paths: TCG (no SHSTK), unsupported CPU, BSP refused
     * activation. Direct-call the target — caller's contract says target
     * never returns either way. */
    if (!g_cet_enabled || !g_cpu_caps.has_shstk) {
        target();
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* The activation MSR write must happen with no possibility of a
     * RET firing on a shadow stack that has nothing matching. Order:
     *
     *   1. Mark g_cet_supv_active = 1 — any context switch that runs
     *      during the brief window between here and the WRMSR below
     *      will RESTORE_PL0_SSP based on ctx.pl0_ssp. That MSR write
     *      lands on a CPU where SH_STK_EN is still 0, so the MSR
     *      just updates IA32_PL0_SSP with no enforcement — harmless.
     *      Once SH_STK_EN flips, future switches behave correctly.
     *
     *   2. WRMSR IA32_S_CET with SH_STK_EN OR-merged into the existing
     *      policy. We RDMSR-then-OR-WRMSR rather than recomputing the
     *      whole policy from scratch — preserves whatever bits any
     *      future config tweak might have written between init_bsp
     *      and now.
     *
     *   3. JMP target. JMP doesn't push to the shadow stack (unlike
     *      CALL), so target starts with the per-CPU PL0_SSP at the
     *      token slot — clean state. target's first CALL pushes one
     *      below the token; subsequent CALL/RET balance. */
    __asm__ volatile(
        "movb $1, g_cet_supv_active(%%rip)\n\t"
        "movl $0x6A2, %%ecx\n\t"           /* IA32_S_CET */
        "rdmsr\n\t"
        "orl  $1, %%eax\n\t"                /* SH_STK_EN = bit 0 */
        "wrmsr\n\t"
        "jmpq *%0\n\t"
        :
        : "r"(target)
        : "rax", "rcx", "rdx", "memory"
    );
    __builtin_unreachable();
}
