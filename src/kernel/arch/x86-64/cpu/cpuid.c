#include "cpuid.h"
#include "klib.h"

// Global CPU capabilities
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

    // Initialize structure
    memset(&g_cpu_caps, 0, sizeof(g_cpu_caps));

    // Get vendor string (CPUID.0)
    cpuid(0, &eax, &ebx, &ecx, &edx);
    g_cpu_caps.max_basic_leaf = eax;
    *((uint32_t*)&g_cpu_caps.vendor_string[0]) = ebx;
    *((uint32_t*)&g_cpu_caps.vendor_string[4]) = edx;
    *((uint32_t*)&g_cpu_caps.vendor_string[8]) = ecx;
    g_cpu_caps.vendor_string[12] = '\0';

    // Check WAITPKG support (CPUID.7.0:ECX[5])
    if (g_cpu_caps.max_basic_leaf >= 7) {
        cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_waitpkg = (ecx & (1 << 5)) != 0;
    }

    // Check Invariant TSC (CPUID.0x80000007:EDX[8])
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    g_cpu_caps.max_extended_leaf = eax;

    if (g_cpu_caps.max_extended_leaf >= 0x80000007) {
        cpuid(0x80000007, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_invariant_tsc = (edx & (1 << 8)) != 0;
    }
}
