/*
 * MCE — Machine Check Architecture implementation
 *
 * See mce.h for spec citations. Code paths:
 *   mce_init()        — BSP bring-up (CR4.MCE, MCG_CAP, per-bank CTL+CLEAR,
 *                       LMCE arming, telemetry)
 *   mce_ap_init()     — same minus IDT
 *   mce_handle(frame) — runs in #MC ABORT context: the IST stack, MCIP=1,
 *                       delivered whatever IF says and whatever the frame it
 *                       interrupted was holding. So for as long as MCIP is
 *                       set it takes NO lock, allocates nothing and says
 *                       nothing. kprintf serialises behind the console lock;
 *                       MemTagApplyByPhys interns the tag string (kmalloc),
 *                       takes the tag registry's global lock, the region
 *                       registry lock and a bucket lock. Any one of them,
 *                       entered on a core that was already inside it, spins
 *                       forever — and a core spinning with MCIP still set
 *                       answers the next machine-check event with the
 *                       architectural shutdown state (SDM §15.3.1.2), a reset
 *                       with nothing said. So while MCIP is set the handler
 *                       only reads MSRs, sets the lock-free PMM poison bit,
 *                       and copies what each bank held into a pre-armed slot.
 *
 *                       MCG_STATUS is cleared at the end of the walk, and the
 *                       records are delivered after that, when this is an
 *                       ordinary core again. Normally delivery is a pass of
 *                       each slot's own baton (baton.h — allocation-free,
 *                       never dropped) to a K-Core, which says the bank line
 *                       and applies the `mce:poisoned` MemTag where locks are
 *                       legal; TouchPublishIrqPair and mce_migrate_request
 *                       ride the same queues. The exception is the road where
 *                       no K-Core will ever run the continuation — a
 *                       kernel-mode fatal verdict, which sends idt.c to
 *                       `cli; hlt` on every core without pumping a baton, and
 *                       the boot window before BatonInit, where a pass is
 *                       refused and the refusal itself would speak from abort
 *                       context. There the handler says the record itself,
 *                       with MCIP already cleared and into the same console
 *                       lock idt.c's panic dump is about to take from this
 *                       very frame. See the block comment above the record
 *                       pool for the full argument.
 */

#include "mce.h"
#include "mce_migrate.h"
#include "klib.h"
#include "cpuid.h"
#include "pmm.h"
#include "memtag.h"
#include "touch.h"
#include "logbook.h"
#include "baton.h"
#include "amp.h"

/* ─── State ────────────────────────────────────────────────────────── */

static bool      g_mce_initialized = false;

/* LMCE is armed per LOGICAL PROCESSOR — IA32_MCG_EXT_CTL is not shared — so
 * this says the FLEET: true only while every core that has run MCA init read
 * IA32_MCG_EXT_CTL.LMCE_EN back as set. The first core that cannot arm it
 * takes it back to false, because one unarmed core is enough to put a
 * broadcast back on the table. */
static bool      g_mce_lmce_armed  = false;
static uint32_t  g_mce_bank_count  = 0;
static uint64_t  g_mce_mcg_cap     = 0;
static uint64_t  g_mce_ban_mask    = 0;  /* bit i set ⇒ bank i CTL skipped */

/* Whether the MCA error code field may be read with Intel's compound-code
 * tables (SDM §15.9.2). Zhaoxin implements the same encoding and Linux
 * decodes it with the same rules; AMD puts the error type in MCA_IPID bank
 * types instead, which this kernel has no table for — see
 * mce_record_names_memory. Sampled once by mce_init, before the handler can
 * run. */
static bool      g_mce_cod_intel   = false;

/* Pre-resolved Touch tag handles. The #MC handler runs on the IST stack
 * (non-maskable, even more restrictive than IRQ context). Per touch.h
 * §IRQ-Publishers, ISR-context code MUST NOT call TouchPublish/TouchPublishId
 * directly because that path can take per-bucket spinlocks + kmalloc the
 * L2 leaf, which deadlocks against any caller that held a Touch lock
 * when the #MC fired. The IRQ-safe variant TouchPublishIrqPair takes
 * pre-resolved tag handles + a bounded payload + queues for K-Core
 * delivery. We resolve once at mce_init time (outside IRQ) and cache
 * the handles here; mce_handle hot-path uses them. */
static TouchTag  g_mce_tag_detected   = TOUCH_TAG_INVALID;
static TouchTag  g_mce_tag_recovered  = TOUCH_TAG_INVALID;
static TouchTag  g_mce_tag_fatal      = TOUCH_TAG_INVALID;

/* Armed by mce_init before the handler can ever run — see the record pool
 * below the severity helpers. */
static void mce_note_pool_arm(void);

/* ─── MSR primitives ──────────────────────────────────────────────── */

static inline uint64_t mce_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void mce_wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFFULL);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

/* ─── CPU feature gating ───────────────────────────────────────────── */

/* CPUID.1:EDX[7]=MCE, [14]=MCA. Both must be set for full MCA. MCE
 * alone (legacy P5/P6 without architecture) is not enough — we refuse
 * to drive a non-MCA system. */
static bool mce_supported_local(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(CPUID_LEAF_FEATURES, &eax, &ebx, &ecx, &edx);
    bool mce = (edx >> 7)  & 1u;
    bool mca = (edx >> 14) & 1u;
    return mce && mca;
}

/* Bank 0 on the parts where IA32_MC0_CTL is not a bank control at all.
 *
 * SDM Vol 3B §15.3.2.1, the NOTE under Figure 15-5: "For P6 family
 * processors, processors based on Intel Core microarchitecture (excluding
 * those on which CPUID reports DisplayFamily_DisplayModel as 06H_1AH and
 * onward): the operating system or executive software must not modify the
 * contents of the IA32_MC0_CTL MSR. This MSR is internally aliased to the
 * EBL_CR_POWERON MSR and controls platform-specific error handling
 * features. System specific firmware (the BIOS) is responsible for the
 * appropriate initialization of the IA32_MC0_CTL MSR."
 *
 * The initialization pseudocode draws the same line and no other: "IF
 * (Processor Family is 6H and Processor EXTMODEL:MODEL is less than 1AH)
 * THEN (* Enable logging of all errors except for MC0_CTL register *) FOR
 * error-reporting banks (1 through MAX_BANK_NUMBER)". So does Linux, whose
 * quirk clears bank 0's `init` below family 6 model 1AH (INTEL_NEHALEM_EP)
 * with the comment "it's not aliased anymore on model 0x1a+ ... merely
 * don't write CTL0".
 *
 * 06H_1AH is the cutoff, and id.model is exactly what it names: the
 * effective DisplayModel, with EXTMODEL already folded in by
 * cpu_read_identity. The cutoff standing here before was model < 0x0E,
 * thirteen models early, so every Core-microarchitecture part in
 * 06_0EH..06_19H — Yonah 0x0E, Merom/Conroe 0x0F, Penryn 0x17 and 0x1D —
 * fell through and had all-ones written into EBL_CR_POWERON: bus-error
 * signalling and response error checking, a register the NOTE says firmware
 * owns, rewritten by the BSP and again by every AP.
 *
 * There was also a family-0FH (NetBurst) arm. The NOTE names P6 and Core
 * only, the pseudocode tests family 6 only, and Linux carries no bank-0
 * quirk for NetBurst — nothing sources it, so it is gone rather than kept
 * on a guess. AMD has no equivalent alias.
 *
 * What this gates is the CTL WRITE alone. mce_program_banks still clears
 * bank 0's IA32_MCi_STATUS on these parts — see the loop for why. */
static bool mce_skip_bank_zero(void) {
    cpu_identity_t id;
    cpu_read_identity(&id);
    if (id.vendor[0] != 'G') return false;  /* "GenuineIntel" check */
    /* Pre-Pentium-Pro families had no MCA; we don't reach here on them. */
    return id.family == 0x06 && id.model < 0x1A;
}

/* Does this vendor encode IA32_MC<i>_STATUS[15:0] with Intel's compound
 * error codes? Intel does by definition; Zhaoxin/Centaur clone the same
 * encoding and Linux dispatches both to the same decoder. AMD and Hygon do
 * not — modern AMD names the error through the SMCA bank type in MCA_IPID,
 * which this kernel does not read, so their codes are left undecoded rather
 * than run through a table that was not written for them. */
static bool mce_vendor_uses_intel_codes(void) {
    cpu_identity_t id;
    cpu_read_identity(&id);
    return __builtin_memcmp(id.vendor, "GenuineIntel", 12) == 0 ||
           __builtin_memcmp(id.vendor, "CentaurHauls", 12) == 0 ||
           __builtin_memcmp(id.vendor, "  Shanghai  ", 12) == 0;
}

/* ─── CR4.MCE bit toggle ──────────────────────────────────────────── */

