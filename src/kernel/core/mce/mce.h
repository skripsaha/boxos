/*
 * MCE — Machine Check Architecture (x86_64)
 *
 * Production-grade #MC handling for BoxOS:
 *   - BSP + per-AP init enables CR4.MCE, per-bank IA32_MC<i>_CTL,
 *     stale IA32_MC<i>_STATUS clear, optional LMCE (Local MCE) gate.
 *   - #MC handler (vector 18) walks every reporting bank, decodes
 *     VAL/UC/PCC/S/AR/ADDRV per Intel SDM Vol 3B §15.3.2, classifies
 *     severity (CE / UCR / UC), and sets the PMM poison bit on affected
 *     phys pages — the one poisoning that takes no lock. The
 *     `mce:poisoned` MemTag and the console line come afterwards, from a
 *     K-Core off the record's baton, because both of them take locks the
 *     interrupted frame may already hold. The one road where the handler
 *     says the record itself is the road where no K-Core will ever run it:
 *     a kernel-mode fatal verdict, which halts every core.
 *   - Touch publish `mce:fault:detected` / `mce:fault:recovered` /
 *     `mce:fault:fatal` so userspace can observe + react.
 *
 * References:
 *   - Intel SDM Vol 3B §15 (MCA)
 *   - AMD APM Vol 2 §9 (Machine-Check)
 *   - Linux kernel arch/x86/kernel/cpu/mce/ (for Intel bank-0 quirks)
 */

#ifndef MCE_H
#define MCE_H

#include "ktypes.h"
#include "error.h"
#include "idt.h"

/* ─── MSRs (Intel SDM Vol 3B Table 15-1) ───────────────────────────── */

#define MCE_MSR_MCG_CAP              0x179U
#define MCE_MSR_MCG_STATUS           0x17AU
#define MCE_MSR_MCG_CTL              0x17BU
#define MCE_MSR_MC_CTL_BASE          0x400U   /* +4*i */
#define MCE_MSR_MC_STATUS_BASE       0x401U
#define MCE_MSR_MC_ADDR_BASE         0x402U
#define MCE_MSR_MC_MISC_BASE         0x403U
#define MCE_MSR_MC_CTL2_BASE         0x280U   /* +i  — CMCI threshold/enable */

#define MCE_MSR_IA32_FEATURE_CONTROL 0x3AU
#define MCE_MSR_FEAT_LOCK_BIT        (1ULL << 0)
#define MCE_MSR_FEAT_LMCE_BIT        (1ULL << 20)

/* ─── IA32_MCG_CAP layout (Intel SDM Vol 3B §15.3.1.1) ──────────────── */

#define MCE_CAP_COUNT_MASK           0xFFU       /* bits 7:0 — bank count */
#define MCE_CAP_MCG_CTL_P            (1ULL <<  8) /* IA32_MCG_CTL present */
#define MCE_CAP_MCG_EXT_P            (1ULL <<  9) /* extended MSRs present */
#define MCE_CAP_MCG_CMCI_P           (1ULL << 10) /* CMCI supported */
#define MCE_CAP_MCG_TES_P            (1ULL << 11) /* threshold-based status */
#define MCE_CAP_MCG_SER_P            (1ULL << 24) /* Software Error Recovery */
#define MCE_CAP_MCG_LMCE_P           (1ULL << 27) /* Local MCE supported */

/* ─── IA32_MCG_STATUS layout (Vol 3B §15.3.1.2) ─────────────────────── */

#define MCE_MCG_STATUS_RIPV          (1ULL << 0)  /* RIP valid (restartable) */
#define MCE_MCG_STATUS_EIPV          (1ULL << 1)  /* RIP at fault */
#define MCE_MCG_STATUS_MCIP          (1ULL << 2)  /* MCE in progress */
#define MCE_MCG_STATUS_LMCE_S        (1ULL << 3)  /* Local MCE signalled */

/* ─── IA32_MC<i>_STATUS layout (Vol 3B §15.3.2.2) ───────────────────── */

