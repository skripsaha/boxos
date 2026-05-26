#include "cpuid.h"
#include "klib.h"

cpu_capabilities_t g_cpu_caps;

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

    // Check APIC/x2APIC/XSAVE/AVX support (CPUID.1)
    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_FEATURES) {
        cpuid(CPUID_LEAF_FEATURES, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_apic = (edx & (1 << 9)) != 0;
        g_cpu_caps.has_x2apic = (ecx & (1 << 21)) != 0;
        g_cpu_caps.has_tsc_deadline = (ecx & (1 << 24)) != 0;
        g_cpu_caps.has_monitor = (ecx & (1 << 3)) != 0;
        g_cpu_caps.has_xsave = (ecx & (1 << 26)) != 0;
        g_cpu_caps.has_avx = (ecx & (1 << 28)) != 0;
        g_cpu_caps.has_pcid = (ecx & (1 << 17)) != 0;
        // PAT (CPUID.1:EDX[16]) — Intel SDM Vol 3A §11.12.2.
        g_cpu_caps.has_pat  = (edx & (1 << 16)) != 0;
    }

    // Check structured extended features (CPUID.7.0). Intel SDM Vol 2A.
    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_EXT_FEATURES) {
        cpuid_count(CPUID_LEAF_EXT_FEATURES, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_waitpkg  = (ecx & (1 << 5))  != 0;
        g_cpu_caps.has_avx512   = (ebx & (1 << 16)) != 0;
        // EBX[0]  FSGSBASE   — Intel SDM Vol 3A §2.5 (CR4.FSGSBASE).
        g_cpu_caps.has_fsgsbase = (ebx & (1 << 0))  != 0;
        g_cpu_caps.has_smep     = (ebx & (1 << 7))  != 0;
        // EBX[10] INVPCID    — Intel SDM Vol 3A §4.10.4.1.
        g_cpu_caps.has_invpcid  = (ebx & (1 << 10)) != 0;
        g_cpu_caps.has_smap     = (ebx & (1 << 20)) != 0;
        // ECX[2]  UMIP       — Intel SDM Vol 3A §2.5 (CR4.UMIP).
        g_cpu_caps.has_umip     = (ecx & (1 << 2))  != 0;
        // ECX[16] LA57       — Intel SDM Vol 3A §4.5 (5-level paging).
        g_cpu_caps.has_la57     = (ecx & (1 << 16)) != 0;
    }

    // Query XSAVE area size and supported components (CPUID.0xD:0)
    if (g_cpu_caps.has_xsave && g_cpu_caps.max_basic_leaf >= CPUID_LEAF_XSAVE) {
        cpuid_count(CPUID_LEAF_XSAVE, 0, &eax, &ebx, &ecx, &edx);
        // EAX = valid bits of XCR0 (lower 32)
        // EDX = valid bits of XCR0 (upper 32)
        // EBX = max size for currently enabled features
        // ECX = max size for all supported features
        g_cpu_caps.xcr0_supported = ((uint64_t)edx << 32) | eax;
        g_cpu_caps.xsave_area_size = ecx;
    }

    // Check Invariant TSC (CPUID.APM:EDX[8])
    cpuid(CPUID_LEAF_EXT_MAX, &eax, &ebx, &ecx, &edx);
    g_cpu_caps.max_extended_leaf = eax;

    if (g_cpu_caps.max_extended_leaf >= CPUID_LEAF_APM) {
        cpuid(CPUID_LEAF_APM, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_invariant_tsc = (edx & (1 << 8)) != 0;
    }

    if (g_cpu_caps.max_extended_leaf >= CPUID_LEAF_EXT_FEATURES2) {
        cpuid(CPUID_LEAF_EXT_FEATURES2, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_1gb_pages = (edx & (1 << 26)) != 0;
        // NX/XD bit — CPUID.80000001h:EDX[20]. Intel SDM Vol 3A §4.6.
        g_cpu_caps.has_nx        = (edx & (1 << 20)) != 0;
    }
}

/* Per-AP capability intersection.
 *
 * Heterogeneous CPUs (Intel Alder Lake-and-later "P+E" hybrid, ARM
 * big.LITTLE) advertise different ECX/EDX bits on different cores. If
 * the BSP is a P-core that publishes AVX-512 and the kernel caches
 * that in g_cpu_caps, a later AP-init on an E-core (which lacks
 * AVX-512) running kernel code that touches a ZMM register would #UD.
 *
 * Conservative fix: each AP re-runs CPUID locally and ANDs its
 * capability bits with whatever the BSP / previous APs already
 * recorded. The post-amp-boot g_cpu_caps reflects the INTERSECTION of
 * features available on every online CPU — no path enables a feature
 * the weakest core can't service.
 *
 * Same-CPU homogeneous systems (the common case under QEMU and on
 * pre-Alder-Lake hardware) AND identically with themselves and the
 * intersection is a no-op. Cost is one CPUID per AP at boot, never
 * after. */
void cpu_intersect_features_ap(void) {
    uint32_t eax, ebx, ecx, edx;

    /* AND only the booleans that gate code emission / instruction
     * usage. max_basic_leaf / max_extended_leaf / vendor_string /
     * xcr0_supported / xsave_area_size are descriptors of the CURRENT
     * core; we keep the BSP values for those since cross-core CPUID
     * variation in those fields is undefined behaviour (Intel SDM
     * Vol 2A §CPUID — "topology" leaves vary, but max-leaf and vendor
     * are required identical across all logical CPUs of a single
     * package). */

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
    }

    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_EXT_FEATURES) {
        cpuid_count(CPUID_LEAF_EXT_FEATURES, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_waitpkg  &= ((ecx & (1 << 5))  != 0);
        g_cpu_caps.has_avx512   &= ((ebx & (1 << 16)) != 0);
        g_cpu_caps.has_fsgsbase &= ((ebx & (1 << 0))  != 0);
        g_cpu_caps.has_smep     &= ((ebx & (1 << 7))  != 0);
        g_cpu_caps.has_invpcid  &= ((ebx & (1 << 10)) != 0);
        g_cpu_caps.has_smap     &= ((ebx & (1 << 20)) != 0);
        g_cpu_caps.has_umip     &= ((ecx & (1 << 2))  != 0);
        g_cpu_caps.has_la57     &= ((ecx & (1 << 16)) != 0);
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
}
