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
 *     pages — CET_USER_SSP_PAGES) and maps it R/W=0 + Dirty=1 + U/S=1,
 *     the encoding that makes a page a user shadow stack. The 8 MiB
 *     user data stack it accompanies admits 524288 frames at the ABI's
 *     16-byte minimum; 16 KiB of shadow stack holds 2048 slots, so on CET
 *     hardware a deep recursion ends at the shadow stack, by a push that
 *     faults in the guard page below the region rather than corrupting a
 *     neighbour. Whether to grow it, or to demand-page it, is the owner's
 *     call. Counted in full at CET_USER_SSP_PAGES in cet_lifecycle.c.
 *     Stores the live SSP in process_t (user_ssp_va / user_ssp_phys);
 *     user_ssp_va starts at the top of the region and thereafter tracks
 *     whatever the processor last had in IA32_PL3_SSP for this strand.
 *     The region's BASE is never read back out of it — that is
 *     process_user_ssp_va_for(proc), which teardown uses.
 *   - cet_process_destroy frees the pages on process teardown.
 *   - IA32_PL3_SSP is swapped per strand by the pair
 *     cet_save_user_ssp_after_entry / cet_load_user_ssp_for_iretq. The
 *     scheduler's frame dispatch calls both — context_save_from_frame
 *     and context_restore_to_frame in scheduler/context_switch.c, beside
 *     the per-strand FS-base swap that has the same shape and the same
 *     reason — and process_start_initial calls the load half alone, to
 *     prime the MSR for the initial process's first iretq on the
 *     single-core boot branch before the scheduler has run.
 *     The swap has to be the kernel's own work: CET_U is an IA32_XSS
 *     supervisor state component (Intel SDM Vol 1 §13.5), this kernel
 *     programs no IA32_XSS and issues no XSAVES, so no FPU-context path
 *     carries the register. U_CET.SH_STK_EN is set unconditionally
 *     whenever the CPU has SHSTK (see the U policy in cet_lifecycle.c),
 *     so without the swap every user strand on a multicore boot — not
 *     just the second one — would enter ring 3 on the previous strand's
 *     SSP or on the reset value 0. That was a boot-killer on SHSTK
 *     silicon, and it is what this pair closes.
 *
 * Codegen compatibility
 * ---------------------
 *   - Kernel + boxlib + utils are compiled with -fcf-protection=full
 *     (Makefile CFLAGS). GCC emits ENDBR64 at every indirect-call
 *     target — ENDBR64 is a NOP without CR4.CET=1, so the flag is
 *     safe on hardware that doesn't advertise CET.
 *   - Handwritten near RETs live in exactly two of the kernel's asm
 *     files: kernel_entry.asm (7) and vmm_la57.asm (2). isr.asm and
 *     notify_entry.asm hold none — they leave through IRETQ, which is
 *     a separate mechanism. Those RETs are audited to pair with a CALL,
 *     required under SHSTK when CR4.CET=1; a RET reached via JMP would
 *     #CP the kernel and panic. The same audit covers FAR returns,
 *     which are checked against three shadow-stack words rather than
 *     one (Intel CET Specification 334525-003 §2.4), so CS reloads use
 *     a far JMP through memory instead: gdt.c gdt_load_asm, per_core.c
 *     per_core_load_gdt and — since the far RET there was removed —
 *     vmm_la57.asm. One far RET is left in the tree, kernel_entry.asm's
 *     load_gdt, and nothing calls it: the symbol is global and
 *     unreferenced, so no core can execute that RET.
 *     ‼ Those two inline-asm far JMPs are clean on the shadow-stack
 *     side and NOT clean on the IBT side: the `1:` landing label in
 *     gdt.c and in per_core.c carries no ENDBR64, while a far JMP arms
 *     the tracker (334525-003 §3.3.2 Case 1, and §3.2: the subroutine
 *     invoked by a far call/jmp must begin with an endbranch), so with
 *     S_CET.ENDBR_EN up that is #CP(ENDBRANCH). Both run during core
 *     bring-up before this file turns CET on for that core, and that
 *     ordering is the only reason they are dormant. The compiler does
 *     not emit ENDBR64 at an inline-asm label; it has to be written
 *     there. Open finding against those two files, not fixed here.
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
 *   4. Writes the supervisor shadow-stack token at the top of each SSP
 *      page: the token's own linear address with bit 0 (BUSY) CLEAR.
 *      Bit 0 is the busy flag, not a supervisor/user selector — the
 *      processor sets it when it claims the stack (SETSSBSY, or an event
 *      delivery to CPL 0 that switches shadow stacks) and faults if it
 *      finds it already set. This happens BEFORE step 5, because a
 *      sealed page takes no store and WRSS is #UD until SH_STK_EN.
 *   5. Seals every SSP page via vmm_shadow_stack_seal — R/W=0 with
 *      Dirty=1, the encoding the processor honours. The ISST page is
 *      NOT sealed; it is read with ordinary loads. A seal that is
 *      refused hands every frame back and returns ERR_NO_MEMORY, so no
 *      core ever activates against pages that are not shadow stacks.
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
 * for `proc` and maps it into the process's own BUNK — the kernel
 * shadow-stack row at VMM_SHADOW_BUNK_BASE, where the bunk is the pid
 * and the page is born R/W=0 + Dirty=1 on a 4 KiB leaf. Before the map
 * it writes the supervisor SSP token (the BUNK's own address, BUSY
 * clear) at the page top and PRE-PUSHES `entry_rip` one slot below it.
 * Sets proc->kernel_ssp_phys / kernel_ssp_va_top and
 * proc->context.pl0_ssp = top - 8.
 *
 * Both writes are plain stores through the frame's Pull Map alias and
 * both precede the bunk mapping. They have to: a page carrying the
 * encoding rejects normal stores, and WRSS — the instruction that
 * exists for writing shadow stacks — is #UD at CPL < 3 while
 * IA32_S_CET.SH_STK_EN is 0, which is exactly the window this function
 * runs in. Intel SDM Vol 2D, WRSSD/WRSSQ; the quotation is in the
 * cet_lifecycle.c header.
 *
 * vmm_shadow_stack_seal is still called on the bunk, and it changes
 * nothing: it is there for the half of SDM Vol 3A §4.6 a leaf cannot
 * express — R/W = 1 in every entry above it. A refusal is reported and
 * the process runs without a supervisor shadow stack.
 *
 * Called twice for the same process it refuses the second call with
 * ERR_ALREADY_MAPPED and leaves the first stack exactly as it was: the
 * bunk is the pid, so there is no second bunk to give, and taking a
 * frame only to hand it back would be the noisy way to say so.
 *
 * Why pre-push? For a dispatcher that enters a process by `push
 * entry_rip; RET` on its own kernel shadow stack: the regular-stack pop
 * gives entry_rip, the shadow-stack pop gives the value at PL0_SSP —
 * which the pre-push made equal to entry_rip. Mismatch → #CP. It is the
 * canonical "first frame" pattern (Linux uses the same shape for fork()
 * shadow-stack hand-off).
 *
 * ‼ NOTHING LOADS proc->context.pl0_ssp TODAY. The live scheduler
 * dispatch is the IRQ-frame path — context_save_from_frame /
 * context_restore_to_frame in scheduler/context_switch.c — and the only
 * CET MSR it swaps is the USER one, IA32_PL3_SSP; the supervisor side is
 * untouched there. The asm entry points that once swapped
 * IA32_PL0_SSP per process were dead code and have been removed. Every
 * ring-3 → ring-0 entry therefore takes its supervisor SSP from
 * IA32_PL0_SSP, which holds THIS CORE's token from
 * cet_lifecycle_init_supervisor_ssp, and the page this function
 * allocates and frees is never seen by the processor. What it costs is
 * now one frame and one PTE for the life of the process: the bunk row
 * has no huge leaf to demote, so a spawn no longer degrades the Pull
 * Map's granularity, which is what it used to do and never undo.
 * Whether the frame path gains a per-process PL0_SSP swap or this
 * per-process kernel shadow stack goes away is still the owner's call.
 *
 * ‼ TWO THINGS HAVE TO BE ANSWERED BEFORE IT BECOMES LIVE, and both are
 * the owner's, not this file's:
 *
 *   1. The writable Pull Map alias. The frame is a shadow stack at its
 *      bunk and ordinary writable kernel memory at PULL_MAP_BASE +
 *      phys, because removing or sealing that alias is the very huge-
 *      leaf demotion the row exists to avoid. Dormant this protects
 *      nothing and costs nothing; live it would be a supervisor shadow
 *      stack a stray kernel write could still reach by its other name.
 *      The three ways out are spelled out in vmm.c above
 *      vmm_shadow_bunk_va.
 *
 *   2. The page's layout does not match the hardware's way in. Both
 *      hardware entries to a supervisor shadow stack read the token AT
 *      the address in IA32_PL0_SSP (Intel SDM Vol 3A §6.12.1.1 for a
 *      ring transition, Vol 2B SETSSBSY Operation for the instruction).
 *      This page has the token at `top` and PL0_SSP at `top - 8` — the
 *      pre-push discipline above, which is a different, self-consistent
 *      arrangement that no hardware path enters. Whichever discipline
 *      this stack is meant to serve, loading the MSR without choosing
 *      one first would #CP on the first entry.
 *
 * Returns OK on success, AND on a CPU where CET is dormant — there the
 * three SSP fields are left zero and the process runs without a
 * per-process supervisor shadow stack, which is correct while
 * SH_STK_EN is also off. Refusals, each of them reported on the
 * console by the code that found it: ERR_INVALID_ARGUMENT for a null
 * proc, ERR_ALREADY_MAPPED for a process that already holds one,
 * ERR_INVALID_PID for a pid the bunk row has no bunk for,
 * ERR_NO_MEMORY when no frame is to be had, ERR_INVALID_ADDRESS when
 * the frame has no Pull Map address to be seeded through,
 * ERR_MAPPING_FAILED when the row's mapper refused, and
 * ERR_PERMISSION_DENIED_MEM when an entry above the leaf denies the
 * bunk shadow-stack rights. Every one of them leaves the process in
 * the CET-dormant shape — three zeroed fields, no frame, no bunk.
 *
 * Must be called AFTER proc->context.rip is fixed (entry_rip has to be
 * that same value) and AFTER cet_lifecycle_init_bsp /
 * cet_lifecycle_init_supervisor_ssp on the calling CPU. */
