#include "hypervisor.h"
#include "cpuid.h"
#include "klib.h"

/*
 * Hypervisor detection.
 *
 * Implementation notes:
 *
 *  - CPUID.01H:ECX[31] ("hypervisor present") is the only architectural
 *    bit; the rest is industry convention. Intel SDM Vol 2A — CPUID —
 *    documents the bit but explicitly leaves the leaf range
 *    0x40000000-0x400000FF "reserved for software use" — it is the
 *    hypervisor families that agreed to expose vendor strings there.
 *
 *  - Some firmwares (notably Bochs) leave bit 31 clear even when the
 *    user obviously isn't on bare metal. We treat bit 31 = 0 as
 *    authoritative bare metal — if it lies, no vendor leaf will match
 *    either, so we still fall back to bare-metal calibration. The user
 *    bug report ("Bochs TSC reports 2 GHz panic fallback") is consistent
 *    with this: Bochs HPET/CPUID.15h both fail, so we hit the 2 GHz
 *    fallback. We can't trust Bochs to advertise itself, so we leave
 *    that path alone and just fix the panic-fallback to be CPUID.16h-
 *    aware (S5 in the audit report).
 *
 *  - Vendor matching is exact 12-byte comparison; we do NOT trim
 *    trailing spaces — every vendor uses a fixed 12-byte form per their
 *    spec. Microsoft Hv = "Microsoft Hv" (with single space), bhyve
 *    = "bhyve bhyve " (note trailing space).
 */

hypervisor_info_t g_hypervisor;

/* Vendor signature → enum lookup table. The string is exactly 12 bytes
 * (NOT NUL-terminated). We memcmp 12 bytes. */
typedef struct {
    const char  sig[13];   /* 12 chars + NUL for readability */
    hv_vendor_t vendor;
} hv_signature_t;

static const hv_signature_t g_signatures[] = {
    { "KVMKVMKVM\0\0\0", HV_VENDOR_KVM       },
    { "TCGTCGTCGTCG",    HV_VENDOR_TCG       },
    { "VMwareVMware",    HV_VENDOR_VMWARE    },
    { "Microsoft Hv",    HV_VENDOR_HYPERV    },
    { "XenVMMXenVMM",    HV_VENDOR_XEN       },
    { "bhyve bhyve ",    HV_VENDOR_BHYVE     },
    { "ACRNACRNACRN",    HV_VENDOR_ACRN      },
    { "prl hyperv  ",    HV_VENDOR_PARALLELS },
};

#define CPUID_FEATURE_HV_PRESENT_BIT  31

#define HV_LEAF_VENDOR     0x40000000u
#define HV_LEAF_KVM_FEATS  0x40000001u  /* KVM-specific features bitmap */
#define HV_LEAF_HV_PRIVS   0x40000003u  /* Hyper-V partition privileges */
#define HV_LEAF_TIMING     0x40000010u  /* TSC kHz / APIC bus kHz */

/* KVM feature bits (asm-generic/kvm_para.h KVM_FEATURE_*). */
#define KVM_FEATURE_CLOCKSOURCE2          (1u << 3)
#define KVM_FEATURE_CLOCKSOURCE_STABLE    (1u << 24)

/* Hyper-V partition privileges (TLFS §2.4.2 — CPUID 0x40000003:EAX).
 * Bit positions verified against linux arch/x86/include/asm/hyperv-
 * tlfs.h (HV_MSR_REFERENCE_TSC_AVAILABLE = BIT(9)). */
#define HV_PARTITION_ACCESS_REFERENCE_TSC (1u << 9)

/* CPUID 0x40000003:EDX["enlightenment info"] bit 8: FrequencyMsrsAvailable.
 * When set, MSR_HV_TSC_FREQUENCY (0x40000022) and MSR_HV_APIC_FREQUENCY
 * (0x40000023) are readable from the guest (TLFS §2.4.4). */
