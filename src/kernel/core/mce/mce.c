/*
 * MCE — Machine Check Architecture implementation
 *
 * See mce.h for spec citations. Code paths:
 *   mce_init()        — BSP bring-up (CR4.MCE, MCG_CAP, per-bank CTL+CLEAR,
 *                       LMCE gate, telemetry)
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

/* ─── State ────────────────────────────────────────────────────────── */

static bool      g_mce_initialized = false;
static bool      g_mce_lmce_on     = false;
static uint32_t  g_mce_bank_count  = 0;
static uint64_t  g_mce_mcg_cap     = 0;
static uint64_t  g_mce_ban_mask    = 0;  /* bit i set ⇒ bank i CTL skipped */

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

/* Intel bank-0 historical hazard. P54C/P55C and a handful of early P-IV
 * SKUs hang or report spurious errors when bank 0 CTL is written all-1s.
 * Linux mce/intel.c skips bank 0 on those families. Modern Intel (Family
 * 6 Model >= 0x0F, all family >= 0xF after Tejas) handles bank 0
 * normally. We detect by family/model. AMD: no equivalent hazard. */
static bool mce_skip_bank_zero(void) {
    cpu_identity_t id;
    cpu_read_identity(&id);
    if (id.vendor[0] != 'G') return false;  /* "GenuineIntel" check */
    /* Pre-Pentium-Pro families had no MCA; we don't reach here on them. */
    if (id.family == 0x0F && id.model < 0x03) return true;  /* Willamette/Northwood */
    if (id.family == 0x06 && id.model < 0x0E) return true;  /* Pre-Core (Banias/Dothan) */
    return false;
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

/* ─── LMCE bring-up (Intel SDM §15.3.1.4) ──────────────────────────── */

static bool mce_enable_lmce_if_supported(uint64_t mcg_cap) {
    if (!(mcg_cap & MCE_CAP_MCG_LMCE_P)) return false;

    /* LMCE requires IA32_FEATURE_CONTROL.LOCK=1 AND LMCE_EN=1. If the
     * firmware hasn't enabled LMCE in IA32_FEATURE_CONTROL we leave it
     * off (writing those bits ourselves would conflict with TXT/VMX
     * negotiation done by firmware). */
    uint64_t feat = mce_rdmsr(MCE_MSR_IA32_FEATURE_CONTROL);
    if ((feat & MCE_MSR_FEAT_LOCK_BIT) &&
        (feat & MCE_MSR_FEAT_LMCE_BIT)) {
        return true;
    }
    return false;
}

/* ─── Bank programming (used by BSP + per-AP) ──────────────────────── */

static void mce_program_banks(uint32_t count, uint64_t skip_mask) {
    for (uint32_t i = 0; i < count && i < MCE_MAX_BANKS; i++) {
        if (skip_mask & (1ULL << i)) continue;
        /* Enable all error sources on this bank. SDM Vol 3B §15.3.2.1:
         * IA32_MCi_CTL is bank-specific; writing 0xFFFFFFFFFFFFFFFF is
         * the canonical "enable everything" pattern. */
        mce_wrmsr(MCE_MSR_MC_CTL_BASE + i * 4, 0xFFFFFFFFFFFFFFFFULL);
        /* Clear any stale status the firmware left behind. SDM §15.3.2.2
         * notes that BIOS POST may have left UC errors logged. */
        mce_wrmsr(MCE_MSR_MC_STATUS_BASE + i * 4, 0ULL);
    }
}

/* ─── Public API ──────────────────────────────────────────────────── */

bool mce_is_initialized(void) { return g_mce_initialized; }
uint32_t mce_bank_count(void) { return g_mce_bank_count; }
bool mce_lmce_enabled(void)   { return g_mce_lmce_on; }

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
    g_mce_lmce_on    = mce_enable_lmce_if_supported(cap);

    debug_printf("[MCE] BSP init: banks=%u MCG_CTL_P=%d CMCI_P=%d LMCE_P=%d "
                 "LMCE_active=%d skip_mask=0x%lx cap=0x%016lx\n",
                 (unsigned)bank_count,
                 (int)((cap >> 8)  & 1u),
                 (int)((cap >> 10) & 1u),
                 (int)((cap >> 27) & 1u),
                 (int)g_mce_lmce_on,
                 (unsigned long)skip,
                 (unsigned long)cap);

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
    if (!g_mce_initialized) return;          /* BSP refused — skip AP */
    if (!mce_supported_local()) return;      /* this AP lacks MCA */

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

static mce_severity_t mce_classify(uint64_t status) {
    if (!(status & MCE_MC_STATUS_VAL)) return MCE_SEV_NONE;
    if (!(status & MCE_MC_STATUS_UC))  return MCE_SEV_CORRECTED;
    if (status & MCE_MC_STATUS_PCC)    return MCE_SEV_UC;
    return MCE_SEV_UCR;
}

static const char *mce_sev_str(mce_severity_t s) {
    switch (s) {
        case MCE_SEV_NONE:      return "NONE";
        case MCE_SEV_CORRECTED: return "CORRECTED";
        case MCE_SEV_UCR:       return "UCR";
        case MCE_SEV_UC:        return "UC";
        default:                return "?";
    }
}

/* Page-aligned phys address recovered from a bank's ADDR register +
 * the granularity hint in MISC.LSB (bits 5:0 of MISC, when MISCV=1).
 * SDM §15.3.2.4. Return 0 if ADDRV not set. */
static uintptr_t mce_extract_phys_page(uint64_t status, uint64_t addr) {
    if (!(status & MCE_MC_STATUS_ADDRV)) return 0;
    return addr & ~(uintptr_t)(PMM_PAGE_SIZE - 1);
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

    if (r->bank == MCE_NOTE_NO_BANK) {
        kprintf("[MCE] #MC with no valid bank on this logical processor. "
                "MCG_STATUS=0x%016lx (RIPV=%d EIPV=%d MCIP=%d) RIP=0x%016lx. "
                "Either the record sits in a bank this CPU does not walk, or "
                "another logical processor took the same broadcast signal and "
                "cleared the shared bank first. RIPV decides what happens to "
                "this thread; the record itself is somebody else's to say\n",
                (unsigned long)r->mcg_status, ripv, eipv, mcip,
                (unsigned long)r->rip);
        return;
    }

    kprintf("[MCE] bank[%u] %s status=0x%016lx addr=0x%016lx misc=0x%016lx "
            "page=0x%lx MCG_STATUS=0x%016lx (RIPV=%d EIPV=%d MCIP=%d) "
            "RIP=0x%016lx%s\n",
            (unsigned)r->bank, mce_sev_str((mce_severity_t)r->severity),
            (unsigned long)r->status, (unsigned long)r->addr,
            (unsigned long)r->misc, (unsigned long)r->phys,
            (unsigned long)r->mcg_status, ripv, eipv, mcip,
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

        uintptr_t poisoned_phys = mce_extract_phys_page(status, addr);

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
        if (poisoned_phys != 0 &&
            (sev == MCE_SEV_UCR || sev == MCE_SEV_UC)) {
            pmm_set_poisoned(poisoned_phys);

            /* Queue a deferred migration. The actual copy + PTE swap +
             * TLB shootdown runs in K-Core context off the slot's baton (the
             * #MC IST stack can't safely take pmm_alloc / region-bucket
             * locks). Failure to queue (ring saturation) is non-fatal:
             * the page stays poisoned in the PMM bitmap, so the owning
             * process will eventually #PF on access and die cleanly. */
            if (deferral_open) {
                (void)mce_migrate_request(poisoned_phys, sev, status);
            }
        }

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
     * An EMPTY WALK IS NOT A VERDICT, in either direction, and this is the
     * one place where copying Linux's answer would be wrong. On this kernel
     * IA32_MCG_EXT_CTL[LMCE_EN] is never written, so every Intel #MC is
     * broadcast to every logical processor (SDM §15.10.4.1: "an MCA signal
     * is broadcast to all logical processors in the system"). They race for
     * one shared bank; exactly one wins and clears it, and every other core
     * finds nothing. Linux answers the empty walk with mce_panic("Fatal
     * machine check from unknown source") — but that is the MONARCH's
     * verdict after mce_start/mce_end have rendezvoused all of them and
     * found that NO core had a record. Taking that verdict locally, per
     * core, would mean every loser of a perfectly recoverable memory error
     * kills the machine, which is precisely what mce_migrate,
     * mce_safe_page_copy and mce_swap_pte exist to prevent. This kernel has
     * no rendezvous — one cannot be built without a wait bounded by a
     * guessed number, which this project does not allow — so the empty walk
     * is answered with the one fact that is genuinely local and genuinely
     * authoritative: RIPV. The record is still said, loudly, by the
     * MCE_NOTE_NO_BANK line, so an unexplained #MC is never silent.
     *
     * Returning false hands the frame to idt.c's generic exception path,
     * which kills the process when the #MC came from ring 3 and halts when
     * it came from ring 0 — the two outcomes §15.10.4.1 leaves open when
     * the interrupted stream cannot be restarted.
     */
    bool fatal = (!mcip) || (!ripv) || any_fatal;

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
