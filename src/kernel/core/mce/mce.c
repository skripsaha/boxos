
#include "mce.h"
#include "mce_migrate.h"
#include "klib.h"
#include "cpuid.h"
#include "pmm.h"
#include "memtag.h"
#include "touch.h"
#include "logbook.h"


static bool      g_mce_initialized = false;
static bool      g_mce_lmce_on     = false;
static uint32_t  g_mce_bank_count  = 0;
static uint64_t  g_mce_mcg_cap     = 0;
static uint64_t  g_mce_ban_mask    = 0;

static TouchTag  g_mce_tag_detected   = TOUCH_TAG_INVALID;
static TouchTag  g_mce_tag_recovered  = TOUCH_TAG_INVALID;
static TouchTag  g_mce_tag_fatal      = TOUCH_TAG_INVALID;


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


static bool mce_supported_local(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(CPUID_LEAF_FEATURES, &eax, &ebx, &ecx, &edx);
    bool mce = (edx >> 7)  & 1u;
    bool mca = (edx >> 14) & 1u;
    return mce && mca;
}

static bool mce_skip_bank_zero(void) {
    cpu_identity_t id;
    cpu_read_identity(&id);
    if (id.vendor[0] != 'G') return false;
    if (id.family == 0x0F && id.model < 0x03) return true;
    if (id.family == 0x06 && id.model < 0x0E) return true;
    return false;
}


#define CR4_MCE_BIT (1ULL << 6)

static inline void mce_cr4_enable(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (!(cr4 & CR4_MCE_BIT)) {
        cr4 |= CR4_MCE_BIT;
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }
}


static bool mce_enable_lmce_if_supported(uint64_t mcg_cap) {
    if (!(mcg_cap & MCE_CAP_MCG_LMCE_P)) return false;

    uint64_t feat = mce_rdmsr(MCE_MSR_IA32_FEATURE_CONTROL);
    if ((feat & MCE_MSR_FEAT_LOCK_BIT) &&
        (feat & MCE_MSR_FEAT_LMCE_BIT)) {
        return true;
    }
    return false;
}


static void mce_program_banks(uint32_t count, uint64_t skip_mask) {
    for (uint32_t i = 0; i < count && i < MCE_MAX_BANKS; i++) {
        if (skip_mask & (1ULL << i)) continue;
        mce_wrmsr(MCE_MSR_MC_CTL_BASE + i * 4, 0xFFFFFFFFFFFFFFFFULL);
        mce_wrmsr(MCE_MSR_MC_STATUS_BASE + i * 4, 0ULL);
    }
}


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

    if (cap & MCE_CAP_MCG_CTL_P) {
        mce_wrmsr(MCE_MSR_MCG_CTL, 0xFFFFFFFFFFFFFFFFULL);
    }

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

    g_mce_tag_detected  = TouchLogbookIntern("mce:fault:detected");
    g_mce_tag_recovered = TouchLogbookIntern("mce:fault:recovered");
    g_mce_tag_fatal     = TouchLogbookIntern("mce:fault:fatal");
    debug_printf("[MCE] Touch handles cached: detected=0x%x recovered=0x%x "
                 "fatal=0x%x\n",
                 (unsigned)g_mce_tag_detected,
                 (unsigned)g_mce_tag_recovered,
                 (unsigned)g_mce_tag_fatal);

    g_mce_initialized = true;
}

void mce_ap_init(void) {
    if (!g_mce_initialized) return;
    if (!mce_supported_local()) return;

    mce_cr4_enable();

    uint64_t cap = mce_rdmsr(MCE_MSR_MCG_CAP);
    uint32_t bank_count = (uint32_t)(cap & MCE_CAP_COUNT_MASK);
    if (bank_count > g_mce_bank_count) bank_count = g_mce_bank_count;

    mce_program_banks(bank_count, g_mce_ban_mask);

    if (cap & MCE_CAP_MCG_CTL_P) {
        mce_wrmsr(MCE_MSR_MCG_CTL, 0xFFFFFFFFFFFFFFFFULL);
    }

    mce_wrmsr(MCE_MSR_MCG_STATUS, 0ULL);
}


typedef struct {
    uint64_t  status;
    uint64_t  addr;
    uint64_t  misc;
    uint64_t  mcg_status;
    uint64_t  rip;
    uint32_t  bank;
    uint8_t   severity;
    uint8_t   recovered;
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

static uintptr_t mce_extract_phys_page(uint64_t status, uint64_t addr) {
    if (!(status & MCE_MC_STATUS_ADDRV)) return 0;
    return addr & ~(uintptr_t)(PMM_PAGE_SIZE - 1);
}

bool mce_handle(interrupt_frame_t *frame) {
    bool any_fatal       = false;
    bool any_recovered   = false;
    bool nested_consumed = false;
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

        if (poisoned_phys != 0 && mce_migrate_note_nested(poisoned_phys)) {
            nested_consumed = true;
        }

        if (poisoned_phys != 0 &&
            (sev == MCE_SEV_UCR || sev == MCE_SEV_UC)) {
            pmm_set_poisoned(poisoned_phys);
            (void)MemTagApplyByPhys(poisoned_phys, 1, "mce:poisoned");

            (void)mce_migrate_request(poisoned_phys, sev, status);
        }

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

        TouchTag pub_tag = TOUCH_TAG_INVALID;
        switch (sev) {
            case MCE_SEV_UC:
                ev.recovered = 0;
                any_fatal = true;
                pub_tag = g_mce_tag_fatal;
                break;
            case MCE_SEV_UCR:
                ev.recovered = ripv ? 1 : 0;
                if (ripv) {
                    any_recovered = true;
                    pub_tag = g_mce_tag_recovered;
                } else {
                    any_fatal = true;
                    pub_tag = g_mce_tag_fatal;
                }
                break;
            case MCE_SEV_CORRECTED:
                ev.recovered = 1;
                any_recovered = true;
                pub_tag = g_mce_tag_detected;
                break;
            case MCE_SEV_NONE:
            default:
                break;
        }
        if (pub_tag != TOUCH_TAG_INVALID) {
            TouchPublishIrqPair(pub_tag, TOUCH_TAG_INVALID,
                                &ev, (uint16_t)sizeof(ev),
                                0u , 0u );
        }

        mce_wrmsr(MCE_MSR_MC_STATUS_BASE + i * 4, 0ULL);
    }

    mce_wrmsr(MCE_MSR_MCG_STATUS, 0ULL);

    if (nested_consumed) {
        debug_printf("[MCE] nested #MC consumed by in-flight migration\n");
    }

    if (any_fatal) return false;
    return any_recovered || !any_fatal;
}