#define HV_FEATURE_FREQUENCY_MSRS_AVAILABLE  (1u << 8)

#define MSR_HV_TSC_FREQUENCY    0x40000022u
#define MSR_HV_APIC_FREQUENCY   0x40000023u

static inline uint64_t hv_rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static hv_vendor_t match_vendor(const char *sig12)
{
    for (size_t i = 0; i < sizeof(g_signatures) / sizeof(g_signatures[0]); i++) {
        if (memcmp(sig12, g_signatures[i].sig, 12) == 0)
            return g_signatures[i].vendor;
    }
    return HV_VENDOR_OTHER;
}

void hypervisor_detect(void)
{
    /* Run only once. Multiple AP-init paths may call us; the result is
     * identical across cores by virtue of being a package-wide property. */
    static volatile bool s_done = false;
    if (s_done) return;

    memset(&g_hypervisor, 0, sizeof(g_hypervisor));

    /* CPUID.01H:ECX[31] = hypervisor present. */
    uint32_t eax, ebx, ecx, edx;
    cpuid(CPUID_LEAF_FEATURES, &eax, &ebx, &ecx, &edx);
    g_hypervisor.present = (ecx & (1u << CPUID_FEATURE_HV_PRESENT_BIT)) != 0;

    if (!g_hypervisor.present) {
        g_hypervisor.vendor = HV_VENDOR_NONE;
        s_done = true;
        debug_printf("[HV] Bare metal (CPUID.01H:ECX[31] = 0)\n");
        return;
    }

    /* Leaf 0x40000000: max-hv-leaf + 12-byte vendor signature
     * (EBX, ECX, EDX in that order — same layout as the standard vendor
     * leaf 0x00 but with the words shifted by one). */
    cpuid(HV_LEAF_VENDOR, &eax, &ebx, &ecx, &edx);
    g_hypervisor.max_hv_leaf = eax;

    /* memcpy avoids the misaligned-store UB that
     * *((uint32_t *)&sig[0]) = ebx would inflict on a char[]. x86
     * tolerates misaligned writes, but the cast is undefined per the
     * C standard and -Wcast-align flags it. */
    char sig[13];
    memcpy(&sig[0], &ebx, 4);
    memcpy(&sig[4], &ecx, 4);
    memcpy(&sig[8], &edx, 4);
    sig[12] = '\0';

    g_hypervisor.vendor = match_vendor(sig);
    memcpy(g_hypervisor.vendor_string, sig, sizeof(g_hypervisor.vendor_string));

    /* Leaf 0x40000010 (TSC kHz / APIC bus kHz). Per industry convention,
     * present iff max_hv_leaf >= 0x40000010 — applies to KVM, VMware,
     * QEMU TCG when the right `-cpu` model is selected. */
    if (g_hypervisor.max_hv_leaf >= HV_LEAF_TIMING) {
        cpuid(HV_LEAF_TIMING, &eax, &ebx, &ecx, &edx);
        g_hypervisor.tsc_khz      = eax;
        g_hypervisor.apic_bus_khz = ebx;
    }

    /* Vendor-specific feature leaves. */
    if (g_hypervisor.vendor == HV_VENDOR_KVM &&
        g_hypervisor.max_hv_leaf >= HV_LEAF_KVM_FEATS) {
        cpuid(HV_LEAF_KVM_FEATS, &eax, &ebx, &ecx, &edx);
        g_hypervisor.kvm_features = eax;
    }

    if (g_hypervisor.vendor == HV_VENDOR_HYPERV &&
        g_hypervisor.max_hv_leaf >= HV_LEAF_HV_PRIVS) {
        cpuid(HV_LEAF_HV_PRIVS, &eax, &ebx, &ecx, &edx);
        g_hypervisor.hyperv_privileges_eax = eax;
        g_hypervisor.hyperv_privileges_edx = edx;
    }

    debug_printf("[HV] %s (sig='%s', max_leaf=0x%x, tsc=%u kHz, apic_bus=%u kHz)\n",
                 hv_vendor_name(),
                 g_hypervisor.vendor_string,
                 g_hypervisor.max_hv_leaf,
                 g_hypervisor.tsc_khz,
                 g_hypervisor.apic_bus_khz);

    if (g_hypervisor.vendor == HV_VENDOR_KVM)
        debug_printf("[HV]   KVM features: 0x%x (kvmclock=%s, stable=%s)\n",
                     g_hypervisor.kvm_features,
                     (g_hypervisor.kvm_features & KVM_FEATURE_CLOCKSOURCE2) ? "yes" : "no",
                     (g_hypervisor.kvm_features & KVM_FEATURE_CLOCKSOURCE_STABLE) ? "yes" : "no");

    if (g_hypervisor.vendor == HV_VENDOR_HYPERV)
        debug_printf("[HV]   Hyper-V privileges: EAX=0x%x EDX=0x%x (ref_tsc=%s)\n",
                     g_hypervisor.hyperv_privileges_eax,
                     g_hypervisor.hyperv_privileges_edx,
                     (g_hypervisor.hyperv_privileges_eax & HV_PARTITION_ACCESS_REFERENCE_TSC)
                         ? "yes" : "no");

    s_done = true;
}

