/*
 * CET — Control-flow Enforcement Technology lifecycle (Phase 2K+).
 *
 * Intel SDM Vol 1 "Control-flow Enforcement Technology (CET)" (§18 in
 * the current numbering, §17 in the revision this file was written
 * against — cite the title, the number moves between revisions) and
 * Vol 3A "Control-flow Enforcement Technology". Production-grade
 * CR4.CET enablement, IA32_S_CET / IA32_U_CET MSR programming,
 * per-process shadow stack allocation + lifecycle.
 *
 * Why CET matters in production
 * -----------------------------
 *   - SHSTK (Shadow Stack): the CPU maintains a separate, kernel-
 *     protected stack mirroring the call stack. CALL pushes to both;
 *     RET pops both and compares. Mismatch → #CP (vector 21) — the
 *     canonical defence against ROP attacks.
 *   - IBT (Indirect Branch Tracking): every indirect call target must
 *     start with ENDBR64. Jump-oriented programming becomes impossible
 *     without a matching ENDBR64 — the canonical defence against JOP /
 *     COP attacks.
 *
 * Hardware availability per Intel: Tiger Lake (2020) + adds CET to
 * client CPUs; Sapphire Rapids (2023) adds it to server CPUs. AMD
 * Zen 3 (2020) adds shadow stacks; Zen 4 (2022) adds full CET.
 *
 * Boot flow
 * ---------
 *   1. cpu_detect_features sets g_cpu_caps.has_shstk / has_ibt from
 *      CPUID.07H.0:ECX[7] and EDX[20] respectively (existing).
 *   2. vmm_cet_probe logs the BSP state (existing observe-only).
 *   3. cet_lifecycle_init_bsp (this file):
 *        - probe whether this CPU offers the CET XSAVE state components
 *          (CPUID.(EAX=0DH,ECX=1):ECX bits 11 and 12). They are
 *          SUPERVISOR state components, reached only through IA32_XSS and
 *          XSAVES — never through XCR0. BoxOS programs no IA32_XSS, so
 *          the probe is telemetry, not an enable: no CET register is
 *          carried by the FPU context-switch path.
 *        - enable CR4.CET=1 if any of (has_shstk, has_ibt) detected.
 *        - program IA32_S_CET + IA32_U_CET with SH_STK_EN | ENDBR_EN |
 *          NO_TRACK_EN bits per the kernel's CET policy.
 *        - publish cet:enabled / shstk:enabled / ibt:enabled Touch.
 *   4. cet_lifecycle_init_ap mirrors the BSP enable per-AP after
 *      cpu_intersect_features_ap so hybrid CPUs get a coherent
 *      package-wide state.
 *
 * Per-process lifecycle
 * ---------------------
 *   - cet_process_create allocates the user shadow stack (16 KiB, four
 *     pages — CET_USER_SSP_PAGES — matching the 16 KiB user data stack so
 *     any depth the data stack can reach is mirrored) and sets
 *     VMM_PTE_CET_SS_USER (bit 61) on it, which marks nothing to the
 *     CPU — see the open-defect note at the top of cet_lifecycle.c.
 *     Stores the SSP value in process_t (user_ssp_va / user_ssp_phys).
 *   - cet_process_destroy frees the pages on process teardown.
 *   - The iretq to user mode loads IA32_PL3_SSP via WRMSR
 *     (cet_load_user_ssp_for_iretq, called from process.c).
 *     ‼ Nothing reloads it afterwards. CET_U is an IA32_XSS state
 *     component and this kernel does not enable IA32_XSS, so XSAVE
 *     does NOT carry IA32_PL3_SSP across a context switch. On silicon
 *     where U_CET.SH_STK_EN is live, the second process to return to
 *     ring 3 on a given core runs on whatever SSP the previous one
 *     left. Closing that needs a PL3_SSP reload on every user return,
 *     which lives in the context-switch path, not in this file.
 *
 * Codegen compatibility
 * ---------------------
 *   - Kernel + boxlib + utils are compiled with -fcf-protection=full
 *     (Makefile CFLAGS). GCC emits ENDBR64 at every indirect-call
 *     target — ENDBR64 is a NOP without CR4.CET=1, so the flag is
 *     safe on hardware that doesn't advertise CET.
 *   - Handwritten asm RETs (context_switch.asm, isr.asm, notify_entry
 *     .asm) are audited to ensure they pair with a CALL — required
 *     under SHSTK when CR4.CET=1. Any RET reached via JMP would #CP
 *     the kernel and panic.
 *
 * QEMU TCG does NOT advertise SHSTK/IBT under -cpu max as of QEMU
 * 10.x — the cet_lifecycle paths stay dormant on TCG (g_cpu_caps
 * gates everything). Real-HW verification belongs to the physical
 * boot test (must-implement item #4).
 */

