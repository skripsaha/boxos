/*
 * MCE — Machine Check Architecture (x86_64)
 *
 * Production-grade #MC handling for BoxOS:
 *   - BSP + per-AP init enables CR4.MCE, per-bank IA32_MC<i>_CTL, clears
 *     stale IA32_MC<i>_STATUS, and — on every logical processor whose three
 *     gates allow it — ARMS LMCE (Local MCE) by writing
 *     IA32_MCG_EXT_CTL.LMCE_EN, so that some errors reach one core instead
 *     of all of them.
 *   - #MC handler (vector 18) walks every reporting bank, decodes
 *     VAL/OVER/UC/EN/MISCV/ADDRV/PCC/S/AR and the MCA error code per Intel
 *     SDM Vol 3B §15.3.2 + §15.9.2, classifies severity against
 *     MCG_CAP.MCG_SER_P (CE / UCNA / UCR / UC), and sets the PMM poison bit
 *     on the phys pages the banks can actually NAME — the one poisoning
 *     that takes no lock. The
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
#define MCE_MSR_FEAT_LOCK_BIT        (1ULL << 0)   /* LOCK */
#define MCE_MSR_FEAT_LMCE_BIT        (1ULL << 20)  /* LMCE_ENABLED */

/* IA32_MCG_EXT_CTL (Vol 3B §15.3.1.4). Present only when MCG_CAP.LMCE_P is
 * set, and touchable only once platform firmware has set
 * IA32_FEATURE_CONTROL.LMCE_ENABLED: "If MCG_LMCE_P is not set in
 * IA32_MCG_CAP, or platform software has not enabled LMCE by setting
 * IA32_FEATURE_CONTROL.LMCE_ENABLED (bit 20), any attempt to write or read
 * IA32_MCG_EXT_CTL will result in #GP." Both halves of that sentence bind —
 * the READ is as forbidden as the write — so nothing here reads this MSR
 * before both gates have answered. */
#define MCE_MSR_MCG_EXT_CTL          0x4D0U
#define MCE_MCG_EXT_CTL_LMCE_EN      (1ULL << 0)

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

/* ─── MCA error code, IA32_MC<i>_STATUS[15:0] (Vol 3B §15.9.2) ───────
 *
 * Bit 12 is the threshold-based "filtered" marker (MCG_TES_P), not part of
 * the code, so every comparison mask below leaves it out.
 *
 * The three compound shapes that name a piece of MEMORY — the only ones for
 * which taking a page out of service means anything:
 *   0000 1MMM CCCC       memory controller error  → bit 7 set
 *   000F 0001 RRRR TTLL  cache hierarchy error    → bit 8 set
 *   0000 0000 0000 11LL  generic memory hierarchy error
 * A TLB code, a bus/interconnect code or an internal-timer code names no
 * page at all; the address such a bank happens to carry is not a licence to
 * offline one. */
#define MCE_MCACOD_MASK              0xEFFFULL
#define MCE_MCACOD_MEMCTL_MASK       0xEF80ULL
#define MCE_MCACOD_MEMCTL            0x0080ULL
#define MCE_MCACOD_CACHE_MASK        0xEF00ULL
#define MCE_MCACOD_CACHE             0x0100ULL
#define MCE_MCACOD_MEMGEN_MASK       0xEFFCULL
#define MCE_MCACOD_MEMGEN            0x000CULL

/* The codes Table 15-6's UCR rows call "known MCACOD" — the ones for which
 * a specific recovery action exists. The rows say plainly what to do with
 * the rest: "For unknown MCACOD, must bugcheck."
 *   SRAO (S=1, AR=0): patrol-scrub error, last-level-cache writeback
 *   SRAR (S=1, AR=1): data load, instruction fetch */
#define MCE_MCACOD_SCRUB_MASK        0xEFF0ULL
#define MCE_MCACOD_SCRUB             0x00C0ULL
#define MCE_MCACOD_L3WB              0x017AULL
#define MCE_MCACOD_DATA              0x0134ULL
#define MCE_MCACOD_INSTR             0x0150ULL

/* ─── IA32_MC<i>_MISC layout for UCR errors (Vol 3B §15.3.2.4) ───────
 *
 * These fields exist ONLY when MISCV and IA32_MCG_CAP[24] (MCG_SER_P) are
 * both set. Without SER_P the register is model-specific end to end and
 * nothing in it may be read as a mode or as a shift.
 *   bits 5:0  Recoverable Address LSB — the lowest VALID bit of the address
 *             in IA32_MC<i>_ADDR. Everything below it is noise, so an LSB
 *             above 12 means the bank cannot even name a 4 KiB page.
 *   bits 8:6  Address Mode — which address space ADDR is in at all. */