#define CR4_MCE_BIT (1ULL << 6)

static inline void mce_cr4_enable(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (!(cr4 & CR4_MCE_BIT)) {
        cr4 |= CR4_MCE_BIT;
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }
}

/* ─── LMCE arming (Intel SDM Vol 3B §15.3.1.4, §15.3.1.5) ──────────── */

/* Arm Local Machine Check on THIS logical processor, and answer with the
 * fact of it.
 *
 * Why it is worth arming: §15.9.3.2 — "MCE signal is broadcast to all
 * logical processors on the system on which the UCR errors are supported,
 * except when the processor supports LMCE and LMCE is enabled by system
 * software". Unarmed, one failing DIMM line interrupts every core at once,
 * they race for one shared bank, and all but one walk away holding nothing;
 * that is the case the verdict at the bottom of mce_handle is built around.
 * Armed, the hardware may hand such an event to a single core, which then
 * has the whole record and no peers to reason about.
 *
 * Three gates, every one of them the spec's:
 *   §15.3.1.4: "The IA32_MCG_EXT_CTL MSR is present if the capability flag
 *   MCG_LMCE_P is set in the IA32_MCG_CAP MSR", and "If MCG_LMCE_P is not
 *   set in IA32_MCG_CAP, or platform software has not enabled LMCE by
 *   setting IA32_FEATURE_CONTROL.LMCE_ENABLED (bit 20), any attempt to write
 *   or read IA32_MCG_EXT_CTL will result in #GP."
 *   §15.3.1.5: "System software must ensure that both
 *   IA32_FEATURE_CONTROL.Lock (bit 0) and IA32_FEATURE_CONTROL.LMCE_ENABLED
 *   (bit 20) are set before attempting to set IA32_MCG_EXT_CTL.LMCE_EN (bit
 *   0)."
 * The order below is that sentence's order, not a taste: the #GP clause
 * names LMCE_P and LMCE_ENABLED, and it covers the READ as well as the
 * write, so both are asked before the read-modify-write can be reached. LOCK
 * is asked next because it gates only the write. A #GP here would be taken
 * during bring-up, on a kernel with no fixup for it and no console line to
 * explain it.
 *
 * Which is also why gate 1 alone is what stands in front of the rdmsr of
 * IA32_FEATURE_CONTROL (3AH), a register the SDM does not otherwise promise
 * on every part: §15.3.1.5 defines the LMCE enable protocol entirely in
 * terms of it — "Platform software can turn LMCE on by setting bit 20
 * (LMCE_ENABLED) in IA32_FEATURE_CONTROL MSR (MSR address 3AH)" — so a
 * processor that sets LMCE_P and implements no 3AH would have published a
 * feature with no way to be enabled. No vendor test is added in front of it:
 * mce_vendor_uses_intel_codes() answers a different question (how
 * MCA error codes are encoded) and would refuse LMCE on any vendor that
 * implements the architecture correctly. What keeps this line unreached on
 * AMD and Hygon today is that MCG_CAP bit 27 is reserved-zero there.
 *
 * The write is the one the initialization pseudocode (SDM Vol 3B, "Machine-
 * Check Initialization Pseudocode") writes: "IA32_MCG_EXT_CTL ←
 * IA32_MCG_EXT_CTL | 01H" — a read-modify-write, so whatever a later
 * processor generation defines beside LMCE_EN survives us.
 *
 * Then it is READ BACK, and only the read-back counts. Reporting capability
 * as if it were state is exactly the lie this function used to tell: it
 * asked IA32_FEATURE_CONTROL, never touched IA32_MCG_EXT_CTL, and answered
 * "enabled" — so mce_lmce_enabled() said "active" on a machine where every
 * single machine check was still going to every single core.
 *
 * `why` names the gate that refused, for the caller to say out loud. */
static bool mce_lmce_arm(uint64_t mcg_cap, const char **why) {
    if (!(mcg_cap & MCE_CAP_MCG_LMCE_P)) {
        *why = "IA32_MCG_CAP.LMCE_P (bit 27) is clear — this part has no "
               "IA32_MCG_EXT_CTL and no local delivery to arm";
        return false;
    }

    uint64_t feat = mce_rdmsr(MCE_MSR_IA32_FEATURE_CONTROL);

    if (!(feat & MCE_MSR_FEAT_LMCE_BIT)) {
        *why = "IA32_FEATURE_CONTROL.LMCE_ENABLED (bit 20) is clear — "
               "platform firmware did not turn LMCE on, and even READING "
               "IA32_MCG_EXT_CTL without it raises #GP";
        return false;
    }
    if (!(feat & MCE_MSR_FEAT_LOCK_BIT)) {
        *why = "IA32_FEATURE_CONTROL.LOCK (bit 0) is clear — firmware left "
               "the register open, so the LMCE_ENABLED beside it is not the "
               "platform's final word and §15.3.1.5 forbids the write";
        return false;
    }

    mce_wrmsr(MCE_MSR_MCG_EXT_CTL,
              mce_rdmsr(MCE_MSR_MCG_EXT_CTL) | MCE_MCG_EXT_CTL_LMCE_EN);

    if (!(mce_rdmsr(MCE_MSR_MCG_EXT_CTL) & MCE_MCG_EXT_CTL_LMCE_EN)) {
        *why = "IA32_MCG_EXT_CTL.LMCE_EN (bit 0) read back clear after the "
               "write — the processor kept local delivery off";
        return false;
    }

    *why = NULL;
    return true;
}

/* One logical processor could not arm LMCE: the fleet answer stops claiming
 * otherwise, and the exchange both publishes that and elects the single
 * speaker — when the answer was already false this stays quiet, because the
 * reason was said once at boot and sixteen copies of it say nothing more. */
static void mce_lmce_fleet_disarm(const char *why) {
    if (!__atomic_exchange_n(&g_mce_lmce_armed, false, __ATOMIC_ACQ_REL)) {
        return;
    }
    kprintf("[MCE] core %u: LMCE not armed: %s. The BSP armed it, so delivery "
            "is uneven from here on and every machine check is treated as "
            "broadcast again\n",
            (unsigned)amp_get_core_index(), why);
}

/* ─── Bank programming (used by BSP + per-AP) ──────────────────────── */

static void mce_program_banks(uint32_t count, uint64_t skip_mask) {
    for (uint32_t i = 0; i < count && i < MCE_MAX_BANKS; i++) {
        if (!(skip_mask & (1ULL << i))) {
            /* Enable all error sources on this bank. SDM Vol 3B §15.3.2.1:
             * IA32_MCi_CTL is bank-specific; writing 0xFFFFFFFFFFFFFFFF is
             * the canonical "enable everything" pattern. */
            mce_wrmsr(MCE_MSR_MC_CTL_BASE + i * 4, 0xFFFFFFFFFFFFFFFFULL);
        }

        /* Clear any stale status the firmware left behind — for EVERY bank,
         * including one whose CTL the alias above forbids us to write. The
         * initialization pseudocode splits exactly there: the CTL loop runs
         * "1 through MAX_BANK_NUMBER" on an aliased part, while the status
         * loop runs "FOR error-reporting banks (0 through MAX_BANK_NUMBER) DO
         * ... (OS only) IA32_MCi_STATUS ← 0" in both arms. §15.3.2.2 says
         * why: "Software is responsible for clearing IA32_MCi_STATUS MSRs by
         * explicitly writing 0s to them", and BIOS POST leaves entries there.
         * Skipping this write with the CTL write left bank 0's firmware-era
         * record standing, so the first #MC walked past it, decoded a POST
         * entry as a live one and poisoned the page its stale ADDR named. */
        mce_wrmsr(MCE_MSR_MC_STATUS_BASE + i * 4, 0ULL);
    }
}

/* ─── Public API ──────────────────────────────────────────────────── */

bool mce_is_initialized(void) { return g_mce_initialized; }
uint32_t mce_bank_count(void) { return g_mce_bank_count; }
bool mce_lmce_enabled(void)   {
    return __atomic_load_n(&g_mce_lmce_armed, __ATOMIC_ACQUIRE);
}