#ifndef CET_LIFECYCLE_H
#define CET_LIFECYCLE_H

#include "ktypes.h"
#include "error.h"

struct process_t;

/* Init — BSP. Idempotent. Safe to call when CET is unsupported (no-op).
 * Must run AFTER:
 *   - cpu_detect_features  (g_cpu_caps.has_shstk / has_ibt populated)
 *   - vmm_cet_probe        (cet:fault:cp Touch tag pre-resolved)
 *   - MemTagInit + SeedReservedTags (cet:enabled etc. reserved)
 * Must run BEFORE any process_create — cet_process_create gates on the
 * CR4.CET state this function establishes, so a process spawned earlier
 * would silently get no shadow stack.
 *
 * Returns OK on success or ERR_UNSUPPORTED when the CPU lacks both
 * SHSTK and IBT (still safe to call). */
error_t cet_lifecycle_init_bsp(void);

/* Init — per-AP. Mirrors the BSP enable on the calling AP after
 * cpu_intersect_features_ap so a hybrid CPU's CET-less AP doesn't get
 * CR4.CET=1 (would #UD on the first WRMSR to IA32_S_CET). Idempotent. */
void cet_lifecycle_init_ap(void);

/* Per-CPU supervisor shadow-stack infrastructure (PL0_SSP + ISST).
 *
 * Intel SDM Vol 3D §17.2.3 + §17.6: the supervisor shadow stack is
 * loaded from IA32_PL0_SSP on interrupt delivery to CPL=0 with IST=0,
 * and from IA32_INTERRUPT_SSP_TABLE_ADDR[i] when the interrupt's IST
 * field is i (1..7). Both MSRs must hold valid SSP token addresses
 * BEFORE S_CET.SH_STK_EN flips to 1, else the first kernel RET / IST
 * delivery faults with #CP and the kernel triple-faults.
 *
 * This function:
 *   1. Allocates a PL0_SSP page (4 KiB) per core.
 *   2. Allocates an IA32_INTERRUPT_SSP_TABLE_ADDR backing page (4 KiB),
 *      populates ISST[0]=PL0_SSP, ISST[1..5]=per-IST SSPs.
 *   3. Allocates 5 per-IST SSP pages (one each for IST 1..5 — #DF / NMI /
 *      #MC / #DB / #SS in BoxOS).
 *   4. Sets PTE bit 60 (VMM_PTE_CET_SS_SUPV) on every SSP page. ‼ That
 *      does NOT make them shadow-stack memory — in ordinary paging bit
 *      60 is protection-key bit 1, and the only encoding silicon
 *      honours is R/W=0 with Dirty=1. See the open-defect note at the
 *      top of cet_lifecycle.c.
 *   5. Writes the supervisor shadow-stack token at the top of each SSP
 *      page: the token's own linear address with bit 0 (BUSY) CLEAR.
 *      Bit 0 is the busy flag, not a supervisor/user selector — the
 *      processor sets it when it claims the stack (SETSSBSY, or an event
 *      delivery to CPL 0 that switches shadow stacks) and faults if it
 *      finds it already set.
 *   6. WRMSRs IA32_PL0_SSP + IA32_INTERRUPT_SSP_TABLE_ADDR.
 *   7. Records the bookkeeping in PerCoreData[core_idx].
 *
 * S_CET.SH_STK_EN is still 0 when this function returns. It is flipped
 * later in the same core's bring-up by cet_supv_shstk_activate_and_jump
 * (main.c on the BSP, ap_init.c on every AP), which is why both MSRs must
 * already hold valid token addresses when this function returns.
 *
 * Returns OK on success. ERR_UNSUPPORTED when CET is dormant on this
 * CPU (caller treats as no-op). ERR_NO_MEMORY on alloc failure (the
 * core then runs with no supervisor SSP — S_CET.SH_STK_EN MUST stay 0
 * for this core, else first kernel RET kills it). */
