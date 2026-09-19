/*
 * CET — Control-flow Enforcement Technology lifecycle.
 *
 * See cet_lifecycle.h for design + spec citations. This file owns:
 *   1. BSP + AP CR4.CET enable.
 *   2. IA32_S_CET + IA32_U_CET MSR programming.
 *   3. Probing whether the silicon offers the CET XSAVE state components.
 *   4. Per-process shadow stack allocation + free.
 *   5. Telemetry + Touch publish on lifecycle events.
 *
 * ‼ OPEN, OWNER DECISION PENDING — how a page is marked shadow stack.
 *
 * This file marks its SSP pages by setting PTE bit 60 (supervisor) or
 * bit 61 (user). That marks nothing. In ordinary paging bits 62:59 are
 * the 4-bit protection key and bit 60 is simply key bit 1 (Intel SDM
 * Vol 3A Table 4-19; Linux: _PAGE_BIT_PKEY_BIT1 = 60). The bit-60
 * "supervisor shadow stack" attribute exists only in EPT
 * paging-structure entries (Vol 3C §29.3.2), which this kernel does
 * not build. The encoding real silicon honours is R/W = 0 with
 * Dirty = 1, U/S selecting supervisor or user (Vol 1 §18.2.2).
 *
 * So today: every supervisor SSP page below is stamped protection key
 * 2 and every user SSP page protection key 4, and none of them is a
 * shadow stack to the CPU.
 *
 * ‼ And this is not dormant. An earlier version of this note said
 * nothing faults because IA32_S_CET.SH_STK_EN stays 0. It does not stay
 * 0: cet_supv_shstk_activate_and_jump ORs SH_STK_EN in on every core
 * (main.c for the BSP, ap_init.c for each AP) as its last act before
 * entering the run loop, and cet_program_msrs already puts SH_STK_EN in
 * IA32_U_CET. On a CPU that advertises SHSTK — Tiger Lake and later,
 * Zen 3 and later — that means the processor begins pushing and popping
 * return addresses against pages it does not regard as shadow stacks,
 * and a shadow-stack access to a page without the encoding faults. The
 * same applies to the WRSSQ in cet_process_create_kernel_ssp. Nothing
 * has shown up because every board this has run on so far reports no
 * SHSTK; the first machine that reports it will not boot.
 *
 * Switching the marking to W=0/D=1 changes how the pages are mapped, how
 * the pre-flip token write below works (a read-only page cannot take a
 * normal store), and how they are torn down; that is a redesign, and the
 * owner decides it, not this file. Until then nothing here claims the
 * bits mean what they do not.
 */

#include "cet_lifecycle.h"
#include "klib.h"
#include "cpuid.h"
#include "vmm.h"
#include "pmm.h"
#include "touch.h"
#include "logbook.h"
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

/* Supervisor shadow-stack token — Intel SDM Vol 1 "Supervisor Shadow Stack
 * Token" (§18.2.3 in the current numbering, §17.2.3 in the revision this
 * file was first written against). Format:
 *   bits 63:3  — the linear address of the token itself, 8-byte aligned
 *   bits 2:1   — reserved, must be 0
 *   bit 0      — BUSY. 0 = this shadow stack is not in use by any logical
 *                processor; 1 = it is.
 *
 * Bit 0 is NOT a supervisor/user mode selector. It is a lock, and the
 * processor is the only party that is supposed to set it:
 *
 *   - SETSSBSY computes expected_token_value = IA32_PL0_SSP and
 *     new_token_value = IA32_PL0_SSP | BUSY, then does a locked compare-
 *     exchange against the token. "If busy bit in token is set" → #CP.
 *   - An event delivered to CPL 0 that switches shadow stacks (any IST
 *     vector, or any entry from CPL 3) reads the token at the new SSP,
 *     requires its busy bit CLEAR, and sets it. A token that is already
 *     busy faults instead of running the handler.
 *
 * So a freshly minted token must be written with busy = 0 — the plain
 * address of its own slot. Writing address|1 hands the CPU a shadow stack
 * that claims to be in use by someone else; the first IST delivery after
 * S_CET.SH_STK_EN goes to 1 lands on #DF / NMI / #MC and escalates with
 * nothing on the console. */
#define CET_SUPV_TOKEN_FLAG_MASK    0x7ULL   /* bit 0 BUSY + bits 2:1 rsvd */

/* The value to place in a supervisor shadow-stack token slot at `top`:
 * its own linear address with BUSY and the reserved bits cleared. */
static inline uint64_t cet_supv_token_free(uintptr_t top) {
    return (uint64_t)top & ~CET_SUPV_TOKEN_FLAG_MASK;
}

