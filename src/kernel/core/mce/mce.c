/*
 * MCE — Machine Check Architecture implementation
 *
 * See mce.h for spec citations. Code paths:
 *   mce_init()        — BSP bring-up (CR4.MCE, MCG_CAP, per-bank CTL+CLEAR,
 *                       LMCE gate, telemetry)
 *   mce_ap_init()     — same minus IDT
 *   mce_handle(frame) — runs in #MC context (IST stack); MUST NOT call any
 *                       service that could itself trigger an error (kmalloc,
 *                       framebuffer write, etc.). Uses only kprintf+pmm+
 *                       MemTagApplyByPhys (all path-tested IRQ-safe).
 */

#include "mce.h"
#include "klib.h"
#include "cpuid.h"
#include "pmm.h"
#include "memtag.h"
#include "touch.h"

/* ─── State ────────────────────────────────────────────────────────── */

static bool      g_mce_initialized = false;
static bool      g_mce_lmce_on     = false;
static uint32_t  g_mce_bank_count  = 0;
static uint64_t  g_mce_mcg_cap     = 0;
static uint64_t  g_mce_ban_mask    = 0;  /* bit i set ⇒ bank i CTL skipped */

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

bool mce_handle(interrupt_frame_t *frame) {
    bool any_fatal       = false;
    bool any_recovered   = false;
    uint64_t mcg_status  = mce_rdmsr(MCE_MSR_MCG_STATUS);
    bool ripv            = (mcg_status & MCE_MCG_STATUS_RIPV) != 0;

    debug_printf("[MCE] #MC fired: MCG_STATUS=0x%016lx RIP=0x%016lx LMCE=%d\n",
                 (unsigned long)mcg_status,
                 (unsigned long)frame->rip,
                 (int)((mcg_status >> 3) & 1));

    for (uint32_t i = 0; i < g_mce_bank_count && i < MCE_MAX_BANKS; i++) {
        uint64_t status = mce_rdmsr(MCE_MSR_MC_STATUS_BASE + i * 4);
        if (!(status & MCE_MC_STATUS_VAL)) continue;

        uint64_t addr = (status & MCE_MC_STATUS_ADDRV)
                        ? mce_rdmsr(MCE_MSR_MC_ADDR_BASE + i * 4) : 0;
        uint64_t misc = (status & MCE_MC_STATUS_MISCV)
                        ? mce_rdmsr(MCE_MSR_MC_MISC_BASE + i * 4) : 0;

        mce_severity_t sev = mce_classify(status);

        uintptr_t poisoned_phys = mce_extract_phys_page(status, addr);

        debug_printf("[MCE]   bank[%u]: status=0x%016lx addr=0x%016lx "
                     "misc=0x%016lx sev=%s phys_page=0x%lx\n",
                     (unsigned)i,
                     (unsigned long)status,
                     (unsigned long)addr,
                     (unsigned long)misc,
                     mce_sev_str(sev),
                     (unsigned long)poisoned_phys);

        /* Mark the affected page poisoned for future allocations. Active
         * mappings keep working until the owning process exits. */
        if (poisoned_phys != 0 &&
            (sev == MCE_SEV_UCR || sev == MCE_SEV_UC)) {
            pmm_set_poisoned(poisoned_phys);
            (void)MemTagApplyByPhys(poisoned_phys, 1, "mce:poisoned");
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

        switch (sev) {
            case MCE_SEV_UC:
                ev.recovered = 0;
                any_fatal = true;
                TouchPublish("mce:fault:fatal", &ev, sizeof(ev));
                break;
            case MCE_SEV_UCR:
                /* Recoverable iff RIPV=1. Without RIPV, IRET resumes at
                 * an unknown RIP — fatal even when UC=1 + PCC=0. */
                ev.recovered = ripv ? 1 : 0;
                if (ripv) {
                    any_recovered = true;
                    TouchPublish("mce:fault:recovered", &ev, sizeof(ev));
                } else {
                    any_fatal = true;
                    TouchPublish("mce:fault:fatal", &ev, sizeof(ev));
                }
                break;
            case MCE_SEV_CORRECTED:
                ev.recovered = 1;
                any_recovered = true;
                TouchPublish("mce:fault:detected", &ev, sizeof(ev));
                break;
            case MCE_SEV_NONE:
            default:
                break;
        }

        /* Clear bank status so we don't re-read on the next #MC. SDM
         * §15.3.2.2: write 0 clears VAL + all sticky bits. */
        mce_wrmsr(MCE_MSR_MC_STATUS_BASE + i * 4, 0ULL);
    }

    /* Clear MCG_STATUS.MCIP. SDM §15.3.1.2: "After the OS has read MCG_
     * STATUS, it must clear MCIP". Failing to do so blocks future #MC
     * delivery on this thread. */
    mce_wrmsr(MCE_MSR_MCG_STATUS, 0ULL);

    if (any_fatal) return false;
    return any_recovered || !any_fatal;
}