void mce_init(void) {
    if (g_mce_initialized) return;

    if (!mce_supported_local()) {
        debug_printf("[MCE] CPUID lacks MCE/MCA — handler disabled\n");
        return;
    }

    mce_cr4_enable();

    uint64_t cap = mce_rdmsr(MCE_MSR_MCG_CAP);
    uint32_t bank_count = (uint32_t)(cap & MCE_CAP_COUNT_MASK);
    if (bank_count > MCE_MAX_BANKS) bank_count = MCE_MAX_BANKS;

    uint64_t skip = 0;
    if (mce_skip_bank_zero()) skip |= 1ULL;

    mce_program_banks(bank_count, skip);

    /* MCG_CTL_P: write 0xFF...FF to enable global reporting on systems
     * with the global control MSR. Skipped when absent (writing would
     * #GP). */
    if (cap & MCE_CAP_MCG_CTL_P) {
        mce_wrmsr(MCE_MSR_MCG_CTL, 0xFFFFFFFFFFFFFFFFULL);
    }

    /* Make sure the in-progress flag is clear at boot. Some firmware
     * leaves MCIP=1 after a warm reboot following a fatal MCE. */
    mce_wrmsr(MCE_MSR_MCG_STATUS, 0ULL);

    g_mce_mcg_cap    = cap;
    g_mce_ban_mask   = skip;
    g_mce_bank_count = bank_count;
    g_mce_cod_intel  = mce_vendor_uses_intel_codes();

    const char *lmce_why = NULL;
    bool lmce_armed = mce_lmce_arm(cap, &lmce_why);
    __atomic_store_n(&g_mce_lmce_armed, lmce_armed, __ATOMIC_RELEASE);

    /* SER_P is said out loud, not merely traced: it is the one bit that
     * decides whether ANY uncorrected error on this machine can be survived.
     * With it clear, §15.6.1 leaves the handler one duty — log the error and
     * shut the system down — and the operator should know that before the
     * first #MC, not from the panic that follows it. */
    kprintf("[MCE] banks=%u SER_P=%d TES_P=%d CMCI_P=%d LMCE_P=%d "
            "LMCE_armed=%d cap=0x%016lx%s\n",
            (unsigned)bank_count,
            (int)((cap >> 24) & 1u),
            (int)((cap >> 11) & 1u),
            (int)((cap >> 10) & 1u),
            (int)((cap >> 27) & 1u),
            (int)lmce_armed,
            (unsigned long)cap,
            (cap & MCE_CAP_MCG_SER_P)
                ? ""
                : " — no software error recovery on this part: every "
                  "uncorrected error is fatal");

    /* Not armed is not a detail to trace away: it decides whether the next
     * uncorrected error interrupts one core or all of them, and it is the
     * premise the handler's verdict rests on. Said with the gate that
     * refused, because "LMCE off" on its own is a line nobody can act on —
     * a clear LOCK or LMCE_ENABLED is firmware the operator can change, a
     * clear LMCE_P is silicon they cannot. */
    if (!lmce_armed) {
        kprintf("[MCE] LMCE not armed: %s. Every machine check on this "
                "machine is broadcast to every logical processor\n",
                lmce_why);
    }

    debug_printf("[MCE] BSP init: MCG_CTL_P=%d skip_mask=0x%lx "
                 "intel_error_codes=%d\n",
                 (int)((cap >> 8) & 1u),
                 (unsigned long)skip,
                 (int)g_mce_cod_intel);

    /* Pre-resolve Touch tag handles for the IST-safe publish path. Done
     * here (outside IRQ context) because TouchLogbookIntern takes
     * locks. The handles are stored as globals for mce_handle to use
     * via TouchPublishIrqPair (which is the only Touch publisher safe
     * to call from IST). Tags are MemTag-reserved in SeedReservedTags. */
    g_mce_tag_detected  = TouchLogbookIntern("mce:fault:detected");
    g_mce_tag_recovered = TouchLogbookIntern("mce:fault:recovered");
    g_mce_tag_fatal     = TouchLogbookIntern("mce:fault:fatal");
    debug_printf("[MCE] Touch handles cached: detected=0x%x recovered=0x%x "
                 "fatal=0x%x\n",
                 (unsigned)g_mce_tag_detected,
                 (unsigned)g_mce_tag_recovered,
                 (unsigned)g_mce_tag_fatal);

    /* Arm the record pool LAST but before the handler is allowed to use it:
     * every slot's baton must carry its continuation before a #MC can pass
     * it. g_mce_initialized, published just below, is half of the gate the
     * handler asks (mce_deferral_open); the other half is g_baton_ready,
     * because the baton queues are not built until much later in main.c and
     * a pass before they exist is refused — out loud, from abort context. */
    mce_note_pool_arm();

    g_mce_initialized = true;
}

void mce_ap_init(void) {
    /* APs program their own CR4.MCE + per-bank state. The BSP detected
     * bank count + capability flags via the shared g_mce_mcg_cap.
     * On a hybrid CPU where an AP reports different MCG_CAP, we trust
     * the AP's CPUID gate to keep us from #GPing on bank registers it
     * doesn't have. */

    /* The BSP refused, so nothing was ever armed and the fleet answer is
     * already false — there is nothing here to take back. */
    if (!g_mce_initialized) return;

    /* This logical processor has no MCA at all, so it has no
     * IA32_MCG_EXT_CTL to arm. It must still take the fleet answer back:
     * mce_lmce_enabled() promises the fact of the write on every core that
     * has run MCA init, and a core that ran this function and armed nothing
     * would otherwise leave that promise standing on the BSP's write alone. */
    if (!mce_supported_local()) {
        mce_lmce_fleet_disarm("CPUID on this logical processor reports no "
                              "MCE/MCA, so there is no IA32_MCG_EXT_CTL here "
                              "to arm");
        return;
    }

    mce_cr4_enable();

    /* Re-read MCG_CAP — possible per-thread divergence on hybrid CPUs.
     * Cap by the BSP-observed count to keep telemetry coherent. */
    uint64_t cap = mce_rdmsr(MCE_MSR_MCG_CAP);
    uint32_t bank_count = (uint32_t)(cap & MCE_CAP_COUNT_MASK);
    if (bank_count > g_mce_bank_count) bank_count = g_mce_bank_count;

    mce_program_banks(bank_count, g_mce_ban_mask);

    if (cap & MCE_CAP_MCG_CTL_P) {
        mce_wrmsr(MCE_MSR_MCG_CTL, 0xFFFFFFFFFFFFFFFFULL);
    }

    mce_wrmsr(MCE_MSR_MCG_STATUS, 0ULL);

    /* LMCE_EN lives in IA32_MCG_EXT_CTL, which is per logical processor: the
     * BSP's write armed the BSP and nothing else, so every AP must ask the
     * three gates for itself against its OWN MCG_CAP — on a hybrid part an
     * E-core need not carry LMCE_P at all.
     *
     * An AP that cannot arm it while the fleet was believed armed is the one
     * asymmetry worth a line: from that moment a broadcast is possible
     * again, and mce_lmce_enabled() must stop claiming otherwise — that is
     * mce_lmce_fleet_disarm's whole job. An AP that DID arm while the BSP
     * could not keeps its own LMCE_EN: local delivery on one core is never
     * worse than broadcast on all, and the fleet-wide answer stays honest at
     * false. */
    const char *why = NULL;
    if (!mce_lmce_arm(cap, &why)) mce_lmce_fleet_disarm(why);
}

/* ─── #MC handler ─────────────────────────────────────────────────── */

/* Touch payload — fits 64 B Pocket envelope. Userspace subscribers
 * to `mce:fault:detected` learn precise bank + status without RDMSR. */
typedef struct {
    uint64_t  status;
    uint64_t  addr;
    uint64_t  misc;
    uint64_t  mcg_status;
    uint64_t  rip;
    uint32_t  bank;
    uint8_t   severity;     /* mce_severity_t */
    uint8_t   recovered;    /* 1 = handled, 0 = fatal */
    uint16_t  pad;
} mce_event_payload_t;

