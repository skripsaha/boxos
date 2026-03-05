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

    cpuid(0, &eax, &ebx, &ecx, &edx);
    g_cpu_caps.max_basic_leaf = eax;
    *((uint32_t*)&g_cpu_caps.vendor_string[0]) = ebx;
    *((uint32_t*)&g_cpu_caps.vendor_string[4]) = edx;
    *((uint32_t*)&g_cpu_caps.vendor_string[8]) = ecx;
    g_cpu_caps.vendor_string[12] = '\0';

    // Check APIC/x2APIC/XSAVE/AVX support (CPUID.1)
    if (g_cpu_caps.max_basic_leaf >= 1) {
        cpuid(1, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_apic = (edx & (1 << 9)) != 0;
        g_cpu_caps.has_x2apic = (ecx & (1 << 21)) != 0;
        g_cpu_caps.has_xsave = (ecx & (1 << 26)) != 0;
        g_cpu_caps.has_avx = (ecx & (1 << 28)) != 0;
    }

    // Check WAITPKG and AVX-512 support (CPUID.7.0)
    if (g_cpu_caps.max_basic_leaf >= 7) {
        cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_waitpkg = (ecx & (1 << 5)) != 0;
        g_cpu_caps.has_avx512 = (ebx & (1 << 16)) != 0;
        g_cpu_caps.has_smep = (ebx & (1 << 7)) != 0;
        g_cpu_caps.has_smap = (ebx & (1 << 20)) != 0;
    }

    // Query XSAVE area size and supported components (CPUID.0xD:0)
    if (g_cpu_caps.has_xsave && g_cpu_caps.max_basic_leaf >= 0xD) {
        cpuid_count(0xD, 0, &eax, &ebx, &ecx, &edx);
        // EAX = valid bits of XCR0 (lower 32)
        // EDX = valid bits of XCR0 (upper 32)
        // EBX = max size for currently enabled features
        // ECX = max size for all supported features
        g_cpu_caps.xcr0_supported = ((uint64_t)edx << 32) | eax;
        g_cpu_caps.xsave_area_size = ecx;
    }

    // Check Invariant TSC (CPUID.0x80000007:EDX[8])
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    g_cpu_caps.max_extended_leaf = eax;

    if (g_cpu_caps.max_extended_leaf >= 0x80000007) {
        cpuid(0x80000007, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_invariant_tsc = (edx & (1 << 8)) != 0;
    }
}
