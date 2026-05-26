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

    // Step 1: Configure CR0 — Intel SDM Vol 3A §2.5.
    //   EM (bit 2) = 0  : let SSE/x87 execute (don't emulate)
    //   MP (bit 1) = 1  : monitor coprocessor present
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);
    cr0 |=  (1ULL << 1);
    asm volatile("mov %0, %%cr0" :: "r"(cr0));

    // Step 2: CR4 bring-up — Intel SDM Vol 3A §2.5.
    asm volatile("mov %%cr4, %0" : "=r"(cr4));

    /* 5-level paging (LA57) interlock. Intel SDM Vol 3A §4.5: if firmware
     * already set CR4.LA57=1, page-walks expect 5 levels and our 4-level
     * PML4 will #GP on first user-mode fault. We don't yet implement 5LP,
     * so refuse to keep going rather than triple-fault later. */
    if (cr4 & (1ULL << 12)) {
        panic("[FPU] CR4.LA57 set by firmware — 5-level paging not supported "
              "by this kernel. Disable LA57 in firmware/BIOS or run with 4-level.");
    }

    cr4 |= (1ULL << 9);   // OSFXSR
    cr4 |= (1ULL << 10);  // OSXMMEXCPT

    if (g_cpu_caps.has_xsave) cr4 |= (1ULL << 18);

    if (g_cpu_caps.has_smep) cr4 |= (1ULL << 20);
    if (g_cpu_caps.has_smap) cr4 |= (1ULL << 21);

    /* UMIP (bit 11) — Intel SDM Vol 3A §2.5. */
    if (g_cpu_caps.has_umip) cr4 |= (1ULL << 11);

    /* FSGSBASE (bit 16) — Intel SDM Vol 3A §2.5. */
    if (g_cpu_caps.has_fsgsbase) cr4 |= (1ULL << 16);

    cr4 |= (1ULL << 7);   // PGE

    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    debug_printf("[FPU] CR4: SMEP=%s SMAP=%s UMIP=%s FSGSBASE=%s PGE=on\n",
                 g_cpu_caps.has_smep     ? "on" : "n/a",
                 g_cpu_caps.has_smap     ? "on" : "n/a",
                 g_cpu_caps.has_umip     ? "on" : "n/a",
                 g_cpu_caps.has_fsgsbase ? "on" : "n/a");

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