/* What one bank record is, judged against what THIS processor implements.
 *
 * Called only from the #MC handler, i.e. always in exception context — the
 * EN rule below is an exception-context rule and would be wrong for a poll.
 *
 * The gate that used to be missing is MCG_CAP.MCG_SER_P (bit 24). SDM
 * §15.6.1: software must use it to detect the presence of software error
 * recovery support, and when it is clear there is no recovery on this
 * processor — the handler's whole duty is to log the error and shut the
 * system down. §15.6: the first part to set it is DisplayFamily_DisplayModel
 * 06_2EH, so every older Intel, and any part that leaves it clear, is such a
 * processor. Table 15-6 says the same from the other side: its UCNA, SRAO
 * and SRAR rows exist "only when IA32_MCG_CAP[24] (MCG_SER_P) is set".
 * Reading S and AR without that bit reads bits that hold nothing —
 * §15.3.1.1 makes 56:55 reserved when TES_P is set without SER_P, and 56:53
 * model-specific when TES_P is clear.
 *
 * With SER_P set the record is read the way Table 15-6 writes it:
 *   EN=0 → every UCR row requires EN=1, so this entry did not signal this
 *          #MC. Log it, clear it, act on nothing. Only legal to skip
 *          BECAUSE SER_P is set: §15.10.4.1 says that without it even a
 *          UC=1/EN=0 entry is fatal.
 *   S=0  → the hardware logged this entry and never signalled it, so AR
 *          decides what it is. AR=0 is UCNA: "System software is not
 *          required to take any recovery action" (§15.6.2), and the handler
 *          pseudocode sends it straight to CONTINUE. AR=1 is a combination
 *          the architecture does not allow to exist — an action is REQUIRED
 *          for an error the processor says it never signalled — and the
 *          pseudocode answers it in one line: "ELSE RESTARTABILITY = FALSE;
 *          (* S=0, AR=1 is illegal *)", followed by "IF RESTARTABILITY =
 *          FALSE THEN ... NOERROR = FALSE". Fatal, therefore, and for the
 *          reason the pair itself gives: AR=1 demands an action for an error
 *          S=0 says was never signalled to anyone, so no row of Table 15-6
 *          names what that action would be — and §15.6.2 is explicit that
 *          when the required action cannot be completed, the system must be
 *          shut down. The page such a record names is still taken out of
 *          service when it names one readably, exactly as for every other
 *          fatal record: the verdict is about this thread, not about the
 *          frame. Linux spells the same rule PANIC "Illegal combination
 *          (UCNA with AR=1)".
 *   S=1  → SRAO (AR=0) or SRAR (AR=1). What may be done about it follows
 *          from AR, from OVER and from the MCA error code — all three are
 *          read together in mce_ucr_action.
 *
 * OVER is deliberately NOT read here, and its place in the order is the
 * whole point. Table 15-6 attaches "If OVER=1, reset system" to the SRAR
 * row — to a record that is enabled, signalled and action-required — not
 * to every uncorrected record carrying the bit. A not-enabled or UCNA entry
 * sits in its bank until somebody polls it, and this kernel has no CMCI
 * handler yet, so that entry is precisely the one most likely to have
 * overflowed by the time an unrelated #MC walks past it: grading it fatal
 * here would halt a healthy machine over an error that never signalled
 * anything. Linux reads the same bits in the same order — "Not enabled" is
 * the second rule of its severity table, its UCNA rule carries the comment
 * "ignore OVER for UCNA", and its generic "Overflowed uncorrected" PANIC
 * rule is the last rule of all. Here that last rule lives in the SER_P=0
 * arm below, where every uncorrected record is fatal already. */
static mce_severity_t mce_classify(uint64_t status) {
    if (!(status & MCE_MC_STATUS_VAL)) return MCE_SEV_NONE;
    if (!(status & MCE_MC_STATUS_UC))  return MCE_SEV_CORRECTED;
    if (status & MCE_MC_STATUS_PCC)    return MCE_SEV_UC;

    if (!(g_mce_mcg_cap & MCE_CAP_MCG_SER_P)) return MCE_SEV_UC;
    if (!(status & MCE_MC_STATUS_EN))         return MCE_SEV_UCNA;
    if (!(status & MCE_MC_STATUS_S)) {
        return (status & MCE_MC_STATUS_AR) ? MCE_SEV_UC : MCE_SEV_UCNA;
    }
    return MCE_SEV_UCR;
}

static const char *mce_sev_str(mce_severity_t s) {
    switch (s) {
        case MCE_SEV_NONE:      return "NONE";
        case MCE_SEV_CORRECTED: return "CORRECTED";
        case MCE_SEV_UCR:       return "UCR";
        case MCE_SEV_UC:        return "UC";
        case MCE_SEV_UCNA:      return "UCNA";
        default:                return "?";
    }
}

/* The physical page this bank record names — or 0, meaning it names none
 * that may be acted on.
 *
 * SDM §15.3.2.3 about IA32_MC<i>_ADDR: "The address returned is an offset
 * into a segment, linear address, or physical address. This depends on the
 * error encountered." So the register alone never says which address space
 * it is in. §15.3.2.4 is where that is said — MISC[8:6] Address Mode and
 * MISC[5:0] Recoverable Address LSB — and it defines that layout only when
 * MISCV and IA32_MCG_CAP[24] are BOTH set. Without SER_P the register is
 * model-specific end to end.
 *
 * Hence all five conditions, which are exactly Linux's
 * intel_mce_usable_address plus the SER gate its mce_read_aux masking
 * carries: ADDRV, SER_P, MISCV, mode == 010b (physical), and an LSB no
 * coarser than a 4 KiB page. A memory-controller bank that is only valid to
 * rank or row granularity reports LSB > 12; its low bits are noise, and
 * taking them as a page number offlines and MIGRATES a healthy page that
 * belongs to a running cabin — losing every write made to it inside the
 * shootdown window — while the page that actually failed stays in service.
 * A record we cannot read is logged and left alone. */
static uintptr_t mce_extract_phys_page(uint64_t status, uint64_t addr,
                                       uint64_t misc) {
    if (!(status & MCE_MC_STATUS_ADDRV))       return 0;
    if (!(g_mce_mcg_cap & MCE_CAP_MCG_SER_P))  return 0;
    if (!(status & MCE_MC_STATUS_MISCV))       return 0;

    uint32_t mode = (uint32_t)((misc >> MCE_MC_MISC_ADDR_MODE_SHIFT) &
                               MCE_MC_MISC_ADDR_MODE_MASK);
    if (mode != MCE_MC_MISC_ADDR_MODE_PHYS) return 0;

    uint32_t lsb = (uint32_t)(misc & MCE_MC_MISC_ADDR_LSB_MASK);
    if (lsb > (uint32_t)__builtin_ctz(PMM_PAGE_SIZE)) return 0;

    /* Drop the bits the bank itself declares invalid, then the rest of the
     * page offset. With LSB no greater than the 4 KiB page shift the second
     * mask subsumes the first; both are written because both are what the
     * spec asks for, in the order it asks for them. */
    uintptr_t phys = (uintptr_t)((addr >> lsb) << lsb);
    return phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1);
}

/* Does this record name a piece of MEMORY? SDM §15.9.2 (Compound Error
 * Codes) puts a memory-controller error at bit 7 of the MCA error code, a
 * cache-hierarchy error at bit 8, and the generic memory-hierarchy error at
 * 0b11LL. A TLB error, a bus/interconnect error or an internal-timer error
 * names no page; offlining the address such a bank happens to carry would
 * take a page out of service for a fault that does not live in it.
 *
 * Only asked where Intel's tables apply. On AMD/Hygon the answer would have
 * to come from the SMCA bank type in MCA_IPID, which this kernel does not
 * read — so there the question is not asked and the record is judged by its
 * address alone, as it was before. A guessed decode would be worse than an
 * absent one. */
static bool mce_record_names_memory(uint64_t status) {
    if (!g_mce_cod_intel) return true;
    return (status & MCE_MCACOD_MEMCTL_MASK) == MCE_MCACOD_MEMCTL ||
           (status & MCE_MCACOD_CACHE_MASK)  == MCE_MCACOD_CACHE  ||
           (status & MCE_MCACOD_MEMGEN_MASK) == MCE_MCACOD_MEMGEN;
}

/* What Table 15-6 lets this handler DO with a UCR record. */
typedef enum {
    MCE_UCR_LOG_ONLY = 0,  /* say it; there is nothing here to act on */
    MCE_UCR_OFFLINE  = 1,  /* take the page out of service and migrate it */
    MCE_UCR_BUGCHECK = 2,  /* the row leaves no recovery — fatal */
} MceUcrAction;

/* `phys` is what mce_extract_phys_page could make of the bank's address —
 * 0 when the bank named no page this handler may trust.
 *
 * SRAR (AR=1) is the error raised at the point of consumption. Table 15-6:
 * "For known MCACOD, take specific recovery action; For unknown MCACOD, must
 * bugcheck", and §15.6.2 adds that when the required action "cannot be
 * successfully completed, system software must shut down the system". The
 * one action this kernel has is offlining the page, so a SRAR whose page
 * cannot be named is a SRAR whose recovery cannot be completed.
 *
 * OVER is read here and nowhere else, because Table 15-6 scopes it to this
 * row: "If OVER=1, reset system, else take specific recovery action." A
 * second uncorrected error reached the bank before the handler read it, so
 * the address still in ADDR names one of the two and nothing says which —
 * the one action this kernel has cannot cover an event whose location the
 * processor already threw away.
 *
 * SRAO (AR=0) is action OPTIONAL: the error was not consumed, so an
 * unreadable address or an unknown code costs nothing but the page. Log it
 * and keep running — turning it fatal would take the machine down for an
 * error the processor itself said could be deferred. OVER does not change
 * that row either: the record's own address is still the address the
 * processor logged, and retiring that one frame is the whole of the
 * optional action. (Linux grades this combination "Action optional with
 * lost events" and takes no action at all. The divergence is deliberate:
 * its action is unmapping and signalling a process, which cannot be undone,
 * while ours is moving a page to a fresh frame, which costs the page and
 * nothing else.) */
