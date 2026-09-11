#ifndef HYPERVISOR_H
#define HYPERVISOR_H

#include "ktypes.h"


typedef enum {
    HV_VENDOR_NONE = 0,
    HV_VENDOR_KVM,
    HV_VENDOR_TCG,
    HV_VENDOR_VMWARE,
    HV_VENDOR_HYPERV,
    HV_VENDOR_XEN,
    HV_VENDOR_BHYVE,
    HV_VENDOR_ACRN,
    HV_VENDOR_PARALLELS,
    HV_VENDOR_OTHER
} hv_vendor_t;

typedef struct {
    bool        present;
    hv_vendor_t vendor;
    char        vendor_string[13];
    uint32_t    max_hv_leaf;

    uint32_t    tsc_khz;
    uint32_t    apic_bus_khz;

    uint32_t    kvm_features;

    uint32_t    hyperv_privileges_eax;
    uint32_t    hyperv_privileges_edx;
} hypervisor_info_t;

extern hypervisor_info_t g_hypervisor;

void hypervisor_detect(void);

static inline bool hv_present(void)         { return g_hypervisor.present; }
static inline hv_vendor_t hv_vendor(void)    { return g_hypervisor.vendor; }
static inline uint32_t hv_tsc_khz(void)      { return g_hypervisor.tsc_khz; }
static inline uint32_t hv_apic_bus_khz(void) { return g_hypervisor.apic_bus_khz; }

bool hv_tsc_is_wallclock(void);

bool hv_has_kvmclock(void);

bool hv_has_hyperv_tsc_page(void);

bool hv_has_hyperv_frequency_msrs(void);

uint64_t hv_hyperv_tsc_khz_from_msr(void);

const char *hv_vendor_name(void);

#endif