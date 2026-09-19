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
 * How a page here is marked a shadow stack
 * ----------------------------------------
 * R/W = 0 and Dirty = 1 on the leaf, R/W = 1 on every entry above it,
 * U/S picking the ring. That is the whole encoding and the only one the
 * processor honours. The verbatim rule from Intel SDM Vol 3A §4.6 sits
 * in vmm.h above VMM_FLAGS_SHADOW_STACK_SUPV, and vmm_shadow_stack_seal
 * / vmm_shadow_stack_unseal are the only two functions that apply and
 * remove it.
 *
 * Earlier revisions set PTE bit 60 (supervisor) or bit 61 (user)
 * instead. Those bits mark nothing: in ordinary paging bits 62:59 are
 * the 4-bit protection key and bit 60 is key bit 1 (Vol 3A Table 4-19;
 * Linux names it _PAGE_BIT_PKEY_BIT1), and the bit-60 "supervisor
 * shadow stack" attribute exists only in EPT paging-structure entries
 * (Vol 3C §29.3.2), a table this kernel does not build. Every
 * supervisor SSP page came out carrying protection key 2 and none of
 * them was a shadow stack to the CPU. The user-side stamp never even
 * reached the entry: vmm_make_pte masks incoming flags with
 * vmm_pte_flags_mask (bits 11:0 plus bit 63), so bit 61 was dropped on
 * the way in.
 *
 * That was never dormant. cet_supv_shstk_activate_and_jump ORs
 * S_CET.SH_STK_EN in on every core (main.c for the BSP, ap_init.c for
 * each AP) and cet_program_msrs puts SH_STK_EN in IA32_U_CET. On a CPU
 * that advertises SHSTK the processor would have pushed and popped
 * return addresses against pages it does not regard as shadow stacks.
 *
 * Why the tokens are written before the seal, and not with WRSS
 * -------------------------------------------------------------
 * A supervisor SSP page needs a token in it before the processor may
 * switch to it, and a sealed page takes no normal store. WRSSQ is the
 * instruction for writing a shadow stack, and it is unavailable in this
 * window. Intel SDM Vol 2D, WRSSD/WRSSQ, Operation, the CPL < 3 arm:
 *
 *     IF (CR4.CET & IA32_S_CET.SH_STK_EN) = 0
 *           THEN #UD; FI;
 *     IF (IA32_S_CET.WR_SHSTK_EN) = 0
 *           THEN #UD; FI;
 *
 * WRSS at CPL < 3 needs S_CET.SH_STK_EN — the one bit this kernel holds
 * at 0 until cet_supv_shstk_activate_and_jump, because turning it on
 * before a valid PL0_SSP exists kills the next kernel RET. Every token
 * write happens inside that window, so WRSS is #UD for all of it. The
 * same page also requires the destination to be a shadow stack already
 * ("#PF ... if destination is ... not a supervisor shadow stack when
 * CPL < 3"), so WRSS could never have written the FIRST token either
 * way — only added to a page that had one.
 *
 * The other option is taken, in its cheapest form: the token goes in
 * through the mapping the page already has, while that mapping is still
 * writable, and the page is sealed afterwards. No second alias is
 * opened and none has to be torn down — pmm_alloc_zero wrote the frame
 * through the same kernel window microseconds earlier, so this write
 * inherits a window rather than creating one. After the seal the only
 * writer left is the processor's own shadow-stack machinery.
 *
 * ‼ Teardown unseals before the frame goes back to the PMM. The next
 * pmm_alloc_zero of that frame memsets it through the kernel window and
 * would fault on a sealed leaf.
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

/* ─── IA32_S_CET / IA32_U_CET bits (Intel SDM Vol 3D §17.2.1) ─── */

#define CET_MSR_SH_STK_EN        (1ULL <<  0)   /* Shadow Stack Enable */
#define CET_MSR_WR_SHSTK_EN      (1ULL <<  1)   /* WRSS / WRUSS allowed */
#define CET_MSR_ENDBR_EN         (1ULL <<  2)   /* IBT — ENDBR enforcement */
#define CET_MSR_LEG_IW_EN        (1ULL <<  3)   /* Legacy WAIT-FOR-ENDBRANCH */
#define CET_MSR_NO_TRACK_EN      (1ULL <<  4)   /* NOTRACK prefix allowed */
#define CET_MSR_SUPPRESS_DIS     (1ULL <<  5)   /* SUPPRESS not allowed */
#define CET_MSR_SUPPRESS         (1ULL << 10)   /* WAIT-FOR-ENDBRANCH state */
#define CET_MSR_TRACKER_IDLE     (1ULL << 11)   /* WAIT-FOR-ENDBRANCH idle */

/* Per-process user shadow stack — 4 pages, 16 KiB, against a 64 KiB user
 * data stack. Counted, not guessed:
 *
 *   data stack   CONFIG_USER_STACK_SIZE_KB = 64 → 16 pages (kernel_config.h)
 *   shadow stack CET_USER_SSP_PAGES        = 4  → 16 KiB = 2048 slots
 *
 * One call costs the shadow stack exactly 8 bytes: the return address,
 * and nothing else ever lands there. One call costs the DATA stack 16
 * bytes at the very least — the 8-byte return address plus the 8 the
 * System V AMD64 ABI forces on any function that goes on to call
 * another. RSP is 16-byte aligned at a call site, so at entry it sits 8
 * bytes off alignment and a non-leaf must push or subtract 8 before its
 * own call. Anything cheaper than 16 belongs to a leaf, and a leaf is
 * the end of the chain.
 *
 * So the deepest chain 64 KiB of data stack admits is 65536 / 16 = 4096
 * frames, and the shadow stack holds 2048 of them. It runs out first, at
 * exactly half the depth, and on ANY chain of small non-leaf frames —
 * not a contrived one. The number that matches the data stack is 8
 * pages, CONFIG_USER_STACK_PAGES / 2, and that ratio survives any resize
 * of the data stack.
 *
 * It stays 4 because 4 is not this file's number. The hammock slot map
 * in cabin_layout.h puts the SSP at slot pages 18..21 with the
 * Pocket/Result/Touch rings starting at 23, PROCESS_USER_SSP_REGION_SIZE
 * in process.c hard-codes four pages behind a _Static_assert, and
 * strand_rings.c derives its ring offsets from the same layout. Growing
 * to 8 shifts every page above it in the slot. Raised with the owner.
 *
 * Running out is a fault, not corruption: the push lands in the unmapped
 * guard page below the region (PROCESS_USER_SSP_GUARD_LO_BASE in
 * process.c, the [mid guard] page of a strand's hammock slot). */
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
    /* SHSTK enable only when the CPU advertises shadow stacks; IBT
     * enable only when it advertises ENDBR/NOTRACK enforcement. The two
     * sides differ on one bit and it matters.
     *
     * Supervisor (IA32_S_CET): SH_STK_EN is withheld HERE and only
     * here. Turning it on requires an allocated supervisor SSP on this
     * core and an IA32_PL0_SSP holding its token, or the very first
     * ring-0 RET after the WRMSR — the one that leaves this function —
     * fires #CP and the kernel dies. cet_lifecycle_init_supervisor_ssp
     * builds that infrastructure later in per-core bring-up, and
     * cet_supv_shstk_activate_and_jump is what finally ORs SH_STK_EN
     * in, from main.c on the BSP and ap_init.c on every AP, as the last
     * act before the core stops being able to return. What the kernel
     * does keep supervisor-side before then is ENDBR_EN + NO_TRACK_EN:
     * IBT defends against JOP/COP with no shadow stack needed.
     *
     * WR_SHSTK_EN, supervisor side: this is the WRSS gate, and WRSS is
     * the one instruction that can forge a shadow-stack entry from
     * ring 0. It is set because the next stage of the CET work (the
     * user-return PL3_SSP reload in the context-switch path) needs it;
     * nothing in this file executes WRSS, and nothing can until
     * SH_STK_EN goes up, since WRSS at CPL < 3 is #UD without it —
     * Intel SDM Vol 2D, WRSSD/WRSSQ. It is NOT the WRUSS gate: WRUSS
     * requires only CR4.CET=1 and CPL=0 (same volume, WRUSSD/WRUSSQ,
     * Operation), and an earlier version of this comment had that
     * wrong.
     *
     * User (IA32_U_CET): full SHSTK. User code returns through its own
     * per-process shadow stack. WR_SHSTK_EN is omitted so ring 3 cannot
     * execute WRSS and write its own return addresses. */
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
     * data stack, as a user shadow stack: PRESENT | DIRTY | USER, and
     * pointedly NOT WRITABLE. That combination — R/W = 0 with Dirty = 1
     * and U/S = 1 — is the whole of what makes these pages shadow
     * stacks to the processor (Intel SDM Vol 3A §4.6, quoted in vmm.h).
     *
     * No token here, unlike the supervisor side: a user shadow stack
     * carries nothing before its first CALL, and the token business
     * belongs to supervisor stacks, which the processor has to claim on
     * event delivery. The pages are born with the encoding because this
     * is their first mapping in this context, so the seal below only
     * has to verify it — see there.
     * pmm_alloc_zero already zeroed the frames through the kernel
     * window, which is a different linear address and stays writable —
     * so the PMM can still recycle them when the process dies. */
    uintptr_t user_ssp_va = process_user_ssp_va_for(proc);  /* fixed VA per process */
    uint64_t flags = VMM_FLAGS_SHADOW_STACK_USER;
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

    /* Verified the same way, and refused as loudly, as the supervisor
     * side. The leaves already carry R/W = 0 + Dirty = 1, so the seal's
     * CAS is a no-op and it sends no shootdown; what it does do is walk
     * SDM Vol 3A §4.6 condition (2) — R/W = 1 in every entry ABOVE the
     * leaf. That is the half of the rule leaf flags cannot express, and
     * a cabin whose PML4E / PDPTE / PDE came out read-only would leave
     * the process holding ordinary read-only pages: its first ring-3
     * CALL takes #CP and nothing on the console says why. Degrading is
     * this path's policy for every other failure too, so degrade — but
     * say it on the console, because this one is structural. */
    for (uint32_t i = 0; i < CET_USER_SSP_PAGES; i++) {
        uintptr_t page_va = user_ssp_va + i * PMM_PAGE_SIZE;
        if (vmm_shadow_stack_seal(ctx, page_va)) continue;

        kprintf("[CET] proc=%u user shadow-stack page %u at 0x%lx is not a "
                "shadow-stack address — process runs without SHSTK\n",
                proc->pid, i, (unsigned long)page_va);
        for (uint32_t k = 0; k < CET_USER_SSP_PAGES; k++) {
            vmm_unmap_page(ctx, user_ssp_va + k * PMM_PAGE_SIZE);
        }
        pmm_free(ssp_phys, CET_USER_SSP_PAGES);
        process_set_user_ssp(proc, 0, 0, 0);
        return OK;
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

/* The page set one core's supervisor shadow-stack infrastructure is made
 * of, carried as one value while it is being built so that a failure at
 * any step can hand back exactly what was taken — including the leaves
 * that already carry the shadow-stack encoding, which must be unsealed
 * before their frames go anywhere near the PMM. */
typedef struct {
    void      *pl0_phys;
    uintptr_t  pl0_va;
    bool       pl0_sealed;
    void      *isst_phys;
    uintptr_t  isst_va;
    void      *ist_phys[CET_SUPV_IST_LEVELS];
    uintptr_t  ist_va[CET_SUPV_IST_LEVELS];
    int        ist_allocated;   /* ist_phys[0 .. n) hold live frames   */
    int        ist_sealed;      /* ist_va[0 .. n) are sealed leaves    */
} CetSupvSspSet;

/* Give the whole set back and return `err` so a caller can write
 * `return CetSupvSspSetRelease(&set, ERR_NO_MEMORY);`. Unseal first,
 * free second, in that order and never the other way round. */
static error_t CetSupvSspSetRelease(CetSupvSspSet *set, error_t err)
{
    vmm_context_t *kctx = vmm_get_kernel_context();

    if (kctx) {
        for (int i = 0; i < set->ist_sealed; i++) {
            (void)vmm_shadow_stack_unseal(kctx, set->ist_va[i]);
        }
        if (set->pl0_sealed) {
            (void)vmm_shadow_stack_unseal(kctx, set->pl0_va);
        }
    }

    for (int i = 0; i < set->ist_allocated; i++) {
        if (set->ist_phys[i]) pmm_free(set->ist_phys[i], 1);
    }
    if (set->isst_phys) pmm_free(set->isst_phys, 1);
    if (set->pl0_phys)  pmm_free(set->pl0_phys, 1);
    return err;
}

error_t cet_lifecycle_init_supervisor_ssp(uint8_t core_idx)
{
    if (!g_cet_enabled || !g_cpu_caps.has_shstk) return ERR_UNSUPPORTED;
    if (core_idx >= MAX_CORES)                   return ERR_INVALID_ARGUMENT;

    PerCoreData *pc = &g_per_core[core_idx];
    if (pc->cet_supv_ready) return OK;  /* idempotent */

    CetSupvSspSet set = {0};

    /* ─── Allocate the page set ─── */
    set.pl0_phys = pmm_alloc_zero(1);
    if (!set.pl0_phys) return ERR_NO_MEMORY;

    set.isst_phys = pmm_alloc_zero(1);
    if (!set.isst_phys) return CetSupvSspSetRelease(&set, ERR_NO_MEMORY);

    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        set.ist_phys[i] = pmm_alloc_zero(1);
        if (!set.ist_phys[i]) return CetSupvSspSetRelease(&set, ERR_NO_MEMORY);
        set.ist_allocated = i + 1;
    }

    /* ─── Resolve kernel-VA aliases ─── */
    set.pl0_va  = (uintptr_t)vmm_phys_to_virt((uintptr_t)set.pl0_phys);
    set.isst_va = (uintptr_t)vmm_phys_to_virt((uintptr_t)set.isst_phys);
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        set.ist_va[i] = (uintptr_t)vmm_phys_to_virt((uintptr_t)set.ist_phys[i]);
        if (!set.ist_va[i]) return CetSupvSspSetRelease(&set, ERR_NO_MEMORY);
    }
    if (!set.pl0_va || !set.isst_va) {
        return CetSupvSspSetRelease(&set, ERR_NO_MEMORY);
    }

    /* ─── Every plain store happens here, before anything is sealed ───
     *
     * The tokens go in through the kernel window the frames already
     * have, while it is still writable. Once a page is sealed its leaf
     * is R/W = 0 and a normal store to it faults; WRSS cannot take over
     * because it is #UD until S_CET.SH_STK_EN is up, which is precisely
     * what this function has to happen before. The reasoning, with the
     * SDM quoted, is in the file header.
     *
     * Token contents: the token's own linear address with the BUSY bit
     * clear — cet_supv_token_free() and the supervisor-token note above
     * CET_SUPV_TOKEN_FLAG_MASK say why "clear" is the only value a token
     * the processor has not claimed may hold. */
    uintptr_t pl0_top = set.pl0_va + (PMM_PAGE_SIZE - 8);
    *(volatile uint64_t *)pl0_top = cet_supv_token_free(pl0_top);

    uintptr_t ist_top[CET_SUPV_IST_LEVELS];
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        ist_top[i] = set.ist_va[i] + (PMM_PAGE_SIZE - 8);
        *(volatile uint64_t *)ist_top[i] = cet_supv_token_free(ist_top[i]);
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
     * holds no slot that is neither a valid token nor zero.
     *
     * The ISST page itself is NOT a shadow stack and is never sealed:
     * the processor reads it with ordinary loads to find an SSP, it
     * holds no return addresses, and sealing it would only make this
     * store fault. */
    uint64_t *table = (uint64_t *)set.isst_va;
    table[0] = (uint64_t)pl0_top;
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        table[1 + i] = (uint64_t)ist_top[i];
    }
    table[6] = 0;
    table[7] = 0;

    /* ─── Seal: R/W = 0 + Dirty = 1 on every SSP leaf ───
     *
     * From here on these six pages are shadow stacks to the processor
     * and nothing but the processor writes them. A refusal is loud and
     * total: a core that cannot seal its stacks hands every frame back
     * and reports ERR_NO_MEMORY, so cet_supv_shstk_activate_and_jump
     * never turns SH_STK_EN on against pages that are not shadow
     * stacks. kprintf, not debug_printf — this is a line that has to
     * reach a real board's console. */
    vmm_context_t *kctx = vmm_get_kernel_context();
    if (!kctx || !vmm_shadow_stack_seal(kctx, set.pl0_va)) {
        kprintf("[CET/core %u] PL0 shadow-stack seal refused at 0x%lx — "
                "supervisor shadow stack NOT armed on this core\n",
                core_idx, (unsigned long)set.pl0_va);
        return CetSupvSspSetRelease(&set, ERR_NO_MEMORY);
    }
    set.pl0_sealed = true;

    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        if (!vmm_shadow_stack_seal(kctx, set.ist_va[i])) {
            kprintf("[CET/core %u] IST %d shadow-stack seal refused at 0x%lx — "
                    "supervisor shadow stack NOT armed on this core\n",
                    core_idx, i + 1, (unsigned long)set.ist_va[i]);
            return CetSupvSspSetRelease(&set, ERR_NO_MEMORY);
        }
        set.ist_sealed = i + 1;
    }

    /* ─── Program the MSRs ───
     *
     * Writing PL0_SSP and ISST does NOT activate supervisor SHSTK —
     * activation is gated by IA32_S_CET.SH_STK_EN, which this core flips
     * later in its own bring-up, inside cet_supv_shstk_activate_and_jump.
     * Both MSRs must already hold valid token addresses by then, which is
     * why they are programmed here and not there. */
    cet_wrmsr(VMM_MSR_IA32_PL0_SSP, (uint64_t)pl0_top);
    cet_wrmsr(VMM_MSR_IA32_INTERRUPT_SSP_TABLE_ADDR, (uint64_t)set.isst_va);

    /* ─── Persist bookkeeping ─── */
    pc->pl0_ssp_phys     = (uintptr_t)set.pl0_phys;
    pc->pl0_ssp_top_va   = pl0_top;
    pc->isst_phys        = (uintptr_t)set.isst_phys;
    pc->isst_va          = set.isst_va;
    for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
        pc->ist_ssp_phys[i]   = (uintptr_t)set.ist_phys[i];
        pc->ist_ssp_top_va[i] = ist_top[i];
    }
    pc->cet_supv_ready = true;

    debug_printf("[CET/core %u] supervisor SSP infra ready: PL0=0x%lx ISST=0x%lx "
                 "(IST 1..5 SSPs sealed R/W=0 D=1, tokens free; S_CET.SH_STK_EN "
                 "still 0 until cet_supv_shstk_activate_and_jump)\n",
                 core_idx, (unsigned long)pl0_top, (unsigned long)set.isst_va);
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

    /* Unseal every SSP leaf before the frames go back to the buddy. A
     * sealed leaf is R/W = 0, and the next pmm_alloc_zero to hand one of
     * these frames out memsets it through this same kernel window — on a
     * sealed leaf that is a #PF in the allocator, on a page the new owner
     * never suspected. The ISST page was never sealed, so it just goes.
     *
     * The unseal shoots the translation down across every core, because
     * the core being torn down is not necessarily the core running this. */
    vmm_context_t *kctx = vmm_get_kernel_context();
    if (kctx) {
        if (pc->pl0_ssp_phys) {
            uintptr_t va = (uintptr_t)vmm_phys_to_virt(pc->pl0_ssp_phys);
            if (va) (void)vmm_shadow_stack_unseal(kctx, va);
        }
        for (int i = 0; i < CET_SUPV_IST_LEVELS; i++) {
            if (!pc->ist_ssp_phys[i]) continue;
            uintptr_t va = (uintptr_t)vmm_phys_to_virt(pc->ist_ssp_phys[i]);
            if (va) (void)vmm_shadow_stack_unseal(kctx, va);
        }
    }

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

    /* Both writes happen here, while the page is still ordinary
     * writable kernel memory, because after the seal below neither is
     * possible: a normal store faults on R/W = 0, and WRSS is #UD until
     * S_CET.SH_STK_EN is up. The file header carries the SDM quote.
     *
     *   [top]     the supervisor SSP token — its own linear address,
     *             BUSY clear; the processor claims the token, not us.
     *   [top - 8] the pre-pushed entry RIP. The very first
     *             task_restore_context for this process does
     *               push entry_rip; RET
     *             and that RET pops entry_rip from the regular stack AND
     *             from the shadow stack at PL0_SSP = top - 8. Match → no
     *             #CP and the process runs at entry_rip. */
    *(volatile uint64_t *)top         = cet_supv_token_free(top);
    *(volatile uint64_t *)(top - 8)   = (uint64_t)entry_rip;

    /* Seal. A process whose kernel shadow stack could not be sealed must
     * not start: on a core with SH_STK_EN up, its first
     * task_restore_context would make a shadow-stack access to a page
     * the processor does not consider a shadow stack. Hand the frame
     * back and refuse — a refused seal writes nothing (vmm.c checks
     * §4.6 condition (2) before it touches the leaf), so the frame goes
     * back to the PMM exactly as it came out. */
    vmm_context_t *kctx = vmm_get_kernel_context();
    if (!kctx || !vmm_shadow_stack_seal(kctx, va)) {
        kprintf("[CET] proc=%u kernel shadow-stack seal refused at 0x%lx — "
                "process not given a supervisor shadow stack\n",
                proc->pid, (unsigned long)va);
        pmm_free(phys, 1);
        proc->kernel_ssp_phys   = 0;
        proc->kernel_ssp_va_top = 0;
        proc->context.pl0_ssp   = 0;
        return ERR_NO_MEMORY;
    }

    proc->kernel_ssp_phys   = (uintptr_t)phys;
    proc->kernel_ssp_va_top = top;
    proc->context.pl0_ssp   = top - 8;

    debug_printf("[CET] proc=%u kernel SSP allocated: phys=0x%lx top=0x%lx "
                 "pl0_ssp=0x%lx (entry_rip=0x%lx pre-pushed, leaf sealed "
                 "R/W=0 D=1)\n",
                 proc->pid, (unsigned long)(uintptr_t)phys,
                 (unsigned long)top, (unsigned long)(top - 8),
                 (unsigned long)entry_rip);
    return OK;
}

void cet_process_destroy_kernel_ssp(struct process_t *proc)
{
    if (!proc) return;
    if (!proc->kernel_ssp_phys) return;

    /* Unseal before the frame returns to the buddy — the next
     * pmm_alloc_zero writes it through this same kernel window and a
     * sealed leaf would fault inside the allocator. */
    vmm_context_t *kctx = vmm_get_kernel_context();
    if (kctx) {
        uintptr_t va = (uintptr_t)vmm_phys_to_virt(proc->kernel_ssp_phys);
        if (va) (void)vmm_shadow_stack_unseal(kctx, va);
    }

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