error_t cet_process_create_kernel_ssp(struct process_t *proc, uintptr_t entry_rip);

/* Free the per-process kernel SSP page. Idempotent on a process with no
 * allocated SSP. Mirrors cet_process_destroy semantics. */
void cet_process_destroy_kernel_ssp(struct process_t *proc);

/* ─── Activation handshake — last action before scheduler / idle ───
 *
 * Flips IA32_S_CET.SH_STK_EN = 1 and JMPs to `target` without ever
 * returning. Caller MUST be unable to return
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
 *   - Every RET in kernel code must pair with a CALL, and no far RET may
 *     be reached without a far CALL (Intel CET Specification 334525-003
 *     §2.4 — a far RET pops CS, LIP and SSP and faults
 *     #CP(FAR-RET/IRET) on a mismatch)
 *   - Nothing swaps IA32_PL0_SSP per process: this core keeps the token
 *     cet_lifecycle_init_supervisor_ssp put in the MSR. The frame
 *     dispatch that replaced the asm context switch swaps IA32_PL3_SSP
 *     per strand and nothing else, which is correct for this design:
 *     the kernel call chain a strand raises on entry unwinds on the same
 *     core before the iretq that leaves it, so one supervisor shadow
 *     stack per CORE stays balanced
 */
__attribute__((noreturn))
void cet_supv_shstk_activate_and_jump(void (*target)(void));

