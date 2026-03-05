#include "ktypes.h"
#include "fpu.h"
#include "klib.h"
#include "cpuid.h"

bool g_use_xsave = false;
uint32_t g_xsave_area_size = 0;
uint64_t g_xsave_mask = 0;

// XCR0 bit definitions
#define XCR0_X87        (1ULL << 0)  // x87 FPU state
#define XCR0_SSE        (1ULL << 1)  // SSE state (XMM registers + MXCSR)
#define XCR0_AVX        (1ULL << 2)  // AVX state (upper YMM halves)
#define XCR0_OPMASK     (1ULL << 5)  // AVX-512 opmask (k0-k7)
#define XCR0_ZMM_HI256  (1ULL << 6)  // AVX-512 upper ZMM halves (ZMM0-15)
#define XCR0_HI16_ZMM   (1ULL << 7)  // AVX-512 ZMM16-31

static inline void xsetbv(uint32_t index, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("xsetbv" : : "c"(index), "a"(lo), "d"(hi));
}

void enable_fpu(void) {
    uint64_t cr0, cr4;

    // Step 1: Enable native FPU (CR0)
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2); // EM = 0 (disable FPU emulation)
    cr0 |=  (1ULL << 1); // MP = 1
    asm volatile("mov %0, %%cr0" :: "r"(cr0));

    // Step 2: Enable OSFXSR + OSXMMEXCPT (CR4) — needed for SSE
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);   // OSFXSR — enable fxsave/fxrstor
    cr4 |= (1ULL << 10);  // OSXMMEXCPT — enable SSE exceptions

    // Step 3: If XSAVE supported, enable OSXSAVE (CR4 bit 18)
    if (g_cpu_caps.has_xsave) {
        cr4 |= (1ULL << 18); // OSXSAVE — enable xsave/xrstor and xsetbv/xgetbv
    }

    // Step 3b: Enable SMEP/SMAP if CPU supports them
    // SMEP (bit 20): prevents kernel from executing user-mode pages
    // SMAP (bit 21): prevents kernel from accessing user-mode pages (except via STAC/CLAC)
    if (g_cpu_caps.has_smep) {
        cr4 |= (1ULL << 20);
    }
    if (g_cpu_caps.has_smap) {
        cr4 |= (1ULL << 21);
    }

    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    if (g_cpu_caps.has_smep || g_cpu_caps.has_smap) {
        debug_printf("[FPU] Memory protection: SMEP=%s SMAP=%s\n",
                     g_cpu_caps.has_smep ? "enabled" : "not supported",
                     g_cpu_caps.has_smap ? "enabled" : "not supported");
    }

    // Step 4: Configure XCR0 if XSAVE available
    if (g_cpu_caps.has_xsave) {
        uint64_t xcr0 = XCR0_X87 | XCR0_SSE; // always enable x87 + SSE

        // Enable AVX if supported
        if (g_cpu_caps.has_avx && (g_cpu_caps.xcr0_supported & XCR0_AVX)) {
            xcr0 |= XCR0_AVX;
        }

        // Enable AVX-512 if all three components are supported
        if (g_cpu_caps.has_avx512 &&
            (g_cpu_caps.xcr0_supported & XCR0_OPMASK) &&
            (g_cpu_caps.xcr0_supported & XCR0_ZMM_HI256) &&
            (g_cpu_caps.xcr0_supported & XCR0_HI16_ZMM)) {
            xcr0 |= XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM;
        }

        xsetbv(0, xcr0);

        // Re-query CPUID.0xD:0 with the actual XCR0 to get the correct size
        // EBX now reflects the size for the currently enabled features
        uint32_t eax, ebx, ecx, edx;
        cpuid_count(0xD, 0, &eax, &ebx, &ecx, &edx);

        g_use_xsave = true;
        g_xsave_mask = xcr0;
        g_xsave_area_size = ebx; // size for currently enabled features

        debug_printf("[FPU] XSAVE enabled: XCR0=0x%lx, area_size=%u bytes\n",
                     xcr0, g_xsave_area_size);
        if (xcr0 & XCR0_AVX)
            debug_printf("[FPU]   AVX: enabled\n");
        if (xcr0 & XCR0_OPMASK)
            debug_printf("[FPU]   AVX-512: enabled\n");
    } else {
        g_use_xsave = false;
        g_xsave_area_size = 0;
        g_xsave_mask = 0;
        debug_printf("[FPU] FXSAVE mode (no XSAVE support)\n");
    }

    asm volatile("fninit");
}

void fpu_init_state(uint8_t* raw) {
    uint8_t* p = fpu_align(raw);
    uint32_t size = g_use_xsave ? g_xsave_area_size : 512;
    memset(p, 0, size);

    // FCW at offset 0: 0x037F = all exceptions masked, double precision, round-to-nearest
    p[0] = 0x7F;
    p[1] = 0x03;
    // MXCSR at offset 24: 0x1F80 = all SSE exceptions masked, round-to-nearest
    p[24] = 0x80;
    p[25] = 0x1F;

    if (g_use_xsave) {
        // XSAVE header at offset 512 (8 bytes XSTATE_BV, 8 bytes XCOMP_BV, 48 bytes reserved)
        // Set XSTATE_BV bits 0,1 (x87 + SSE) so xrstor loads our FCW/MXCSR values
        // Other components have XSTATE_BV=0, so xrstor will init them to defaults
        p[512] = 0x03; // XSTATE_BV = x87 | SSE
    }
}