static MceUcrAction mce_ucr_action(uint64_t status, uintptr_t phys) {
    bool ar = (status & MCE_MC_STATUS_AR) != 0;

    if (ar) {
        if (status & MCE_MC_STATUS_OVER) return MCE_UCR_BUGCHECK;

        bool known = !g_mce_cod_intel ||
                     (status & MCE_MCACOD_MASK) == MCE_MCACOD_DATA ||
                     (status & MCE_MCACOD_MASK) == MCE_MCACOD_INSTR;
        if (!known || phys == 0) return MCE_UCR_BUGCHECK;
        return MCE_UCR_OFFLINE;
    }

    bool known = !g_mce_cod_intel ||
                 (status & MCE_MCACOD_SCRUB_MASK) == MCE_MCACOD_SCRUB ||
                 (status & MCE_MCACOD_MASK)       == MCE_MCACOD_L3WB;
    if (!known || phys == 0) return MCE_UCR_LOG_ONLY;
    return MCE_UCR_OFFLINE;
}

/* ─── What the #MC saw: the record, and who is left to say it ───────── */
/*
 * The handler may take no lock while MCIP is set (see the file header), and
 * the two things a machine check must still produce — the line on the
 * console and the `mce:poisoned` MemTag on the page — are both lock-takers.
 * So the bank walk copies what it saw into a slot and hands THAT SLOT'S OWN
 * baton to a K-Core, exactly as mce_migrate does with its migration
 * requests: the node is embedded in the slot, so the pass needs no
 * allocation and cannot be dropped for want of a queue entry.
 *
 * The pass happens once the verdict is known, and not one line earlier,
 * because a baton is only worth handing to someone who will still be there
 * to run it. A kernel-mode FATAL verdict sends idt.c down
 * panic_claim_or_halt → the dump → IPI_PANIC → `cli; hlt` on every core, and
 * nothing on that road pumps a baton: the whole MCA record — bank, status,
 * addr, misc, MCG_STATUS, RIP — would be queued to a core that halts before
 * it drains, and the operator would read "KERNEL PANIC: #MC (vector 18)"
 * with not one number saying which piece of hardware failed. That is the one
 * event this subsystem exists for, so on that road the handler says the
 * record ITSELF.
 *
 * Saying it there is legal for a reason, not by exception:
 *   - MCG_STATUS has already been cleared by then, so a core that wedges on
 *     the console lock is no longer a core with MCIP set — the "second
 *     machine-check event ⇒ architectural shutdown" hazard is over;
 *   - the caller is about to take that very lock for its panic dump from
 *     this very frame. If the interrupted code held it, the dump was going
 *     to wedge on it anyway; speaking here moves nothing into the hazard and
 *     buys the MCA protocol ahead of it.
 * On every other road — a recoverable verdict, or a fatal one from ring 3
 * where idt.c kills the process and the machine lives — the handler stays
 * silent and lets the K-Core speak, because there the wedge would be a fresh
 * one that nobody else was going to risk.
 *
 * The same applies before BatonInit. main.c arms this pool inside mce_init
 * and builds the baton queues much later, so there is a real window with a
 * pool and no queue; a pass inside it is refused by BatonPass, which says so
 * with kprintf from abort context — the exact deadlock this file is built to
 * avoid. So the handler asks first (mce_deferral_open) and speaks for itself
 * instead of letting the refusal speak.
 */

#define MCE_NOTE_POOL_SIZE   32u
#define MCE_NOTE_POOL_MASK   (MCE_NOTE_POOL_SIZE - 1u)

/* Bank index of a record that stands for the whole event instead of for one
 * bank: the walk found no valid bank at all. */
#define MCE_NOTE_NO_BANK     0xFFFFFFFFu

typedef struct {
    uint64_t  status;
    uint64_t  addr;
    uint64_t  misc;
    uint64_t  mcg_status;
    uint64_t  rip;
    uintptr_t phys;          /* page to tag, 0 when none */
    uint32_t  bank;          /* MCE_NOTE_NO_BANK = the empty walk */
    uint16_t  severity;      /* mce_severity_t */
    uint8_t   nested;        /* 1 = consumed by an in-flight migration */
    uint8_t   rsv0;
} MceRecord;

typedef struct {
    MceRecord         rec;
    volatile uint32_t in_use;   /* 0 = free, 1 = claimed */
    Baton             baton;    /* this slot's own pass to the K-Core */
} MceNoteSlot;

static MceNoteSlot       g_mce_notes[MCE_NOTE_POOL_SIZE];
static volatile uint32_t g_mce_note_cursor = 0;
static volatile uint64_t g_mce_notes_lost  = 0;

/* The losses since anyone last said them, claimed with an exchange so the
 * count reads the same whether a K-Core or a dying core says it. */
static void mce_notes_lost_say(void) {
    uint64_t lost = __atomic_exchange_n(&g_mce_notes_lost, 0, __ATOMIC_RELAXED);
    if (lost == 0) return;
    kprintf("[MCE] ERROR: %lu machine-check record(s) never reached a K-Core "
            "— the bank walk outran the pool of %u. Those pages stay poisoned "
            "in the PMM bitmap, so no future allocation takes them, but they "
            "carry no `mce:poisoned` tag and their banks were never said\n",
            (unsigned long)lost, (unsigned)MCE_NOTE_POOL_SIZE);
}

/* The line itself. Runs on a K-Core off the baton, or on the core that is
 * about to hand idt.c a fatal verdict — see the block comment above for why
 * the console lock is legal in the second case. */
static void mce_record_say(const MceRecord *r) {
    int ripv = (r->mcg_status & MCE_MCG_STATUS_RIPV) ? 1 : 0;
    int eipv = (r->mcg_status & MCE_MCG_STATUS_EIPV) ? 1 : 0;
    int mcip = (r->mcg_status & MCE_MCG_STATUS_MCIP) ? 1 : 0;
    /* LMCE_S is printed on every record because it is the one bit that says
     * whether this event had peers at all, and the verdict's whole argument
     * for having no rendezvous turns on that. */
    int lmce = (r->mcg_status & MCE_MCG_STATUS_LMCE_S) ? 1 : 0;

    if (r->bank == MCE_NOTE_NO_BANK) {
        kprintf("[MCE] #MC with no valid bank on this logical processor. "
                "MCG_STATUS=0x%016lx (RIPV=%d EIPV=%d MCIP=%d LMCE_S=%d) "
                "RIP=0x%016lx. %s RIPV=1 together with EIPV=0 decides what "
                "happens to this thread; the record itself is somebody "
                "else's to say\n",
                (unsigned long)r->mcg_status, ripv, eipv, mcip, lmce,
                (unsigned long)r->rip,
                lmce ? "LMCE_S is set, so this event was delivered to this "
                       "processor ALONE — no peer can be holding the record, "
                       "and it is either in a bank this CPU does not walk or "
                       "was taken by firmware-first reporting."
                     : "Either the record sits in a bank this CPU does not "
                       "walk, or another logical processor took the same "
                       "broadcast signal and cleared the shared bank first.");
        return;
    }

    kprintf("[MCE] bank[%u] %s status=0x%016lx addr=0x%016lx misc=0x%016lx "
            "page=0x%lx MCG_STATUS=0x%016lx (RIPV=%d EIPV=%d MCIP=%d "
            "LMCE_S=%d) RIP=0x%016lx%s\n",
            (unsigned)r->bank, mce_sev_str((mce_severity_t)r->severity),
            (unsigned long)r->status, (unsigned long)r->addr,
            (unsigned long)r->misc, (unsigned long)r->phys,
            (unsigned long)r->mcg_status, ripv, eipv, mcip, lmce,
            (unsigned long)r->rip,
            r->nested ? " [nested #MC consumed by an in-flight migration: "
                        "that copy zero-fills its tail]" : "");
}

/* The visit: normal K-Core context, where the console lock and the MemTag
 * registry are legal. */
static void mce_note_worker(void *slot_ptr) {
    MceNoteSlot *slot = (MceNoteSlot *)slot_ptr;

    /* Copy out, then hand the slot back before anything slow: a #MC landing
     * while this record is being said must still find room. */
    MceRecord r = slot->rec;
    __atomic_store_n(&slot->in_use, 0, __ATOMIC_RELEASE);

    mce_notes_lost_say();
    mce_record_say(&r);

    if (r.phys != 0 &&
        (r.severity == MCE_SEV_UCR || r.severity == MCE_SEV_UC)) {
        error_t e = MemTagApplyByPhys(r.phys, 1, "mce:poisoned");
        if (e != OK) {
            kprintf("[MCE] ERROR: page 0x%lx could not be tagged "
                    "`mce:poisoned` (%s) — the PMM bitmap still keeps it out "
                    "of every future allocation, but nothing that reads tags "
                    "can see why\n",
                    (unsigned long)r.phys, ErrorString(e));
        }
    }
}