/* Per-process shadow-stack alloc. Called from process_create after the
 * user VA layout is fixed. Allocates CET_USER_SSP_PAGES (4) contiguous
 * pages from PMM — 16 KiB — and maps them into the process's
 * vmm_context with VMM_FLAGS_SHADOW_STACK_USER: PRESENT | DIRTY | USER
 * and NOT WRITABLE, which is what makes a page a user shadow stack
 * (Intel SDM Vol 3A §4.6, quoted in vmm.h). No seal step and no token:
 * this is the pages' first mapping in that context, so they are born
 * with the encoding, and a user shadow stack carries nothing until its
 * first CALL. Records the SSP in proc->user_ssp_va / user_ssp_phys /
 * user_ssp_size. The mapping goes at process_user_ssp_va_for(proc) and
 * the initial SSP is that base + size - 8, so the first CALL has room to
 * push; user_ssp_va then follows the live register from there.
 *
 * Safe to call when CET is unsupported (sets ssp fields to 0 and
 * returns OK — process_create can proceed without CET). */
error_t cet_process_create(struct process_t *proc);

/* Free the per-process SSP on process teardown. Idempotent on already-
 * freed / never-allocated processes. */
void cet_process_destroy(struct process_t *proc);

/* True when cet_lifecycle_init_bsp completed and CR4.CET is set. The
 * shadow-stack-related paths (the IA32_PL3_SSP swap below) gate on this
 * rather than on g_cpu_caps directly so a future runtime disable still
 * works. */
bool cet_is_enabled(void);

/* ─── IA32_PL3_SSP, saved out of a strand and loaded back into one ───
 *
 * The processor keeps ONE IA32_PL3_SSP per logical processor: it stores
 * the live SSP there when an event takes the core out of CPL 3, and
 * reloads SSP from it on the IRET back (Intel CET Specification
 * 334525-003 §6.2 / §6.3). That covers a ring crossing; it does not
 * cover a ring crossing that comes back up as a DIFFERENT strand, which
 * is what the scheduler's frame dispatch does. These two close that gap,
 * and they are the only writers/readers of the MSR in the kernel.
 *
 * cet_save_user_ssp_after_entry records the register into `proc`. Valid
 * only after a ring-3 → ring-0 entry on this core, which is where the
 * scheduler calls it (context_save_from_frame). It accepts the register
 * only when it points inside THAT strand's own shadow-stack region, so a
 * strand that has not been to ring 3 since its SSP was installed keeps
 * the SSP it was given instead of inheriting a neighbour's.
 *
 * cet_load_user_ssp_for_iretq installs `proc`'s SSP so the next iretq to
 * ring 3 runs on it. Two callers: context_restore_to_frame, and
 * process_start_initial immediately before jump_to_userspace. The latter
 * MUST hold interrupts off (CLI) — between the WRMSR and the iretq there
 * must be no possibility of a switch onto a different process.
 *
 * A strand with NO shadow stack (cet_process_create degraded under
 * memory pressure, user_ssp_va == 0) gets 0 written, not the register
 * left alone: U_CET.SH_STK_EN is up on all SHSTK silicon, so leaving it
 * alone would have that strand push into the previous strand's shadow
 * stack. 0 makes its first CALL fault inside itself instead.
 *
 * Both are no-ops when CET is dormant (g_cet_enabled=false, or the CPU
 * has no SHSTK) — no RDMSR and no WRMSR is issued at all. */
void cet_save_user_ssp_after_entry(struct process_t *proc);
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
