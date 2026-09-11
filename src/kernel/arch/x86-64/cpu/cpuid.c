#include "cpuid.h"
#include "klib.h"

cpu_capabilities_t g_cpu_caps;

#define MSR_IA32_BIOS_SIGN_ID       0x0000008Bu
#define MSR_IA32_CORE_CAPABILITIES  0x000000CFu
#define MSR_TEST_CTL                0x00000033u
#define TEST_CTL_SPLIT_LOCK_AC_BIT  29u

static inline uint64_t cpu_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpu_wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : :
                     "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

void cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0)
    );
}

void cpuid_count(uint32_t leaf, uint32_t subleaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf)
    );
}

void cpu_detect_features(void) {
    uint32_t eax, ebx, ecx, edx;

    memset(&g_cpu_caps, 0, sizeof(g_cpu_caps));

    cpuid(CPUID_LEAF_VENDOR, &eax, &ebx, &ecx, &edx);
    g_cpu_caps.max_basic_leaf = eax;
    *((uint32_t*)&g_cpu_caps.vendor_string[0]) = ebx;
    *((uint32_t*)&g_cpu_caps.vendor_string[4]) = edx;
    *((uint32_t*)&g_cpu_caps.vendor_string[8]) = ecx;
    g_cpu_caps.vendor_string[12] = '\0';

    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_FEATURES) {
        cpuid(CPUID_LEAF_FEATURES, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_apic = (edx & (1 << 9)) != 0;
        g_cpu_caps.has_x2apic = (ecx & (1 << 21)) != 0;
        g_cpu_caps.has_tsc_deadline = (ecx & (1 << 24)) != 0;
        g_cpu_caps.has_monitor = (ecx & (1 << 3)) != 0;
        g_cpu_caps.has_xsave = (ecx & (1 << 26)) != 0;
        g_cpu_caps.has_avx = (ecx & (1 << 28)) != 0;
        g_cpu_caps.has_pcid = (ecx & (1 << 17)) != 0;
        g_cpu_caps.has_pat  = (edx & (1 << 16)) != 0;
        g_cpu_caps.has_hypervisor = (ecx & (1u << 31)) != 0;
        g_cpu_caps.has_rdrand = (ecx & (1u << 30)) != 0;
    }

    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_EXT_FEATURES) {
        cpuid_count(CPUID_LEAF_EXT_FEATURES, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_waitpkg  = (ecx & (1 << 5))  != 0;
        g_cpu_caps.has_avx512   = (ebx & (1 << 16)) != 0;
        g_cpu_caps.has_fsgsbase = (ebx & (1 << 0))  != 0;
        g_cpu_caps.has_rdseed   = (ebx & (1 << 18)) != 0;
        g_cpu_caps.has_smep     = (ebx & (1 << 7))  != 0;
        g_cpu_caps.has_invpcid  = (ebx & (1 << 10)) != 0;
        g_cpu_caps.has_smap     = (ebx & (1 << 20)) != 0;
        g_cpu_caps.has_umip     = (ecx & (1 << 2))  != 0;
        g_cpu_caps.has_la57     = (ecx & (1 << 16)) != 0;
        g_cpu_caps.has_tsc_adjust = (ebx & (1 << 1)) != 0;
        g_cpu_caps.has_erms = (ebx & (1 << 9))  != 0;
        g_cpu_caps.has_fsrm = (edx & (1 << 4))  != 0;
        g_cpu_caps.has_core_capabilities = (edx & (1u << 30)) != 0;
        g_cpu_caps.has_pku  = (ecx & (1u << 3))  != 0;
        g_cpu_caps.has_pks  = (ecx & (1u << 31)) != 0;
        g_cpu_caps.has_tme   = (ecx & (1u << 13)) != 0;
        g_cpu_caps.has_shstk = (ecx & (1u << 7))  != 0;
        g_cpu_caps.has_ibt   = (edx & (1u << 20)) != 0;
        g_cpu_caps.has_pconfig = (edx & (1u << 18)) != 0;
        if (eax >= 1) {
            uint32_t lam_eax, lam_ebx, lam_ecx, lam_edx;
            cpuid_count(CPUID_LEAF_EXT_FEATURES, 1,
                        &lam_eax, &lam_ebx, &lam_ecx, &lam_edx);
            g_cpu_caps.has_lam = (lam_eax & (1u << 26)) != 0;
        }
    }

    g_cpu_caps.has_tdx = false;
    if (g_cpu_caps.max_basic_leaf >= 0x21) {
        cpuid_count(0x21, 0, &eax, &ebx, &ecx, &edx);
        if (ebx == 0x65746E49u && edx == 0x5844546Cu && ecx == 0x20202020u) {
            g_cpu_caps.has_tdx = true;
        }
    }

    if (g_cpu_caps.has_xsave && g_cpu_caps.max_basic_leaf >= CPUID_LEAF_XSAVE) {
        cpuid_count(CPUID_LEAF_XSAVE, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.xcr0_supported = ((uint64_t)edx << 32) | eax;
        g_cpu_caps.xsave_area_size = ecx;
    }

    cpuid(CPUID_LEAF_EXT_MAX, &eax, &ebx, &ecx, &edx);
    g_cpu_caps.max_extended_leaf = eax;

    if (g_cpu_caps.max_extended_leaf >= CPUID_LEAF_APM) {
        cpuid(CPUID_LEAF_APM, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_invariant_tsc = (edx & (1 << 8)) != 0;
    }

    if (g_cpu_caps.max_extended_leaf >= CPUID_LEAF_EXT_FEATURES2) {
        cpuid(CPUID_LEAF_EXT_FEATURES2, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_1gb_pages = (edx & (1 << 26)) != 0;
        g_cpu_caps.has_nx        = (edx & (1 << 20)) != 0;
    }

    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_THERMAL_PM) {
        cpuid(CPUID_LEAF_THERMAL_PM, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_arat = (eax & (1u << 2)) != 0;
    }

    g_cpu_caps.monitor_line_min = 64;
    g_cpu_caps.monitor_line_max = 64;
    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_MONITOR) {
        cpuid(CPUID_LEAF_MONITOR, &eax, &ebx, &ecx, &edx);
        uint16_t min_line = (uint16_t)(eax & 0xFFFFu);
        uint16_t max_line = (uint16_t)(ebx & 0xFFFFu);
        if (min_line == 0) min_line = 64;
        if (max_line == 0) max_line = 64;
        g_cpu_caps.monitor_line_min = min_line;
        g_cpu_caps.monitor_line_max = max_line;
    }

    g_cpu_caps.has_split_lock_detect = false;
    if (g_cpu_caps.has_core_capabilities) {
        uint64_t core_cap = cpu_rdmsr(MSR_IA32_CORE_CAPABILITIES);
        g_cpu_caps.has_split_lock_detect = (core_cap & (1ULL << 5)) != 0;
    }
}


#define MSR_IA32_UMWAIT_CONTROL    0x000000E1
#define UMWAIT_CONTROL_CAP_MS      2u

static uint64_t umwait_control_compose(uint64_t tsc_freq_khz) {
    uint64_t cap_ticks = tsc_freq_khz * (uint64_t)UMWAIT_CONTROL_CAP_MS;
    if (cap_ticks > 0xFFFFFFFCULL) cap_ticks = 0xFFFFFFFCULL;
    return cap_ticks & ~3ULL;
}

void cpu_umwait_control_init(uint64_t tsc_freq_khz) {
    if (!g_cpu_caps.has_waitpkg) return;
    if (tsc_freq_khz == 0)       return;

    uint64_t v = umwait_control_compose(tsc_freq_khz);
    cpu_wrmsr(MSR_IA32_UMWAIT_CONTROL, v);
}

void cpu_test_ctl_init(void) {
    if (!g_cpu_caps.has_split_lock_detect) return;

    uint64_t v = cpu_rdmsr(MSR_TEST_CTL);
    v &= ~(1ULL << TEST_CTL_SPLIT_LOCK_AC_BIT);
    cpu_wrmsr(MSR_TEST_CTL, v);
}

uint32_t cpu_microcode_revision(void) {
    uint32_t a, b, c, d;
    cpuid(CPUID_LEAF_VENDOR, &a, &b, &c, &d);
    bool is_intel = (b == 0x756E6547u  &&
                     d == 0x49656E69u  &&
                     c == 0x6C65746Eu );

    if (is_intel) {
        cpu_wrmsr(MSR_IA32_BIOS_SIGN_ID, 0);
        cpuid(CPUID_LEAF_FEATURES, &a, &b, &c, &d);
        return (uint32_t)(cpu_rdmsr(MSR_IA32_BIOS_SIGN_ID) >> 32);
    }

    return (uint32_t)(cpu_rdmsr(MSR_IA32_BIOS_SIGN_ID) & 0xFFFFFFFFu);
}

void cpu_read_identity(cpu_identity_t* out) {
    if (!out) return;

    uint32_t a, b, c, d;

    cpuid(CPUID_LEAF_VENDOR, &a, &b, &c, &d);
    *((uint32_t*)&out->vendor[0]) = b;
    *((uint32_t*)&out->vendor[4]) = d;
    *((uint32_t*)&out->vendor[8]) = c;
    out->vendor[12] = '\0';

    cpuid(CPUID_LEAF_FEATURES, &a, &b, &c, &d);
    uint32_t base_family = (a >> 8)  & 0xF;
    uint32_t base_model  = (a >> 4)  & 0xF;
    uint32_t ext_family  = (a >> 20) & 0xFF;
    uint32_t ext_model   = (a >> 16) & 0xF;

    out->stepping = a & 0xF;
    out->type     = (a >> 12) & 0x3;
    out->family   = base_family + (base_family == 0x0F ? ext_family : 0);
    out->model    = base_model  | ((base_family == 0x06 || base_family == 0x0F)
                                    ? (ext_model << 4) : 0);
    out->apic_id  = (b >> 24) & 0xFF;

    out->microcode_rev = cpu_microcode_revision();
}

void cpu_log_identity(const char* prefix) {
    cpu_identity_t id;
    cpu_read_identity(&id);

    if (id.microcode_rev) {
        kprintf("[CPU] %s%svendor=%s family=0x%x model=0x%x stepping=%u "
                "apic=%u microcode=0x%08x\n",
                prefix ? prefix : "", prefix ? " " : "",
                id.vendor, id.family, id.model, id.stepping,
                id.apic_id, id.microcode_rev);
    } else {
        kprintf("[CPU] %s%svendor=%s family=0x%x model=0x%x stepping=%u "
                "apic=%u microcode=none\n",
                prefix ? prefix : "", prefix ? " " : "",
                id.vendor, id.family, id.model, id.stepping, id.apic_id);
    }
}


#define MSR_IA32_EFER               0xC0000080u
#define EFER_NXE_BIT                (1ULL << 11)

static bool g_no_execute_taken_up = false;

bool CpuNoExecuteTakenUp(void) {
    return g_no_execute_taken_up;
}

bool CpuTakeUpNoExecute(void) {
#ifdef CONFIG_NO_EXECUTE_REFUSED
    {
        uint64_t efer = cpu_rdmsr(MSR_IA32_EFER);
        if (efer & EFER_NXE_BIT) cpu_wrmsr(MSR_IA32_EFER, efer & ~EFER_NXE_BIT);
        kprintf("[CPU] no-execute refused by this build (EFER.NXE was %s, now "
                "clear) — bit 63 stays out of every page table entry\n",
                (efer & EFER_NXE_BIT) ? "on" : "off");
    }
    g_no_execute_taken_up = false;
    return false;
#else
    if (!g_cpu_caps.has_nx) {
        kprintf("[CPU] no NX on this machine (CPUID.80000001h:EDX[20]=0) — "
                "bit 63 stays out of every page table entry\n");
        g_no_execute_taken_up = false;
        return false;
    }

    uint64_t efer = cpu_rdmsr(MSR_IA32_EFER);
    bool     was_on = (efer & EFER_NXE_BIT) != 0;
    if (!was_on) {
        cpu_wrmsr(MSR_IA32_EFER, efer | EFER_NXE_BIT);
    }
    g_no_execute_taken_up = true;

    kprintf("[CPU] no-execute taken up (EFER.NXE was %s)\n",
            was_on ? "already on" : "off — the loader left it off");
    return true;
#endif
}

void cpu_intersect_features_ap(void) {
    uint32_t eax, ebx, ecx, edx;

    bool bsp_x2apic       = g_cpu_caps.has_x2apic;
    bool bsp_tsc_deadline = g_cpu_caps.has_tsc_deadline;
    bool bsp_monitor      = g_cpu_caps.has_monitor;
    bool bsp_xsave        = g_cpu_caps.has_xsave;
    bool bsp_avx          = g_cpu_caps.has_avx;
    bool bsp_pcid         = g_cpu_caps.has_pcid;
    bool bsp_pat          = g_cpu_caps.has_pat;
    bool bsp_avx512       = g_cpu_caps.has_avx512;
    bool bsp_fsgsbase     = g_cpu_caps.has_fsgsbase;
    bool bsp_smep         = g_cpu_caps.has_smep;
    bool bsp_smap         = g_cpu_caps.has_smap;
    bool bsp_umip         = g_cpu_caps.has_umip;
    bool bsp_invpcid      = g_cpu_caps.has_invpcid;
    bool bsp_tsc_adjust   = g_cpu_caps.has_tsc_adjust;
    bool bsp_erms         = g_cpu_caps.has_erms;
    bool bsp_fsrm         = g_cpu_caps.has_fsrm;
    bool bsp_inv_tsc      = g_cpu_caps.has_invariant_tsc;
    bool bsp_arat         = g_cpu_caps.has_arat;
    bool bsp_core_caps    = g_cpu_caps.has_core_capabilities;
    bool bsp_split_lock   = g_cpu_caps.has_split_lock_detect;


    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_FEATURES) {
        cpuid(CPUID_LEAF_FEATURES, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_apic    &= ((edx & (1 << 9))  != 0);
        g_cpu_caps.has_x2apic  &= ((ecx & (1 << 21)) != 0);
        g_cpu_caps.has_tsc_deadline &= ((ecx & (1 << 24)) != 0);
        g_cpu_caps.has_monitor &= ((ecx & (1 << 3)) != 0);
        g_cpu_caps.has_xsave   &= ((ecx & (1 << 26)) != 0);
        g_cpu_caps.has_avx     &= ((ecx & (1 << 28)) != 0);
        g_cpu_caps.has_pcid    &= ((ecx & (1 << 17)) != 0);
        g_cpu_caps.has_pat     &= ((edx & (1 << 16)) != 0);
        g_cpu_caps.has_hypervisor &= ((ecx & (1u << 31)) != 0);
        g_cpu_caps.has_rdrand  &= ((ecx & (1u << 30)) != 0);
    }

    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_EXT_FEATURES) {
        cpuid_count(CPUID_LEAF_EXT_FEATURES, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_waitpkg  &= ((ecx & (1 << 5))  != 0);
        g_cpu_caps.has_avx512   &= ((ebx & (1 << 16)) != 0);
        g_cpu_caps.has_fsgsbase &= ((ebx & (1 << 0))  != 0);
        g_cpu_caps.has_rdseed   &= ((ebx & (1 << 18)) != 0);
        g_cpu_caps.has_smep     &= ((ebx & (1 << 7))  != 0);
        g_cpu_caps.has_invpcid  &= ((ebx & (1 << 10)) != 0);
        g_cpu_caps.has_smap     &= ((ebx & (1 << 20)) != 0);
        g_cpu_caps.has_umip     &= ((ecx & (1 << 2))  != 0);
        g_cpu_caps.has_la57     &= ((ecx & (1 << 16)) != 0);
        g_cpu_caps.has_tsc_adjust &= ((ebx & (1 << 1))  != 0);
        g_cpu_caps.has_erms       &= ((ebx & (1 << 9))  != 0);
        g_cpu_caps.has_fsrm       &= ((edx & (1 << 4))  != 0);
        g_cpu_caps.has_core_capabilities &= ((edx & (1u << 30)) != 0);
        g_cpu_caps.has_pku &= ((ecx & (1u << 3))  != 0);
        g_cpu_caps.has_pks &= ((ecx & (1u << 31)) != 0);
        g_cpu_caps.has_tme   &= ((ecx & (1u << 13)) != 0);
        g_cpu_caps.has_shstk &= ((ecx & (1u << 7))  != 0);
        g_cpu_caps.has_ibt   &= ((edx & (1u << 20)) != 0);
        g_cpu_caps.has_pconfig &= ((edx & (1u << 18)) != 0);
        if (g_cpu_caps.has_tdx) {
            uint32_t tdx_eax, tdx_ebx, tdx_ecx, tdx_edx;
            if (eax >= 0x21) {
                cpuid_count(0x21, 0, &tdx_eax, &tdx_ebx, &tdx_ecx, &tdx_edx);
                bool tdx_match = (tdx_ebx == 0x65746E49u &&
                                  tdx_edx == 0x5844546Cu &&
                                  tdx_ecx == 0x20202020u);
                g_cpu_caps.has_tdx &= tdx_match;
            } else {
                g_cpu_caps.has_tdx = false;
            }
        }
        if (eax >= 1) {
            uint32_t lam_eax, lam_ebx, lam_ecx, lam_edx;
            cpuid_count(CPUID_LEAF_EXT_FEATURES, 1,
                        &lam_eax, &lam_ebx, &lam_ecx, &lam_edx);
            g_cpu_caps.has_lam &= ((lam_eax & (1u << 26)) != 0);
        } else {
            g_cpu_caps.has_lam = false;
        }
    }

    if (g_cpu_caps.max_extended_leaf >= CPUID_LEAF_APM) {
        cpuid(CPUID_LEAF_APM, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_invariant_tsc &= ((edx & (1 << 8)) != 0);
    }

    if (g_cpu_caps.max_extended_leaf >= CPUID_LEAF_EXT_FEATURES2) {
        cpuid(CPUID_LEAF_EXT_FEATURES2, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_1gb_pages &= ((edx & (1 << 26)) != 0);
        g_cpu_caps.has_nx        &= ((edx & (1 << 20)) != 0);
    }
    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_THERMAL_PM) {
        cpuid(CPUID_LEAF_THERMAL_PM, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_arat &= ((eax & (1u << 2)) != 0);
    }

    if (g_cpu_caps.has_core_capabilities) {
        uint64_t core_cap = cpu_rdmsr(MSR_IA32_CORE_CAPABILITIES);
        g_cpu_caps.has_split_lock_detect &= (core_cap & (1ULL << 5)) != 0;
    } else {
        g_cpu_caps.has_split_lock_detect = false;
    }

    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_MONITOR) {
        cpuid(CPUID_LEAF_MONITOR, &eax, &ebx, &ecx, &edx);
        uint16_t ap_min = (uint16_t)(eax & 0xFFFFu);
        uint16_t ap_max = (uint16_t)(ebx & 0xFFFFu);
        if (ap_min != 0 && ap_min < g_cpu_caps.monitor_line_min)
            g_cpu_caps.monitor_line_min = ap_min;
        if (ap_max != 0 && ap_max > g_cpu_caps.monitor_line_max)
            g_cpu_caps.monitor_line_max = ap_max;
    }

    #define _LOG_DROP(name, was) \
        do { if ((was) && !g_cpu_caps.has_##name) { \
            kprintf("[CPU] AP feature drop: " #name " unavailable on this core; " \
                    "kernel-wide " #name " now off\n"); \
        } } while (0)
    _LOG_DROP(x2apic,       bsp_x2apic);
    _LOG_DROP(tsc_deadline, bsp_tsc_deadline);
    _LOG_DROP(monitor,      bsp_monitor);
    _LOG_DROP(xsave,        bsp_xsave);
    _LOG_DROP(avx,          bsp_avx);
    _LOG_DROP(pcid,         bsp_pcid);
    _LOG_DROP(pat,          bsp_pat);
    _LOG_DROP(avx512,       bsp_avx512);
    _LOG_DROP(fsgsbase,     bsp_fsgsbase);
    _LOG_DROP(smep,         bsp_smep);
    _LOG_DROP(smap,         bsp_smap);
    _LOG_DROP(umip,         bsp_umip);
    _LOG_DROP(invpcid,      bsp_invpcid);
    _LOG_DROP(tsc_adjust,   bsp_tsc_adjust);
    _LOG_DROP(erms,         bsp_erms);
    _LOG_DROP(fsrm,         bsp_fsrm);
    _LOG_DROP(invariant_tsc, bsp_inv_tsc);
    _LOG_DROP(arat,         bsp_arat);
    _LOG_DROP(core_capabilities, bsp_core_caps);
    _LOG_DROP(split_lock_detect, bsp_split_lock);
    #undef _LOG_DROP
}