#define MCE_MC_STATUS_VAL            (1ULL << 63) /* register valid */
#define MCE_MC_STATUS_OVER           (1ULL << 62) /* overflow (more errors) */
#define MCE_MC_STATUS_UC             (1ULL << 61) /* uncorrected */
#define MCE_MC_STATUS_EN             (1ULL << 60) /* error reporting enabled */
#define MCE_MC_STATUS_MISCV          (1ULL << 59) /* MISC register valid */
#define MCE_MC_STATUS_ADDRV          (1ULL << 58) /* ADDR register valid */
#define MCE_MC_STATUS_PCC            (1ULL << 57) /* Processor Context Corrupt */
#define MCE_MC_STATUS_S              (1ULL << 56) /* Signalled */
#define MCE_MC_STATUS_AR             (1ULL << 55) /* Action Required */

#define MCE_MAX_BANKS                64u  /* SDM architectural max = 32, +slack */

/* ─── Severity classification ──────────────────────────────────────── */

typedef enum {
    MCE_SEV_NONE      = 0,  /* VAL=0 — bank reports nothing */
    MCE_SEV_CORRECTED = 1,  /* UC=0  — hardware already fixed */
    MCE_SEV_UCR       = 2,  /* UC=1, PCC=0, S=1 — recoverable */
    MCE_SEV_UC        = 3,  /* UC=1, PCC=1 — fatal, context corrupt */
} mce_severity_t;

/* ─── Public API ───────────────────────────────────────────────────── */

/* BSP-side init. Call EARLY in main.c — after cpu_detect_features so
 * has_pat / CPUID feature snapshot is populated, and after vmm_init so
 * the bank-status MSR-based dump uses a valid Pull Map for any
 * subsequent kprintf. Idempotent. */
void          mce_init(void);

/* Per-AP init. Call from per_core_init_ap AFTER cpu_intersect_features_ap
 * so an E-core that the BSP detected as missing MCA isn't fed a #GP via
 * a per-bank CTL write. Mirrors mce_init minus IDT registration. */
void          mce_ap_init(void);

/* #MC handler — called from idt.c exception_handler when vector == 18.
 * Runs in abort context on the IST stack: for as long as MCIP is set it
 * takes no lock, allocates nothing and says nothing (see mce.c's file
 * header).
 *
 * Returns true — the caller IRET-restarts — ONLY when all three hold:
 *   - MCG_STATUS.MCIP = 1 (this handler is standing in the event the
 *     processor is reporting);
 *   - MCG_STATUS.RIPV = 1. SDM Vol 3B §15.10.4.1: "If the RIPV flag is not
 *     set, program execution cannot be restarted reliably." This is a
 *     property of the event and of THIS logical processor, so it outranks
 *     any single bank's severity;
 *   - every bank record it decoded was recoverable (UC=0, or UC=1 with
 *     PCC=0).
 *
 * An empty walk — no bank with VAL=1 — is deliberately NOT a verdict of its
 * own. Without LMCE every Intel #MC is broadcast to every logical processor
 * and only one of them wins the shared bank, so an empty walk is the normal
 * answer on every core but one; calling it fatal would let a recoverable
 * memory error take the machine down. Linux's "Fatal machine check from
 * unknown source" is a monarch's verdict after a rendezvous this kernel does
 * not have. The empty walk is said out loud and judged by RIPV alone.
 *
 * Returns false otherwise, and the caller then falls through to the
 * generic exception path: kill the process from ring 3, halt from ring 0.
 *
 * Walks every bank, sets the PMM poison bit on affected pages, queues their
 * migration, publishes Touch events, and delivers one record per decoded
 * bank: to a K-Core, which says it and applies the `mce:poisoned` tag, or —
 * when the verdict is fatal in kernel mode and every core is about to halt
 * without draining anything — from the handler itself. */
bool          mce_handle(interrupt_frame_t *frame);

/* True after mce_init has completed (so callers know whether per-AP
 * init is required, etc.). */
bool          mce_is_initialized(void);

/* Bank count reported by IA32_MCG_CAP. Useful for telemetry. */
uint32_t      mce_bank_count(void);

/* True if LMCE is enabled (Intel SDM §15.3.1.4). */
bool          mce_lmce_enabled(void);

#endif /* MCE_H */