error_t cet_lifecycle_init_supervisor_ssp(uint8_t core_idx);

/* Teardown — frees all supervisor SSP pages allocated by
 * cet_lifecycle_init_supervisor_ssp for `core_idx`. Idempotent on a
 * core whose SSP was never allocated. Provided for completeness +
 * future runtime CET disable; not called from any hot path. */
void cet_lifecycle_release_supervisor_ssp(uint8_t core_idx);

/* ─── Per-process kernel shadow stack ───
 *
 * cet_process_create_kernel_ssp allocates a 4 KiB supervisor SSP page
 * for `proc`, sets PTE bit 60 (VMM_PTE_CET_SS_SUPV) on it — which marks
 * nothing to the CPU, see the note at the top of cet_lifecycle.c — writes
 * the supervisor SSP token (own address, BUSY clear) at the page top,
 * and PRE-PUSHES `entry_rip`
 * one slot below via WRSSQ. Sets proc->kernel_ssp_phys / kernel_ssp_va_top
 * and proc->context.pl0_ssp = top - 8.
 *
 * Why pre-push? When task_restore_context first scheduling this process
 * does `push entry_rip; RET`, the regular-stack pop gives entry_rip;
 * the shadow-stack pop gives the value at PL0_SSP — which we made equal
 * to entry_rip via the WRSSQ. Mismatch → #CP. The pre-push is the
 * canonical "first frame" pattern (Linux uses the same shape for fork()
 * shadow-stack hand-off).
 *
 * Returns OK on success; ERR_UNSUPPORTED when CET is dormant on this CPU
 * (proc gets zero SSP fields and runs without per-process supv SHSTK —
 * acceptable when SH_STK_EN is also off, fatal if it's on); ERR_NO_MEMORY
 * on alloc failure.
 *
 * Must be called AFTER task_init_context (so entry_rip == ctx.rip) and
 * AFTER cet_lifecycle_init_bsp / cet_lifecycle_init_supervisor_ssp on
 * the calling CPU (so CR4.CET=1 + S_CET.WR_SHSTK_EN=1 for WRSSQ to work). */
error_t cet_process_create_kernel_ssp(struct process_t *proc, uintptr_t entry_rip);

/* Free the per-process kernel SSP page. Idempotent on a process with no
 * allocated SSP. Mirrors cet_process_destroy semantics. */
void cet_process_destroy_kernel_ssp(struct process_t *proc);

/* ─── Activation handshake — last action before scheduler / idle ───
 *
 * Flips IA32_S_CET.SH_STK_EN = 1, sets g_cet_supv_active = 1, and JMPs
 * to `target` without ever returning. Caller MUST be unable to return
 * — there's no matching FAR CALL on the shadow stack for any return,
 * so any RET that fires under SH_STK_EN=1 with an empty shadow stack
 * crashes the kernel.
 *
 * The CALLer of this function is the LAST function on the current call
 * chain whose CALL was made before SH_STK_EN flipped. That CALL didn't
 * push to the shadow stack. So this function ABANDONS the call chain
 * (no RET) and JMPs to a forever-running target.
 *
 * Target candidates:
 *   - kcore_run_loop (BSP + AP K-Cores) — already noreturn
 *   - app_core_idle_loop (AP App Cores) — must be noreturn
 *
 * Safe to call when CET is dormant — degrades to direct call into target
 * (target is responsible for never returning either way).
 *
 * After return-via-jump:
 *   - SH_STK_EN=1 globally for this CPU
 *   - Every CALL/RET in `target`'s execution tracks IA32_PL0_SSP
 *   - Context switches use SAVE/RESTORE_PL0_SSP to swap per-process SSP
 *   - New processes created after this point MUST have kernel SSP
 *     allocated via cet_process_create_kernel_ssp, else first
 *     task_restore_context #CPs
 */
__attribute__((noreturn))
void cet_supv_shstk_activate_and_jump(void (*target)(void));