#define MCE_MC_MISC_ADDR_LSB_MASK     0x3FULL
#define MCE_MC_MISC_ADDR_MODE_SHIFT   6u
#define MCE_MC_MISC_ADDR_MODE_MASK    0x7ULL
#define MCE_MC_MISC_ADDR_MODE_SEGOFF  0u  /* segment offset */
#define MCE_MC_MISC_ADDR_MODE_LINEAR  1u  /* linear address */
#define MCE_MC_MISC_ADDR_MODE_PHYS    2u  /* physical address */
#define MCE_MC_MISC_ADDR_MODE_MEM     3u  /* memory address (rank/row/bank) */
#define MCE_MC_MISC_ADDR_MODE_GENERIC 7u  /* generic */

#define MCE_MAX_BANKS                64u  /* SDM architectural max = 32, +slack */

/* ─── Severity classification ──────────────────────────────────────── */

typedef enum {
    MCE_SEV_NONE      = 0,  /* VAL=0 — bank reports nothing */
    MCE_SEV_CORRECTED = 1,  /* UC=0 — hardware already fixed it */
    MCE_SEV_UCR       = 2,  /* Uncorrected RECOVERABLE. Every one of these
                             * must hold: MCG_CAP.MCG_SER_P=1 (the part
                             * implements software error recovery at all),
                             * UC=1, PCC=0, EN=1, S=1. Table 15-6 then
                             * splits it by AR: SRAO (AR=0, action optional)
                             * and SRAR (AR=1, action required). OVER is not
                             * part of this test — Table 15-6 attaches it to
                             * the SRAR row alone, so it is read there. */
    MCE_SEV_UC        = 3,  /* Fatal. PCC=1; or ANY UC=1 on a part without
                             * MCG_SER_P, where §15.6.1 leaves the handler
                             * nothing but "log and shut the system down";
                             * or a SRAR with OVER=1, whose twin error was
                             * lost and cannot be acted on ("If OVER=1,
                             * reset system"); or a UCR row whose MCACOD
                             * names no recovery this handler has; or a
                             * SRAR on the processor that CONSUMED the
                             * poison, which must not be restarted at the
                             * instruction that consumed it; or the pair S=0
                             * with AR=1, which the handler pseudocode calls
                             * illegal outright — "(* S=0, AR=1 is illegal
                             * *)" — and answers with RESTARTABILITY = FALSE
                             * and NOERROR = FALSE: an action is REQUIRED
                             * for an error the processor says it never
                             * signalled, so no Table 15-6 row names that
                             * action and §15.6.2 leaves only shutdown. */
    MCE_SEV_UCNA      = 4,  /* UC=1, PCC=0, S=0, AR=0 on a SER part: the
                             * hardware logged it and never signalled it to
                             * us — Table 15-6, "Ok to keep the system
                             * running".
                             * An EN=0 entry on a SER part lands here too:
                             * every UCR row requires EN=1, so such an entry
                             * is not the error that raised this #MC and is
                             * not ours to act on. Logged, cleared, and
                             * nothing else. */
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

/* Arm THIS core's corrected-machine-check interrupt: the per-bank
 * IA32_MCi_CTL2 threshold and enable (Intel SDM Vol 3B §16.5.1) and this
 * core's LVT CMCI entry (§16.5, LAPIC offset 0x2F0). Idempotent.
 *
 * ‼ CALL IT AFTER THE LOCAL APIC IS SOFTWARE-ENABLED, not before.
 * mce_init and mce_ap_init both run earlier than that and arm the BANK half
 * on their own, so corrected errors are already being counted; but SDM
 * Vol 3A "Local APIC State After It Has Been Software Disabled" says an
 * unmask written to a software-disabled APIC is ignored, so the INTERRUPT
 * that carries them out is armed only from here. The two call sites are
 * main.c (after irqchip_init) and per_core.c (after lapic_enable). */
void          MceCmciArmLocal(void);

/* #MC handler — called from idt.c exception_handler when vector == 18.
 * Runs in abort context on the IST stack: for as long as MCIP is set it
 * takes no lock, allocates nothing and says nothing (see mce.c's file
 * header).
 *
 * Returns true — the caller IRET-restarts — ONLY when all of these hold:
 *   - MCG_STATUS.MCIP = 1 (this handler is standing in the event the
 *     processor is reporting);
 *   - MCG_STATUS.RIPV = 1. SDM Vol 3B §15.10.4.1: "If the RIPV flag is not
 *     set, program execution cannot be restarted reliably." This is a
 *     property of the event and of THIS logical processor, so it outranks
 *     any single bank's severity;
 *   - MCG_STATUS.EIPV = 0, whenever the walk found nothing uncorrected. SDM
 *     Vol 3B, "Machine-Check Error Handler Pseudocode Supporting UCR": "IF
 *     NOERROR = TRUE THEN IF NOT (MCG_RIPV = 1 AND MCG_EIPV = 0) THEN
 *     RESTARTABILITY = FALSE". A thread that found no uncorrected record and
 *     carries EIPV=1 is the thread the error belongs to — whichever core won
 *     the race for the shared bank cleared it out from under this one — so
 *     RIPV alone must not restart it;
 *   - no bank record it decoded was fatal. Fatal is what §15.6.1 and
 *     Table 15-6 make fatal, not merely PCC: an uncorrected error on a
 *     part without MCG_SER_P; a SRAR with OVER=1 (its twin was lost, so
 *     nobody can say where it was); a record carrying S=0 with AR=1, the
 *     combination the handler pseudocode calls illegal; a UCR row whose
 *     MCACOD names no recovery — "for unknown MCACOD, must bugcheck" — or
 *     whose required action this kernel could not start;
 *   - the #MC was not a SRAR taken on the processor that consumed the
 *     poison. §15.6.2 wants that recovery "completed successfully before
 *     any additional work is scheduled for this processor", and the
 *     migration this kernel performs finishes on a K-Core long after IRET
 *     would return — so the consuming context is not resumed at all. The
 *     processors that merely took the broadcast are told apart from the
 *     consumer by MCG_STATUS the way SDM Table 15-21 says: RIPV=1 with
 *     EIPV=0 is a thread that consumed nothing, and it keeps running.
 *
 * A walk that found nothing uncorrected — no bank with VAL=1, or only
 * corrected and UCNA entries — is deliberately NOT a verdict of its own. An
 * event that is not LMCE is broadcast to every logical processor and only
 * one of them wins the shared bank, so such a walk is the normal answer on
 * every core but one; calling it fatal would let a recoverable memory error
 * take the machine down. Linux's "Fatal machine check from unknown source"
 * is a monarch's verdict after a rendezvous this kernel does not have — see
 * the verdict block in mce.c for the whole argument: what answers the
 * per-core question instead, why no wait on a guessed number is involved in
 * replacing it, and what declining the rendezvous still costs.
 * Such a walk is said out loud and judged by the pair RIPV=1 and EIPV=0.
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

/* True when LMCE is ARMED on every logical processor that has run mce_init
 * or mce_ap_init — including one that left mce_ap_init early for lack of MCA
 * altogether, which arms nothing and so takes this answer back like any
 * other core that could not arm it. The fact of the write, never the
 * permission to make it. Each core asks
 * MCG_CAP.LMCE_P and the two gates of SDM §15.3.1.5 — "System software must
 * ensure that both IA32_FEATURE_CONTROL.Lock (bit 0) and
 * IA32_FEATURE_CONTROL.LMCE_ENABLED (bit 20) are set before attempting to
 * set IA32_MCG_EXT_CTL.LMCE_EN (bit 0)" — then writes LMCE_EN and READS IT
 * BACK; only a read-back carrying the bit counts as armed. One core that
 * cannot arm it takes this answer back to false and says why, because a
 * fleet armed unevenly is a fleet where the handler may still meet a
 * broadcast.
 *
 * True is still not a promise that nothing is broadcast. §15.9.3.1: "If LMCE
 * is supported and enabled, some errors (not limited to UCR errors) may be
 * delivered to only a single logical processor" — SOME, chosen by hardware,
 * and §15.3.1.5 adds that "software should make no assumptions about the
 * type of error that hardware can choose to deliver as LMCE". What says it
 * for one particular event is MCG_STATUS.LMCE_S, which the handler prints on
 * every record; this says only that such events are possible at all. */
bool          mce_lmce_enabled(void);

#endif /* MCE_H */