/* WRSSQ — Write 8 bytes to a supervisor-shadow-stack page.
 *
 * Intel SDM Vol 2 §WRSSQ: writes a 64-bit value to memory the CPU sees
 * as a supervisor shadow stack, i.e. a page mapped R/W = 0 with
 * Dirty = 1 and U/S = 0. CPL=0 + CR4.CET=1 + IA32_S_CET.WR_SHSTK_EN=1
 * required.
 *
 * Critical: a NORMAL store to such a page FAULTS — only WRSSQ /
 * WRUSS / SETSSBSY / RSTORSSP are allowed write paths into shadow stack
 * memory. The kernel must use this primitive when initialising supervisor
 * SSP tokens and when pushing the first synthetic entry during the
 * SH_STK_EN activation handshake.
 *
 * See the shadow-stack marking note at the top of this file: the pages
 * this kernel hands to WRSSQ today are not marked that way, so WRSSQ
 * against them is inert rather than protective.
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
 * after any change to a leaf PTE's upper bits so subsequent accesses
 * see the new entry rather than a cached copy of the old one. */
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
 * Supervisor (IA32_S_CET): SH_STK_EN is omitted HERE and only here.
 * Enabling the supervisor shadow stack requires an allocated supervisor
 * SSP per CPU and an IA32_PL0_SSP write before any kernel RET — without
 * those, the very first ring-0 RET (e.g. the one that returns from
 * cet_program_msrs into per_core_init_bsp) fires #CP and the kernel
 * dies. That infrastructure exists: cet_lifecycle_init_supervisor_ssp
 * runs later in per-core bring-up, and cet_supv_shstk_activate_and_jump
 * is what finally ORs SH_STK_EN in — from main.c on the BSP and from
 * ap_init.c on every AP, as the last act before the core stops being
 * able to return. So this mask is the pre-activation policy, not the
 * kernel's final S_CET.
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

    /* CR4.CET first, then the two MSRs.
     *
     * Not because the order is mandated — an earlier version of this
     * comment claimed IA32_*_CET is inaccessible until CR4.CET=1 and that
     * writing it first raises #GP. That is not so: the MSRs exist whenever
     * CPUID.07H.0:ECX[CET_SS] or EDX[CET_IBT] is set, independent of CR4,
     * and Linux writes MSR_IA32_S_CET BEFORE cr4_set_bits(X86_CR4_CET) in
     * setup_cet(). Either order works; this one keeps the enable in one
     * place.
     *
     * The rule that DOES bind here is the other one: CR4.CET may be set
     * only while CR0.WP = 1, and a MOV to CR4 that sets CET with WP clear
     * raises #GP(0) (the same rule forbids clearing CR0.WP afterwards).
     * enable_fpu sets CR0.WP and runs before this on both paths — main.c
     * calls it long before cet_lifecycle_init_bsp, and per_core_init_ap
     * calls it before cet_lifecycle_init_ap. */
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

/* ─── CET state in the XSAVE feature set ─────────────────────────────
 *
 * ‼ The CET registers are NOT XCR0 material, and this kernel does not
 * carry them across a context switch.
 *
 * Component 11 (CET_U — IA32_U_CET and IA32_PL3_SSP) and component 12
 * (CET_S — IA32_PL{0,1,2}_SSP) are SUPERVISOR state components. Intel SDM
 * Vol 1 §13.5: supervisor state components are enumerated in
 * CPUID.(EAX=0DH,ECX=1):ECX/EDX and are enabled through the IA32_XSS MSR
 * (0xDA0), and only XSAVES / XRSTORS touch them. XCR0 carries USER state
 * components only, and CPUID.(EAX=0DH,ECX=0) — the leaf g_cpu_caps
 * .xcr0_supported is read from — never reports bits 11 or 12 at all.
 *
 * So the previous shape of this function, which called
 * fpu_xsave_register_extension for both bits, could not have worked on any
 * processor ever built: the call was refused by the xcr0_supported test on
 * every machine, through a debug_printf that compiles to nothing. What it
 * left behind was a promise in cet_lifecycle.h that IA32_PL3_SSP rides
 * along in the XSAVE area. It does not. BoxOS programs no IA32_XSS and
 * issues no XSAVES, so the kernel owns IA32_PL3_SSP itself — see the
 * PL3_SSP note in cet_lifecycle.h.
 *
 * What we can honestly report is whether THIS CPU offers the components,
 * so `hw cet status` distinguishes "silicon has no CET state components"
 * from "it has them and we do not manage them". */
#define CET_XSS_CET_U_BIT   (1u << 11)
#define CET_XSS_CET_S_BIT   (1u << 12)

