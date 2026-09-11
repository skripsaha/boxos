#ifndef ARCH_X86_64_CPUID_H
#define ARCH_X86_64_CPUID_H

#include "ktypes.h"

#define CPUID_LEAF_VENDOR        0x00000000
#define CPUID_LEAF_FEATURES      0x00000001
#define CPUID_LEAF_MONITOR       0x00000005
#define CPUID_LEAF_THERMAL_PM    0x00000006
#define CPUID_LEAF_EXT_FEATURES  0x00000007
#define CPUID_LEAF_XSAVE         0x0000000D
#define CPUID_LEAF_EXT_MAX       0x80000000
#define CPUID_LEAF_EXT_FEATURES2 0x80000001
#define CPUID_LEAF_APM           0x80000007
#define CPUID_LEAF_ADDR_SIZE     0x80000008

typedef struct {
    bool has_apic;
    bool has_x2apic;
    bool has_tsc_deadline;
    bool has_monitor;
    bool has_waitpkg;
    bool has_invariant_tsc;
    bool has_xsave;
    bool has_avx;
    bool has_avx512;
    bool has_smep;
    bool has_smap;
    bool has_umip;
    bool has_fsgsbase;
    bool has_rdrand;
    bool has_rdseed;
    bool has_la57;
    bool has_invpcid;
    bool has_nx;
    bool has_pat;
    bool has_1gb_pages;
    bool has_pcid;
    bool has_hypervisor;
    bool has_tsc_adjust;
    bool has_arat;
    bool has_erms;
    bool has_fsrm;
    bool has_core_capabilities;
    bool has_split_lock_detect;
    bool has_pku;
    bool has_pks;
    bool has_lam;
    bool has_tme;
    bool has_shstk;
    bool has_ibt;
    bool has_pconfig;
    bool has_tdx;
    uint16_t monitor_line_min;
    uint16_t monitor_line_max;
    char vendor_string[13];
    uint32_t max_basic_leaf;
    uint32_t max_extended_leaf;
    uint32_t xsave_area_size;
    uint64_t xcr0_supported;
} cpu_capabilities_t;

extern cpu_capabilities_t g_cpu_caps;

void cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx);
void cpuid_count(uint32_t leaf, uint32_t subleaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx);
void cpu_detect_features(void);

void cpu_intersect_features_ap(void);

bool CpuTakeUpNoExecute(void);

bool CpuNoExecuteTakenUp(void);

void cpu_umwait_control_init(uint64_t tsc_freq_khz);

void cpu_test_ctl_init(void);

uint32_t cpu_microcode_revision(void);

typedef struct {
    char     vendor[13];
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t type;
    uint32_t microcode_rev;
    uint32_t apic_id;
} cpu_identity_t;

void cpu_read_identity(cpu_identity_t* out);

void cpu_log_identity(const char* prefix);

static inline uint8_t cpuid_get_maxphyaddr(void) {
    uint32_t eax, ebx, ecx, edx;

    cpuid(CPUID_LEAF_EXT_MAX, &eax, &ebx, &ecx, &edx);

    if (eax < CPUID_LEAF_ADDR_SIZE) {
        return 36;
    }

    cpuid(CPUID_LEAF_ADDR_SIZE, &eax, &ebx, &ecx, &edx);
    uint8_t phys_bits = (uint8_t)(eax & 0xFF);

    if (phys_bits < 32 || phys_bits > 52) {
        return 36;
    }

    return phys_bits;
}

static inline uint8_t cpuid_get_maxvirtaddr(void) {
    uint32_t eax, ebx, ecx, edx;

    cpuid(CPUID_LEAF_EXT_MAX, &eax, &ebx, &ecx, &edx);

    if (eax < CPUID_LEAF_ADDR_SIZE) {
        return 48;
    }

    cpuid(CPUID_LEAF_ADDR_SIZE, &eax, &ebx, &ecx, &edx);
    uint8_t virt_bits = (uint8_t)((eax >> 8) & 0xFF);

    if (virt_bits < 48 || virt_bits > 57) {
        return 48;
    }

    return virt_bits;
}

#endif