/* Per-process shadow-stack alloc. Called from process_create after the
 * user VA layout is fixed. Allocates CET_USER_SSP_PAGES (4) contiguous
 * pages from PMM — 16 KiB — and maps them into the process's
 * vmm_context with VMM_FLAG_PRESENT |
 * VMM_FLAG_WRITABLE | VMM_PTE_CET_SS_USER + VMM_FLAG_USER. ‼ Bit 61
 * does not mark a user shadow stack in ordinary paging — it is
 * protection-key bit 2 (Intel SDM Vol 3A Table 4-19); a user shadow
 * stack is R/W=0 + Dirty=1 + U/S=1 (Vol 1 §18.2.2). See the open-defect
 * note at the top of cet_lifecycle.c. Records the SSP in
 * proc->user_ssp_va / user_ssp_phys / user_ssp_size. The SSP value
 * starts at user_ssp_va + size - 8 so the first CALL has room to push.
 *
 * Safe to call when CET is unsupported (sets ssp fields to 0 and
 * returns OK — process_create can proceed without CET). */
error_t cet_process_create(struct process_t *proc);

/* Free the per-process SSP on process teardown. Idempotent on already-
 * freed / never-allocated processes. */
void cet_process_destroy(struct process_t *proc);

/* True when cet_lifecycle_init_bsp completed and CR4.CET is set. The
 * shadow-stack-related paths (jump_to_userspace WRMSR IA32_PL3_SSP)
 * gate on this rather than on g_cpu_caps directly so a future runtime
 * disable still works. */
bool cet_is_enabled(void);

/* Program IA32_PL3_SSP with the per-process initial SSP value just
 * before iretq drops to ring 3. The caller MUST hold interrupts off
 * (CLI) — between the WRMSR and the iretq there must be no possibility
 * of a context switch onto a different process. Called from process.c
 * immediately before jump_to_userspace.
 *
 * No-op when:
 *   - CET is dormant (g_cet_enabled=false / no SHSTK on the CPU)
 *   - proc has no allocated SSP (proc->user_ssp_va == 0)
 *
 * After the iretq, the CPU's PL3_SSP is the user-mode SSP. The first
 * CALL pushes its return address to *PL3_SSP--.
 *
 * ‼ Subsequent kernel ↔ user crossings do NOT restore PL3_SSP. CET_U is
 * an IA32_XSS supervisor state component (Intel SDM Vol 1 §13.5), not an
 * XCR0 one, so the kernel's XSAVE/XRSTOR context-switch path cannot carry
 * it and never did — CPUID.(EAX=0DH,ECX=0) does not even report bit 11.
 * Until a PL3_SSP reload is added to the user-return path, a core that
 * has run two processes with user shadow stacks hands the second one the
 * first one's SSP. */
void cet_load_user_ssp_for_iretq(struct process_t *proc);

/* Increments the #CP fault counter — called from idt.c paranoid vector
 * handler. Visible via cet_lifecycle_dump / shell `hw cet status`. */
void cet_lifecycle_record_cp_fault(void);

/* Telemetry. */
typedef struct {
    bool     enabled;            /* CR4.CET=1 + at least one of SHSTK/IBT */
    bool     shstk_active;
    bool     ibt_active;
    /* CPUID.(EAX=0DH,ECX=1):ECX — does the silicon OFFER the CET state
     * components? Both are IA32_XSS components and BoxOS enables no
     * IA32_XSS, so "true" means "available and unmanaged", never
     * "saved on context switch". */
    bool     xsave_cet_s;        /* component 12 (IA32_PL{0,1,2}_SSP) */
    bool     xsave_cet_u;        /* component 11 (IA32_U_CET, PL3_SSP) */
    uint64_t s_cet_msr;          /* IA32_S_CET — supervisor controls */
    uint64_t u_cet_msr;          /* IA32_U_CET — user controls */
    uint64_t cp_faults;          /* count of #CP exceptions observed */
    uint64_t ssp_allocs;         /* per-process SSP allocations */
    uint64_t ssp_frees;          /* per-process SSP frees */
} cet_lifecycle_stats_t;

void cet_lifecycle_get_stats(cet_lifecycle_stats_t *out);

void cet_lifecycle_dump(void);

#endif /* CET_LIFECYCLE_H */