static void mce_note_pool_arm(void) {
    for (uint32_t i = 0; i < MCE_NOTE_POOL_SIZE; i++) {
        __atomic_store_n(&g_mce_notes[i].in_use, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_mce_notes[i].baton.next, NULL, __ATOMIC_RELAXED);
        g_mce_notes[i].baton.run = mce_note_worker;
        g_mce_notes[i].baton.ctx = &g_mce_notes[i];
    }
}

/* Is there anywhere to defer TO? The pool must be armed (mce_init) and the
 * baton queues must exist (BatonInit, much later in main.c). Between the two
 * a pass is refused and the refusal speaks from abort context, so the
 * handler does not pass at all in that window — it says what it saw where it
 * stands, which is the same lock with the refusal's noise removed. */
static inline bool mce_deferral_open(void) {
    return g_mce_initialized &&
           __atomic_load_n(&g_baton_ready, __ATOMIC_ACQUIRE) != 0;
}

/* Abort context: claim a slot and fill it. No lock, no allocation. NULL when
 * the pool is saturated — counted, and said by whoever speaks next, because
 * nothing can be printed from where the loss happens. */
static MceNoteSlot *mce_note_claim(const MceRecord *r) {
    uint32_t start = __atomic_fetch_add(&g_mce_note_cursor, 1, __ATOMIC_RELAXED);
    for (uint32_t k = 0; k < MCE_NOTE_POOL_SIZE; k++) {
        MceNoteSlot *slot = &g_mce_notes[(start + k) & MCE_NOTE_POOL_MASK];
        uint32_t expected = 0;
        if (!__atomic_compare_exchange_n(&slot->in_use, &expected, 1, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            continue;
        }
        slot->rec = *r;
        return slot;
    }
    __atomic_fetch_add(&g_mce_notes_lost, 1, __ATOMIC_RELAXED);
    return NULL;
}

/* The verdict is in: hand this record to a K-Core if one will still be
 * running, say it here if none will. Either way the slot comes back — a
 * refused pass that kept its slot would bleed the pool one entry per
 * machine check. */
static void mce_note_deliver(MceNoteSlot *slot, bool say_here) {
    if (!say_here && BatonPass(&slot->baton)) return;

    MceRecord r = slot->rec;
    __atomic_store_n(&slot->in_use, 0, __ATOMIC_RELEASE);

    mce_notes_lost_say();
    mce_record_say(&r);
}

bool mce_handle(interrupt_frame_t *frame) {
    bool     any_fatal = false;
    uint32_t decoded   = 0;          /* banks that held a valid record */

    /* SDM's NOERROR, kept from the other side — the verdict at the bottom
     * needs it. In the handler pseudocode ("Machine-Check Error Handler
     * Pseudocode Supporting UCR") every record inside `IF UC Flag ... = 1`
     * ends at NOERROR = FALSE except three that leave before reaching it:
     * "IF EN Flag ... = 0 AND OVER Flag ... = 0 THEN (* It is a spurious MCA
     * Log ... *) CLEAR_MC_BANK = TRUE; GOTO LOG MCA REGISTER", and "IF AR
     * Flag ... = 0 THEN (* It is a uncorrected no action required (UCNA)
     * error *) GOTO CONTINUE", plus the corrected arm's own CONTINUE. The
     * walk grades those CORRECTED and UCNA, and grades everything else UCR
     * or UC — including the S=0/AR=1 record the pseudocode calls illegal,
     * which reaches NOERROR = FALSE through "IF RESTARTABILITY = FALSE THEN
     * ... NOERROR = FALSE". So `uncorrected_seen` false is NOERROR true.
     *
     * With one divergence, stated rather than glossed: mce_classify grades
     * EN=0 UCNA whatever OVER says, where the pseudocode's spurious exit
     * wants OVER=0 too and carries an EN=0/OVER=1 record onward to NOERROR =
     * FALSE. A record whose reporting was never enabled did not raise this
     * #MC, whatever later overflowed on top of it — that is Linux's "Not
     * enabled" rule and mce_classify argues it there. What that rule costs
     * at the RECORD level is argued at mce_classify; what it costs HERE is
     * nothing, in the safe direction: keeping NOERROR true leaves the EIPV
     * term below LIVE, so on this half the divergence can only make the
     * walk more fatal, never less. */
    bool     uncorrected_seen = false;

    /* Records claimed during the walk. They are held, not passed, until the
     * verdict below knows whether a K-Core will still be there to run them.
     * MCE_MAX_BANKS pointers is 512 bytes of the 16 KiB IST stack, and the
     * walk below cannot claim more than one per bank it visits. */
    MceNoteSlot *claimed[MCE_MAX_BANKS];
    uint32_t     claimed_n = 0;

    /* Snapshot MCG_STATUS once, at entry: the verdict at the bottom is taken
     * from THIS value, because the register itself is cleared before we get
     * there. */
    uint64_t mcg_status = mce_rdmsr(MCE_MSR_MCG_STATUS);
    bool     ripv       = (mcg_status & MCE_MCG_STATUS_RIPV) != 0;
    bool     eipv       = (mcg_status & MCE_MCG_STATUS_EIPV) != 0;
    bool     mcip       = (mcg_status & MCE_MCG_STATUS_MCIP) != 0;

    /* Asked once, for every deferral below — the Touch publish, the
     * migration request and the record itself all ride the same queues. */
    bool deferral_open = mce_deferral_open();

    for (uint32_t i = 0; i < g_mce_bank_count && i < MCE_MAX_BANKS; i++) {
        uint64_t status = mce_rdmsr(MCE_MSR_MC_STATUS_BASE + i * 4);
        if (!(status & MCE_MC_STATUS_VAL)) continue;

        uint64_t addr = (status & MCE_MC_STATUS_ADDRV)
                        ? mce_rdmsr(MCE_MSR_MC_ADDR_BASE + i * 4) : 0;
        uint64_t misc = (status & MCE_MC_STATUS_MISCV)
                        ? mce_rdmsr(MCE_MSR_MC_MISC_BASE + i * 4) : 0;

        mce_severity_t sev = mce_classify(status);

        /* The page this record names, and whether it may be acted on. Both
         * questions, not one: a record may name a page it has no licence to
         * offline (an unknown MCACOD, an error that is not a memory error at
         * all), and a record may have a licence and no readable address —
         * which for an action-REQUIRED error is itself fatal. */
        uintptr_t poisoned_phys = mce_extract_phys_page(status, addr, misc);
        bool      offline       = false;

        if (sev == MCE_SEV_UCR) {
            switch (mce_ucr_action(status, poisoned_phys)) {
                case MCE_UCR_OFFLINE:
                    offline = true;
                    /* SRAR (AR=1) is the error raised where the poison was
                     * CONSUMED. §15.6.2 requires its recovery action to be
                     * "completed successfully before any additional work is
                     * scheduled for this processor", and this handler
                     * cannot complete it here: the replacement page, the
                     * copy, the PTE swap and the shootdown all take locks
                     * the interrupted frame on this very core may already
                     * hold, so they ride a baton to a K-Core and finish
                     * well after IRET. The one way to honour the ordering
                     * is therefore not to schedule that context again at
                     * all — the record is fatal for the consumer, and idt.c
                     * decides what that costs: the process is killed when
                     * the #MC came from ring 3, the machine halts when it
                     * came from ring 0 (no recoverable-copy region exists
                     * to resume a poisoned kernel load into, and Linux
                     * grades the same combination PANIC "Uncorrected in
                     * kernel"). The migration still runs, for every OTHER
                     * cabin that maps the frame.
                     *
                     * WHICH processor is the consumer is MCG_STATUS's to
                     * say, not cs's. Without LMCE armed every #MC is
                     * broadcast, so N-1 logical processors walk this same
                     * shared bank having consumed nothing at all, and SDM
                     * Table 15-21 says how they read: an SRAR's non-
                     * affected processors "should find that the RIPV flag
                     * ... is set and the EIPV flag ... is cleared". That
                     * pair, and only that pair, is allowed to keep running
                     * — Linux's rule "Action required but unaffected thread
                     * is continuable" is the same test. Judging by cs
                     * instead would halt the whole machine every time a
                     * broadcast SRAR happened to interrupt one innocent
                     * core in ring 0. */
                    if ((status & MCE_MC_STATUS_AR) && !(ripv && !eipv)) {
                        sev = MCE_SEV_UC;
                    }
                    break;
                case MCE_UCR_BUGCHECK:
                    sev     = MCE_SEV_UC;
                    offline = (poisoned_phys != 0) &&
                              mce_record_names_memory(status);
                    break;
                case MCE_UCR_LOG_ONLY:
                default:
                    break;
            }
        } else if (sev == MCE_SEV_UC) {
            /* PCC=1, or an uncorrected error on a part with no recovery at
             * all. The machine may still live — idt.c kills the process when
             * the #MC came from ring 3 — so the page is only taken out of
             * service when the record actually names memory. */
            offline = (poisoned_phys != 0) && mce_record_names_memory(status);
        }

        if (!offline) poisoned_phys = 0;

        decoded++;

        /* Nested-#MC notifier: if this CPU is in the middle of a
         * mce_safe_page_copy and the bad phys matches the in-flight
         * migration's source, signal the copy loop to abort + zero-fill.
         * This must happen BEFORE we poison the page below so the abort
         * flag is set while the copy is still running. The check is
         * O(1) per-CPU. */
        bool nested = (poisoned_phys != 0 &&
                       mce_migrate_note_nested(poisoned_phys));

        /* Mark the affected page poisoned for future allocations. The PMM
         * bitmap is the only poisoning legal here: pmm_set_poisoned is a
         * lock-free __atomic_fetch_or. The `mce:poisoned` MemTag that used
         * to be applied on this line is applied by the record's K-Core
         * visit instead — MemTagApplyByPhys interns the tag string
         * (kmalloc), takes the tag registry's global lock, the region
         * registry lock and a bucket lock, every one of which the
         * interrupted frame on this very core may already hold. Active
         * mappings keep working until the owning process exits OR until the
         * deferred migration worker (queued below) swaps the PTE to a fresh
         * phys page. */
        if (poisoned_phys != 0) {
            pmm_set_poisoned(poisoned_phys);

            /* Queue a deferred migration. The actual copy + PTE swap +
             * TLB shootdown runs in K-Core context off the slot's baton (the
             * #MC IST stack can't safely take pmm_alloc / region-bucket
             * locks).
             *
             * Whether a failure to queue is survivable is the bank's to say,
             * not ours. For an action-OPTIONAL error it is: the page stays
             * poisoned in the PMM bitmap, so no future allocation takes it,
             * and the cabin that still maps it will #PF and die cleanly. An
             * action-REQUIRED error reaches this line only on a processor
             * that did not consume it — the consumer's record was graded
             * fatal above — and even there §15.6.2 is explicit: when the
             * recovery action cannot be successfully completed, the system
             * must be shut down. Queuing it IS the action; a saturated ring,
             * or a #MC before the baton queues exist, means it never
             * started, and the frame stays live under every cabin that maps
             * it. */
            bool queued = deferral_open &&
                          mce_migrate_request(poisoned_phys, sev, status);
            if (!queued && (status & MCE_MC_STATUS_AR) &&
                sev == MCE_SEV_UCR) {
                sev = MCE_SEV_UC;
            }
        }

        if (sev == MCE_SEV_UCR || sev == MCE_SEV_UC) uncorrected_seen = true;

        /* Touch payload — copied onto IRQ pocket inline. */
        mce_event_payload_t ev = {
            .status     = status,
            .addr       = addr,
            .misc       = misc,
            .mcg_status = mcg_status,
            .rip        = frame->rip,
            .bank       = i,
            .severity   = (uint8_t)sev,
            .recovered  = 0,
            .pad        = 0,
        };

        /* IRQ-safe Touch publish — see touch.h §IRQ-Publishers. The
         * #MC handler runs on the IST_MACHINE_CHECK stack; calling
         * TouchPublish here would deadlock against any caller holding
         * a Touch lock at the moment #MC fired. TouchPublishIrqPair
         * copies the payload into a preallocated static slot and
         * defers actual publish to a K-Core worker. */
        TouchTag pub_tag = TOUCH_TAG_INVALID;
        switch (sev) {
            case MCE_SEV_UC:
                ev.recovered = 0;
                any_fatal = true;
                pub_tag = g_mce_tag_fatal;
                break;
            case MCE_SEV_UCR:
                /* Recoverable iff RIPV=1. Without RIPV, IRET resumes at
                 * an unknown RIP — fatal even when UC=1 + PCC=0. */
                ev.recovered = ripv ? 1 : 0;
                if (ripv) {
                    pub_tag = g_mce_tag_recovered;
                } else {
                    any_fatal = true;
                    pub_tag = g_mce_tag_fatal;
                }
                break;
            case MCE_SEV_UCNA:
                /* Uncorrected, but the processor never signalled it to us —
                 * or never enabled it. Nothing was acted on and nothing is
                 * fatal; it is still published, because a subscriber counting
                 * UCNAs on a DIMM is exactly how the next SRAR is seen
                 * coming. */
                ev.recovered = 1;
                pub_tag = g_mce_tag_detected;
                break;
            case MCE_SEV_CORRECTED:
                ev.recovered = 1;
                pub_tag = g_mce_tag_detected;
                break;
            case MCE_SEV_NONE:
            default:
                break;
        }
        if (deferral_open && pub_tag != TOUCH_TAG_INVALID) {
            TouchPublishIrqPair(pub_tag, TOUCH_TAG_INVALID,
                                &ev, (uint16_t)sizeof(ev),
                                0u /* source_pid: kernel */, 0u /* flags */);
        }

        /* Take this bank's record NOW — after the clear below it exists
         * nowhere else. Who says it is decided after the walk. */
        {
            MceRecord r = {
                .status     = status,
                .addr       = addr,
                .misc       = misc,
                .mcg_status = mcg_status,
                .rip        = frame->rip,
                .phys       = poisoned_phys,
                .bank       = i,
                .severity   = (uint16_t)sev,
                .nested     = nested ? 1u : 0u,
                .rsv0       = 0,
            };
            MceNoteSlot *slot = mce_note_claim(&r);
            if (slot != NULL && claimed_n < MCE_MAX_BANKS) {
                claimed[claimed_n++] = slot;
            } else if (slot != NULL) {
                __atomic_store_n(&slot->in_use, 0, __ATOMIC_RELEASE);
                __atomic_fetch_add(&g_mce_notes_lost, 1, __ATOMIC_RELAXED);
            }
        }

        /* Clear bank status so we don't re-read on the next #MC. SDM
         * §15.3.2.2: write 0 clears VAL + all sticky bits. */
        mce_wrmsr(MCE_MSR_MC_STATUS_BASE + i * 4, 0ULL);
    }

    /* Clear MCG_STATUS.MCIP. SDM §15.3.1.2: "After the OS has read MCG_
     * STATUS, it must clear MCIP". Failing to do so blocks future #MC
     * delivery on this thread. From here on this core is an ordinary core
     * again: a second machine-check event no longer meets MCIP=1 and no
     * longer puts the processor into the architectural shutdown state. */
    mce_wrmsr(MCE_MSR_MCG_STATUS, 0ULL);

    /* ─── The verdict ───────────────────────────────────────────────
     *
     * Taken from the MCG_STATUS snapshot read at entry, not from the
     * register — that was just cleared above.
     *
     * RIPV belongs to the EVENT and to THIS logical processor, so it
     * outranks every bank's severity. SDM Vol 3B §15.10.4.1: "If the RIPV
     * flag is not set, program execution cannot be restarted reliably."
     * Linux acts on it before it has looked at a single bank record
     * (arch/x86/kernel/cpu/mce/core.c: `if (!(m->mcgstatus &
     * MCG_STATUS_RIPV)) kill_current_task = 1;`), and its severity table
     * carries MCESEV(PANIC, "In kernel and no restart IP", MCGMASK(
     * MCG_STATUS_RIPV, 0)). This is what closes the hole the old
     * tautological verdict left open: the core that actually consumed the
     * poisoned data carries the SRAR signature RIPV=0/EIPV=1, and it is
     * that core, not some other, that must never IRET.
     *
     * MCIP=0 inside a #MC handler means the event we are standing in is not
     * the one the processor is reporting — Linux calls exactly that
     * MCESEV(PANIC, "MCIP not set in MCA handler", MCGMASK(
     * MCG_STATUS_MCIP, 0)).
     *
     * A WALK THAT FOUND NOTHING UNCORRECTED IS NOT A VERDICT, in either
     * direction, and this is the one place where copying Linux's answer
     * would be wrong. An event that is not LMCE is broadcast to every
     * logical processor (§15.10.4.1: "an MCA signal is broadcast to all
     * logical processors in the system"); they race for one shared bank,
     * exactly one wins and clears it, and every other core finds nothing.
     * Linux answers that with mce_panic("Fatal machine check from unknown
     * source") — but that is the MONARCH's verdict, taken after
     * mce_start/mce_end have rendezvoused every processor and established
     * that NO core had a record. Taken locally, per core, it would mean
     * every loser of the race kills the machine over a perfectly
     * recoverable memory error, which is exactly what mce_migrate,
     * mce_safe_page_copy and mce_swap_pte exist to prevent.
     *
     * ─── WHY THERE IS NO MONARCH/SUBJECT RENDEZVOUS HERE ───
     *
     * Not an omission, and not a corner cut. A rendezvous is a wait for
     * cores that may never arrive, and the only way to end such a wait is a
     * guessed number — Linux spells it `mca_cfg.monarch_timeout` and simply
     * abandons the wait when it expires. A wait that gives up is this
     * project's definition of a defect, so the three things a rendezvous
     * would buy are bought another way:
     *
     *   1. FEWER SUBJECTS TO BEGIN WITH. LMCE is now armed on every logical
     *      processor whose gates allow it (mce_lmce_arm above), and
     *      §15.9.3.2 says what that buys: "MCE signal is broadcast to all
     *      logical processors ... except when the processor supports LMCE
     *      and LMCE is enabled by system software". An event that arrives
     *      with MCG_STATUS.LMCE_S set went to this processor ALONE and has
     *      no subjects to rendezvous with. Fewer, though, is not none:
     *      §15.9.3.1 says only that "some errors (not limited to UCR
     *      errors) may be delivered to only a single logical processor" and
     *      §15.3.1.5 warns that "software should make no assumptions about
     *      the type of error that hardware can choose to deliver as LMCE".
     *      So the broadcast case stays, and it stays first.
     *
     *   2. TELLING SUBJECT FROM SOURCE WITHOUT ASKING ANYONE. The bits that
     *      say it were latched in THIS core's MCG_STATUS at delivery, and
     *      the SDM's own handler pseudocode reads exactly them for exactly
     *      this case: "IF NOERROR = TRUE THEN IF NOT (MCG_RIPV = 1 AND
     *      MCG_EIPV = 0) THEN RESTARTABILITY = FALSE". Table 15-21 states
     *      it from the SRAR side — the non-affected logical processors are
     *      the ones with RIPV=1 and EIPV=0.
     *      RIPV alone is NOT that test, and the difference is the hole this
     *      verdict used to carry: Table 15-21 gives the AFFECTED processor
     *      of a recoverable-CONTINUABLE SRAR RIPV=1 with EIPV=1. Let another
     *      core win the shared bank and the one core that actually consumed
     *      the poison walks away with an empty hand, RIPV=1, and used to be
     *      IRETed straight back onto the load that consumed it — onto a
     *      frame whose migration had not even started. EIPV closes that, and
     *      closes it with a fact, not with a wait.
     *
     *   3. MAKING THE MACHINE STOP WHEN SOMEBODY ELSE HELD THE FATAL
     *      RECORD. A core that grades its own walk fatal returns false and
     *      idt.c stops the machine: panic_claim_or_halt → the dump →
     *      IPI_PANIC to every other core → `cli; hlt`. That is an EVENT, and
     *      the LAPIC's IRR makes it one that cannot be lost — but be exact
     *      about where it lands. IPI_PANIC is vector 0xF2 behind an ordinary
     *      interrupt gate, and vector 18 is an interrupt gate too, so a core
     *      standing right here has IF=0 and does NOT take it here. It waits
     *      in that core's IRR exactly as it waits out any spin_lock critical
     *      section, and is taken the instant IRET restores IF. The subject
     *      therefore resumes, runs briefly, and then halts: the stop arrives
     *      late, never by a deadline, and fails to arrive only for a core
     *      that never enables interrupts again — which is a property of
     *      every panic on this kernel, not of this handler.
     *
     * The record is still said, loudly, by the MCE_NOTE_NO_BANK line, so an
     * unexplained #MC is never silent.
     *
     * ─── WHAT DECLINING THE RENDEZVOUS STILL COSTS ───
     *
     * Three things bought another way is not four. What the same pseudocode
     * buys and this handler does not is named here rather than left for the
     * next reader to find. SDM Example 15-4 ("Machine-Check Error Handler
     * Pseudocode Supporting UCR") builds the broadcast case out of two
     * constructs, and the verdict above answers only the second of them:
     *
     *   - "Acquire SpinLock; ProcessorCount++; (* Allowing one logical
     *     processor at a time to examine machine check registers *)".
     *     An MCA-private spinlock is not one the interrupted frame can be
     *     holding, so the file header's deadlock argument does not reach
     *     it. What reaches it is that a core waiting on it waits with MCIP
     *     STILL SET, and a peer that dies inside its own walk leaves that
     *     core waiting until the next machine-check event turns into the
     *     architectural shutdown state (§15.3.1.2) — a wait with no event
     *     to end it, which is the very thing a deadline would then be
     *     invented for. So it is declined, and the price is stated: two
     *     logical processors that share a bank both decode it, so one
     *     failure can be said twice and its page migrated twice. Neither
     *     loses anything — pmm_set_poisoned is an atomic OR and
     *     mce_swap_pte is a CAS — but the ORDER of the two walks is not
     *     architecturally guaranteed, and a bank the peer clears between
     *     this core's STATUS read and its ADDR read hands this core an
     *     address §15.3.2.3 no longer calls valid. That window is open on
     *     every broadcast #MC and closed on every LMCE one, which is the
     *     other half of why arming LMCE above is worth the three gates.
     *
     *   - "IF ProcessorCount = MAX_PROCESSORS AND NOERROR = TRUE THEN
     *     Report RESTARTABILITY to console; Reset system", reached only
     *     after "Wait till ProcessorCount = MAX_PROCESSRS on system;
     *     (* implement a timeout and abort function if necessary *)".
     *     The wait IS the guessed number, in the SDM's own hand, and its
     *     whole yield is the machine-wide statement "nobody found a
     *     record". This handler makes no such statement and does not need
     *     to: the per-core question — may THIS thread be restarted — is
     *     answered by RIPV and EIPV above, from bits latched on this core
     *     at delivery. A #MC that no core on the machine can explain is
     *     therefore said and survived here where the SDM's pseudocode
     *     would reset; that is a deliberate divergence, not an oversight,
     *     and the MCE_NOTE_NO_BANK line is what makes it visible.
     *
     * A third divergence from the same Example belongs in the same list,
     * because it too is about a core that is not the source. Its SRAR arm
     * reads "IF MCACOD ... is recognized AND Current Processor is an
     * Affected Processor THEN Implement MCACOD specific recovery action ...
     * ELSE RESTARTABILITY = FALSE", which taken literally stops any core
     * that merely OBSERVED the broadcast and still found the bank. The walk
     * above lets that core continue on RIPV=1 with EIPV=0, because §15.9.3
     * says so in prose about the same error: the processors "that observed
     * but not affected by an SRAR error should find that the RIPV flag ...
     * is set and the EIPV flag ... is cleared, indicating that it is safe to
     * restart the execution ... after the recovery action is successfully
     * taken by system software" — and when this handler cannot START that
     * action the record is graded fatal above anyway. The pseudocode branch
     * is the outlier against its own chapter; Linux reads it the same way.
     *
     * Returning false hands the frame to idt.c's generic exception path,
     * which kills the process when the #MC came from ring 3 and halts when
     * it came from ring 0 — the two outcomes §15.10.4.1 leaves open when
     * the interrupted stream cannot be restarted.
     */
    bool fatal = (!mcip) || (!ripv) || any_fatal ||
                 (!uncorrected_seen && eipv);

    if (decoded == 0) {
        MceRecord r = {
            .status     = 0,
            .addr       = 0,
            .misc       = 0,
            .mcg_status = mcg_status,
            .rip        = frame->rip,
            .phys       = 0,
            .bank       = MCE_NOTE_NO_BANK,
            .severity   = (uint16_t)MCE_SEV_NONE,
            .nested     = 0,
            .rsv0       = 0,
        };
        MceNoteSlot *slot = mce_note_claim(&r);
        if (slot != NULL && claimed_n < MCE_MAX_BANKS) {
            claimed[claimed_n++] = slot;
        } else if (slot != NULL) {
            __atomic_store_n(&slot->in_use, 0, __ATOMIC_RELEASE);
            __atomic_fetch_add(&g_mce_notes_lost, 1, __ATOMIC_RELAXED);
        }
    }

    /* Who says what we saw. A kernel-mode fatal verdict ends in idt.c's
     * panic path, which halts every core without pumping a single baton, so
     * a record handed to a K-Core there is a record nobody ever reads — and
     * it is the fatal case whose numbers matter most. Before BatonInit there
     * is no K-Core to hand it to at all. In both cases this core says it
     * itself; MCIP is already cleared, and on the fatal road idt.c is about
     * to take the same console lock from the same frame regardless. */
    bool say_here = (fatal && (frame->cs & 3) == 0) || !deferral_open;

    for (uint32_t k = 0; k < claimed_n; k++) {
        mce_note_deliver(claimed[k], say_here);
    }

    return !fatal;
}