bool hv_tsc_is_wallclock(void)
{
    /* Bare metal: always real cycles. */
    if (!g_hypervisor.present) return true;

    /* TCG: rdtsc() is an instruction counter, not a clock. Calibrating
     * a frequency from it produces nonsense (the user-reported 71/40
     * kHz). Mark untrusted; HPET counter must be the primary clocksource
     * under TCG. */
    if (g_hypervisor.vendor == HV_VENDOR_TCG) return false;

    /* KVM, VMware, Hyper-V, Xen and friends pass the host TSC through
     * with a per-VM scale; rdtsc() correctly reports cycles. */
    return true;
}

bool hv_has_kvmclock(void)
{
    return g_hypervisor.vendor == HV_VENDOR_KVM &&
           (g_hypervisor.kvm_features & KVM_FEATURE_CLOCKSOURCE2) != 0;
}

bool hv_has_hyperv_tsc_page(void)
{
    return g_hypervisor.vendor == HV_VENDOR_HYPERV &&
           (g_hypervisor.hyperv_privileges_eax & HV_PARTITION_ACCESS_REFERENCE_TSC) != 0;
}

bool hv_has_hyperv_frequency_msrs(void)
{
    return g_hypervisor.vendor == HV_VENDOR_HYPERV &&
           (g_hypervisor.hyperv_privileges_edx & HV_FEATURE_FREQUENCY_MSRS_AVAILABLE) != 0;
}

uint64_t hv_hyperv_tsc_khz_from_msr(void)
{
    if (!hv_has_hyperv_frequency_msrs()) return 0;
    /* MSR returns TSC frequency in Hz. Convert to kHz. */
    uint64_t hz = hv_rdmsr(MSR_HV_TSC_FREQUENCY);
    return hz / 1000ULL;
}

const char *hv_vendor_name(void)
{
    switch (g_hypervisor.vendor) {
    case HV_VENDOR_NONE:      return "bare metal";
    case HV_VENDOR_KVM:       return "KVM";
    case HV_VENDOR_TCG:       return "QEMU/TCG";
    case HV_VENDOR_VMWARE:    return "VMware";
    case HV_VENDOR_HYPERV:    return "Hyper-V";
    case HV_VENDOR_XEN:       return "Xen";
    case HV_VENDOR_BHYVE:     return "bhyve";
    case HV_VENDOR_ACRN:      return "ACRN";
    case HV_VENDOR_PARALLELS: return "Parallels";
    case HV_VENDOR_OTHER:     return "unknown hypervisor";
    }
    return "?";
}
