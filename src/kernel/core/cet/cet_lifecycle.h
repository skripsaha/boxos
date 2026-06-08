/*
 * CET — Control-flow Enforcement Technology lifecycle (Phase 2K+).
 *
 * Intel SDM Vol 1 §17, Vol 3A §7.4 + §4.5, Vol 3D §17. Production-grade
 * CR4.CET enablement, IA32_S_CET / IA32_U_CET MSR programming, XSAVE
 * component 11 (CET_S) + 12 (CET_U) registration, per-process shadow
 * stack allocation + lifecycle.
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
 *        - register XSAVE components 11 + 12 via
 *          fpu_xsave_register_extension (grows the per-process FPU area
 *          so XSAVE save/restore covers CET_S + CET_U) — must run
 *          before any process spawn.
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
 *   - cet_process_create allocates the user shadow-stack page (4 KiB,
 *     matches user stack depth budget for first-launch processes) and
 *     marks it with the user-SS PTE bit (Intel SDM Vol 3A §4.5.1).
 *     Stores the SSP value in process_t (user_ssp_va / user_ssp_phys).
 *   - cet_process_destroy frees the page on process teardown.
 *   - First iretq to user mode loads IA32_PL3_SSP via WRMSR (handled
 *     in jump_to_userspace); subsequent context switches restore
 *     PL3_SSP from the XSAVE area's CET_U component (no kernel work).
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
 *   - fpu_init             (XSAVE area exists so we can grow it)
 * Must run BEFORE any process_create — XSAVE component growth must
 * settle into g_xsave_area_size before process FPU areas allocate.
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
 *   4. Sets the supervisor PTE.bit60 (VMM_PTE_CET_SS_SUPV) on every
 *      SSP page so RDSSP/INCSSP recognise them as shadow-stack memory.
 *   5. Writes the supervisor SSP token (SSP|0x1) at the top of each
 *      SSP page (Intel SDM Vol 1 §17.2.3).
 *   6. WRMSRs IA32_PL0_SSP + IA32_INTERRUPT_SSP_TABLE_ADDR.
 *   7. Records the bookkeeping in PerCoreData[core_idx].
 *
 * S_CET.SH_STK_EN stays 0 — flipping it requires a kernel-wide audit of
 * every assembly RET / IRET / LRET to ensure they pair with matching
 * CALL / interrupt-frame-push on the shadow stack. The audit is a
 * follow-up commit; until then this infrastructure is dormant but
 * complete (one S_CET MSR rewrite is enough to activate).
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

/* Per-process shadow-stack alloc. Called from process_create after the
 * user VA layout is fixed. Allocates a 4 KiB page from PMM, maps it
 * into the process's vmm_context with VMM_FLAG_PRESENT |
 * VMM_FLAG_WRITABLE | VMM_PTE_CET_SS_USER + VMM_FLAG_USER (Intel SDM
 * Vol 3A §4.5.1 — bit 61 marks user shadow stack). Records the SSP in
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
 * CALL pushes its return address to *PL3_SSP--. Subsequent kernel ↔ user
 * crossings save/restore PL3_SSP through XSAVE component 12 (CET_U)
 * without further WRMSR — the XSAVE bitmap was opted into during BSP
 * init via fpu_xsave_register_extension. */
void cet_load_user_ssp_for_iretq(struct process_t *proc);

/* Increments the #CP fault counter — called from idt.c paranoid vector
 * handler. Visible via cet_lifecycle_dump / shell `hw cet status`. */
void cet_lifecycle_record_cp_fault(void);

/* Telemetry. */
typedef struct {
    bool     enabled;            /* CR4.CET=1 + at least one of SHSTK/IBT */
    bool     shstk_active;
    bool     ibt_active;
    bool     xsave_cet_s;        /* XCR0 bit 11 published */
    bool     xsave_cet_u;        /* XCR0 bit 12 published */
    uint64_t s_cet_msr;          /* IA32_S_CET — supervisor controls */
    uint64_t u_cet_msr;          /* IA32_U_CET — user controls */
    uint64_t cp_faults;          /* count of #CP exceptions observed */
    uint64_t ssp_allocs;         /* per-process SSP allocations */
    uint64_t ssp_frees;          /* per-process SSP frees */
} cet_lifecycle_stats_t;

void cet_lifecycle_get_stats(cet_lifecycle_stats_t *out);

void cet_lifecycle_dump(void);

#endif /* CET_LIFECYCLE_H */