static void cet_probe_xsave_components(void) {
    g_xsave_cet_s = false;
    g_xsave_cet_u = false;
    if (!g_cpu_caps.has_xsave)             return;
    if (g_cpu_caps.max_basic_leaf < 0x0D)  return;

    uint32_t eax, ebx, ecx, edx;
    cpuid_count(0x0D, 1, &eax, &ebx, &ecx, &edx);
    g_xsave_cet_u = (ecx & CET_XSS_CET_U_BIT) != 0;
    g_xsave_cet_s = (ecx & CET_XSS_CET_S_BIT) != 0;

    debug_printf("[CET] XSAVE state components offered via IA32_XSS: "
                 "CET_U(11)=%d CET_S(12)=%d — IA32_XSS is not programmed by "
                 "this kernel, so neither is saved on a context switch\n",
                 (int)g_xsave_cet_u, (int)g_xsave_cet_s);
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
    g_tag_enabled      = TouchLogbookIntern("cet:enabled");
    g_tag_shstk_active = TouchLogbookIntern("shstk:enabled");
    g_tag_ibt_active   = TouchLogbookIntern("ibt:enabled");

    /* Record whether the silicon offers the CET XSAVE state components. */
    cet_probe_xsave_components();

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
    vmm_context_t *ctx = proc->cabin ? proc->cabin->vmm : NULL;
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
     * data stack, writable, with VMM_PTE_CET_SS_USER.
     *
     * That last flag is bit 61, which Table 4-19 places inside the
     * protection-key field: the page ends up carrying protection key 4,
     * and the CPU does not treat it as a user shadow stack. The
     * encoding it would honour is R/W = 0 + Dirty = 1 + U/S = 1 — see
     * the note at the top of this file. */
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
    vmm_context_t *ctx = proc->cabin ? proc->cabin->vmm : NULL;
    /* Recover the base of the mapped range from the initial SSP value,
     * which cet_process_create set to base + SIZE - 8. */
    uintptr_t va_base = (process_get_user_ssp_va(proc) + 8) - CET_USER_SSP_SIZE;
    if (ctx) {
        for (uint32_t i = 0; i < CET_USER_SSP_PAGES; i++) {
            vmm_unmap_page(ctx, va_base + i * PMM_PAGE_SIZE);
        }
    }
    pmm_free((void *)phys, CET_USER_SSP_PAGES);
    process_set_user_ssp(proc, 0, 0, 0);
    __atomic_fetch_add(&g_stat_ssp_frees, 1, __ATOMIC_RELAXED);
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
    debug_printf("[CET] stats: enabled=%d SHSTK=%d IBT=%d XSS_offered{S=%d U=%d} "
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
     *   1. Write the token via NORMAL store BEFORE the PTE is stamped.
     *      A normal store to a page the CPU sees as a shadow stack
     *      faults (only WRSSQ/RSTORSSP may write there per Intel SDM
     *      §17.2.1), so init runs in the pre-stamp window and does not
     *      depend on WRSSQ availability at this stage.
     *
     *   2. Set PTE bit 60 (VMM_PTE_CET_SS_SUPV). This is the marking
     *      the file-header note calls out: bit 60 is protection-key
     *      bit 1 in ordinary paging, so the page comes out carrying
     *      key 2 and the CPU still sees ordinary supervisor memory.
     *      Real supervisor shadow stacks are R/W = 0 + Dirty = 1.
     *
     *   3. INVLPG to drop any stale TLB entry from earlier kernel writes
     *      (Pull Map zero-fill, etc.), so later accesses resolve against
     *      the entry as written rather than a cached copy.
     *
     * Token contents: the token's own linear address with the BUSY bit
     * clear — see CET_SUPV_TOKEN_BUSY_BIT above for why "clear" is the
     * only value a token the processor has not claimed may hold.
     */
    uintptr_t pl0_top = pl0_va + (PMM_PAGE_SIZE - 8);
    *(volatile uint64_t *)pl0_top = cet_supv_token_free(pl0_top);

    uintptr_t ist_top[CET_SUPV_IST_LEVELS];
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        ist_top[i] = ist_va[i] + (PMM_PAGE_SIZE - 8);
        *(volatile uint64_t *)ist_top[i] = cet_supv_token_free(ist_top[i]);
    }

    /* Set PTE bit 60 on every SSP page (token already written above via
     * the pre-stamp normal-store window). The ISST page does NOT get
     * the bit — the CPU reads it via normal loads. On this hardware the
     * bit is protection-key bit 1, not a shadow-stack attribute; see
     * the note at the top of this file. */
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
     * Entry 0 is never consulted: an IDT gate's IST field of 0 means no
     * IST stack switch at all, and such a delivery takes its SSP from
     * IA32_PL0_SSP. We fill it with the same token anyway so the table
     * holds no slot that is neither a valid token nor zero. */
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
     * activation is gated by IA32_S_CET.SH_STK_EN, which this core flips
     * later in its own bring-up, inside cet_supv_shstk_activate_and_jump.
     * Both MSRs must already hold valid token addresses by then, which is
     * why they are programmed here and not there. */
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
                 "(IST 1..5 SSPs allocated, tokens free; S_CET.SH_STK_EN still 0 "
                 "until cet_supv_shstk_activate_and_jump)\n",
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

    /* Pre-stamp window: write supervisor token via NORMAL store before
     * the PTE is stamped. This avoids a WRSSQ dependency for the very
     * first write. BUSY clear — the processor claims the token, not us. */
    *(volatile uint64_t *)top = cet_supv_token_free(top);

    /* Set PTE bit 60 + INVLPG. Bit 60 is protection-key bit 1 here, not
     * a shadow-stack attribute — see the note at the top of this file. */
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
