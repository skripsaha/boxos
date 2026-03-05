#ifndef ARCH_X86_64_CPUID_H
#define ARCH_X86_64_CPUID_H

#include "ktypes.h"

typedef struct {
    bool has_apic;              // On-chip APIC (CPUID.1:EDX[9])
    bool has_x2apic;            // x2APIC support (CPUID.1:ECX[21])
    bool has_waitpkg;           // UMONITOR/UMWAIT support (CPUID.7.0:ECX[5])
    bool has_invariant_tsc;     // Invariant TSC
    bool has_xsave;             // XSAVE/XRSTOR support (CPUID.1:ECX[26])
    bool has_avx;               // AVX support (CPUID.1:ECX[28])
    bool has_avx512;            // AVX-512 Foundation (CPUID.7.0:EBX[16])
    char vendor_string[13];     // CPU vendor (e.g., "GenuineIntel")
    uint32_t max_basic_leaf;    // Maximum CPUID basic leaf
    uint32_t max_extended_leaf; // Maximum CPUID extended leaf
    uint32_t xsave_area_size;   // Total XSAVE area size from CPUID.0xD:0 (0 if no XSAVE)
    uint64_t xcr0_supported;    // Supported XCR0 bits from CPUID.0xD:0
} cpu_capabilities_t;

extern cpu_capabilities_t g_cpu_caps;

void cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx);
void cpuid_count(uint32_t leaf, uint32_t subleaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx);
void cpu_detect_features(void);

static inline uint8_t cpuid_get_maxphyaddr(void) {
    uint32_t eax, ebx, ecx, edx;

    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);

    if (eax < 0x80000008) {
        return 36;
    }

    cpuid(0x80000008, &eax, &ebx, &ecx, &edx);
    uint8_t phys_bits = (uint8_t)(eax & 0xFF);

    if (phys_bits < 32 || phys_bits > 52) {
        return 36;
    }

    return phys_bits;
}

static inline uint8_t cpuid_get_maxvirtaddr(void) {
    uint32_t eax, ebx, ecx, edx;

    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);

    if (eax < 0x80000008) {
        return 48;
    }

    cpuid(0x80000008, &eax, &ebx, &ecx, &edx);
    uint8_t virt_bits = (uint8_t)((eax >> 8) & 0xFF);

    if (virt_bits < 48 || virt_bits > 57) {
        return 48;
    }

    return virt_bits;
}

#endif // ARCH_X86_64_CPUID_H
