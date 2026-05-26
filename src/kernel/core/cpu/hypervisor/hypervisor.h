#ifndef HYPERVISOR_H
#define HYPERVISOR_H

#include "ktypes.h"

/*
 * Hypervisor detection.
 *
 * Real PCs and modern VMs are both first-class targets for BoxOS. Native
 * silicon publishes CPUID.01H:ECX[31] = 0; every hypervisor sets it to 1
 * and a single vendor string at CPUID leaf 0x40000000 identifies which
 * one (Intel SDM Vol 2A — CPUID — "Hypervisor present" bit; the leaf
 * range 0x40000000-0x400000FF is reserved by industry convention for
 * the hypervisor interface — Microsoft TLFS §2.4.1).
 *
 * The detection answers three questions used by the timer subsystem:
 *
 *   1. Should we trust rdtsc() as a wall-clock cycle counter?
 *      Under QEMU TCG (signature "TCGTCGTCGTCG") the TSC is an
 *      instruction counter, not a clock. Calibrating CPU frequency
 *      from rdtsc() under TCG returns garbage like the user-reported
 *      "71 kHz" / "40 kHz" because rdtsc() advances at the rate
 *      translation blocks execute, not at the rate the host CPU spins.
 *      KVM passes the host TSC through with a fixed scale and is
 *      trustworthy.
 *
 *   2. Is there a hypervisor-supplied TSC frequency we should prefer
 *      over measurement? CPUID leaf 0x40000010 returns
 *      EAX = TSC kHz, EBX = APIC bus kHz on KVM, VMware, and TCG
 *      when QEMU runs with `-cpu` that enables the leaf. This avoids
 *      every measurement-based calibration error in one read.
 *
 *   3. Should we wire up a para-virtual clocksource (kvmclock,
 *      Hyper-V reference TSC page)? Those are stable across live
 *      migration and survive host frequency scaling — bare TSC
 *      doesn't.
 */

typedef enum {
    HV_VENDOR_NONE = 0,    /* Bare metal */
    HV_VENDOR_KVM,         /* "KVMKVMKVM\0\0\0" */
    HV_VENDOR_TCG,         /* "TCGTCGTCGTCG"   */
    HV_VENDOR_VMWARE,      /* "VMwareVMware"   */
    HV_VENDOR_HYPERV,      /* "Microsoft Hv"   */
    HV_VENDOR_XEN,         /* "XenVMMXenVMM"   */
    HV_VENDOR_BHYVE,       /* "bhyve bhyve "   */
    HV_VENDOR_ACRN,        /* "ACRNACRNACRN"   */
    HV_VENDOR_PARALLELS,   /* "prl hyperv "    */
    HV_VENDOR_OTHER        /* Unknown signature */
} hv_vendor_t;

typedef struct {
    bool        present;          /* CPUID.01H:ECX[31] = 1 */
    hv_vendor_t vendor;
    char        vendor_string[13]; /* NUL-terminated copy of the 12-byte sig */
    uint32_t    max_hv_leaf;      /* CPUID.40000000h:EAX — highest leaf */

    /* CPUID 0x40000010 — exposed by KVM, VMware, TCG (with the right
     * `-cpu` option). EAX = TSC freq in kHz, EBX = APIC bus freq in kHz.
     * Both are zero when not present. */
    uint32_t    tsc_khz;
    uint32_t    apic_bus_khz;

    /* KVM-specific features bitmap from CPUID 0x40000001 EAX. Bits we
     * care about (asm-generic/kvm_para.h):
     *   bit 0  KVM_FEATURE_CLOCKSOURCE      (deprecated MSR 0x11/0x12)
     *   bit 3  KVM_FEATURE_CLOCKSOURCE2     (MSR_KVM_SYSTEM_TIME_NEW 0x4b564d01)
     *   bit 24 KVM_FEATURE_CLOCKSOURCE_STABLE_BIT
     */
    uint32_t    kvm_features;

    /* Hyper-V partition privilege flags from CPUID 0x40000003. Bit
     * positions per linux arch/x86/include/asm/hyperv-tlfs.h:
     *   EAX bit 1 = HV_MSR_TIME_REF_COUNT_AVAILABLE
     *   EAX bit 9 = HV_MSR_REFERENCE_TSC_AVAILABLE
     *               (reference TSC page at MSR 0x40000021)
     *   EDX bit 8 = HV_FEATURE_FREQUENCY_MSRS_AVAILABLE
     *               (MSR_HV_TSC_FREQUENCY / MSR_HV_APIC_FREQUENCY)
     */
    uint32_t    hyperv_privileges_eax;
    uint32_t    hyperv_privileges_edx;
} hypervisor_info_t;

extern hypervisor_info_t g_hypervisor;

/* Called once during early boot, after cpu_detect_features() so we
 * know we can run CPUID at all. Reads CPUID.01H:ECX[31] then probes
 * leaf 0x40000000 family for the vendor string, hypervisor TSC info,
 * and vendor-specific feature bitmaps. Idempotent — re-running on
 * APs is a no-op. */
void hypervisor_detect(void);

/* Convenience helpers. Return constants known after hypervisor_detect()
 * (safe to call from any context, no locking). */
static inline bool hv_present(void)         { return g_hypervisor.present; }
static inline hv_vendor_t hv_vendor(void)    { return g_hypervisor.vendor; }
static inline uint32_t hv_tsc_khz(void)      { return g_hypervisor.tsc_khz; }
static inline uint32_t hv_apic_bus_khz(void) { return g_hypervisor.apic_bus_khz; }

/* True iff rdtsc() under this hypervisor reports host-clock cycles
 * (KVM, VMware, Hyper-V, bare metal). False on TCG/Bochs where the
 * TSC is synthetic and unsafe to calibrate. */
bool hv_tsc_is_wallclock(void);

/* True iff the kvmclock pvclock interface is available (KVM with
 * KVM_FEATURE_CLOCKSOURCE2 advertised). */
bool hv_has_kvmclock(void);

/* True iff the Hyper-V reference TSC page is available. */
bool hv_has_hyperv_tsc_page(void);

/* True iff Hyper-V exposes the FrequencyMsrs facility (CPUID
 * 0x40000003:EDX[8]). When set the guest may RDMSR MSR
 * HV_X64_MSR_TSC_FREQUENCY (0x40000022) and HV_X64_MSR_APIC_FREQUENCY
 * (0x40000023) to read exact TSC / APIC bus frequencies in Hz. */
bool hv_has_hyperv_frequency_msrs(void);

/* Hyper-V MSR-supplied TSC frequency in kHz. Returns 0 if the MSR
 * is not available. */
uint64_t hv_hyperv_tsc_khz_from_msr(void);

/* Short vendor name for boot logs. */
const char *hv_vendor_name(void);

#endif /* HYPERVISOR_H */
