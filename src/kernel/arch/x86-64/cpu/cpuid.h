#ifndef ARCH_X86_64_CPUID_H
#define ARCH_X86_64_CPUID_H

#include "ktypes.h"

// CPU Capabilities Structure
typedef struct {
    bool has_waitpkg;           // UMONITOR/UMWAIT support (CPUID.7.0:ECX[5])
    bool has_invariant_tsc;     // Invariant TSC
    char vendor_string[13];     // CPU vendor (e.g., "GenuineIntel")
    uint32_t max_basic_leaf;    // Maximum CPUID basic leaf
    uint32_t max_extended_leaf; // Maximum CPUID extended leaf
} cpu_capabilities_t;

// Global CPU capabilities (initialized at boot)
extern cpu_capabilities_t g_cpu_caps;

// Functions
void cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx);
void cpuid_count(uint32_t leaf, uint32_t subleaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx);
void cpu_detect_features(void);

// Get MAXPHYADDR (maximum physical address bits)
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

// Get MAXVIRTADDR (maximum virtual address bits)
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
