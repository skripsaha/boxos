#include "ktypes.h"
#include "fpu.h"
#include "klib.h"
#include "cpuid.h"
#include "uaccess.h"

bool g_use_xsave = false;
uint32_t g_xsave_area_size = 0;
uint64_t g_xsave_mask = 0;

#define XCR0_X87        (1ULL << 0)
#define XCR0_SSE        (1ULL << 1)
#define XCR0_AVX        (1ULL << 2)
#define XCR0_OPMASK     (1ULL << 5)
#define XCR0_ZMM_HI256  (1ULL << 6)
#define XCR0_HI16_ZMM   (1ULL << 7)

static inline void xsetbv(uint32_t index, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("xsetbv" : : "c"(index), "a"(lo), "d"(hi));
}

bool fpu_xsave_register_extension(uint64_t xcr0_bit, const char *name) {
    if (!g_use_xsave) {
        debug_printf("[FPU] xsave_register(%s): SKIP — XSAVE off\n",
                     name ? name : "?");
        return false;
    }
    if (!(g_cpu_caps.xcr0_supported & xcr0_bit)) {
        debug_printf("[FPU] xsave_register(%s): SKIP — XCR0 bit not supported\n",
                     name ? name : "?");
        return false;
    }
    if (g_xsave_mask & xcr0_bit) {
        return true;
    }

    uint64_t new_mask = g_xsave_mask | xcr0_bit;
    xsetbv(0, new_mask);

    uint32_t eax, ebx, ecx, edx;
    cpuid_count(0xD, 0, &eax, &ebx, &ecx, &edx);

    g_xsave_mask = new_mask;
    g_xsave_area_size = ebx;

    debug_printf("[FPU] xsave_register(%s): XCR0=0x%lx area_size=%u\n",
                 name ? name : "?",
                 (unsigned long)g_xsave_mask,
                 (unsigned)g_xsave_area_size);
    return true;
}

volatile uint8_t g_fsgsbase_active = 0;
volatile uint8_t g_user_fsbase_used = 0;

void enable_fpu(void) {
    uint64_t cr0, cr4;

    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);
    cr0 |=  (1ULL << 1);
    cr0 |=  (1ULL << 16);
    asm volatile("mov %0, %%cr0" :: "r"(cr0));

    asm volatile("mov %%cr4, %0" : "=r"(cr4));


    cr4 |= (1ULL << 9);
    cr4 |= (1ULL << 10);

    if (g_cpu_caps.has_xsave) cr4 |= (1ULL << 18);

    if (g_cpu_caps.has_smep) cr4 |= (1ULL << 20);
    if (g_cpu_caps.has_smap) cr4 |= (1ULL << 21);
    uaccess_set_smap(g_cpu_caps.has_smap);

    if (g_cpu_caps.has_umip) cr4 |= (1ULL << 11);

    if (g_cpu_caps.has_fsgsbase) cr4 |= (1ULL << 16);
    g_fsgsbase_active = g_cpu_caps.has_fsgsbase ? 1 : 0;

    cr4 |= (1ULL << 7);

    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    debug_printf("[FPU] CR0.WP=on  CR4: SMEP=%s SMAP=%s UMIP=%s FSGSBASE=%s PGE=on\n",
                 g_cpu_caps.has_smep     ? "on" : "n/a",
                 g_cpu_caps.has_smap     ? "on" : "n/a",
                 g_cpu_caps.has_umip     ? "on" : "n/a",
                 g_cpu_caps.has_fsgsbase ? "on" : "n/a");

    if (g_cpu_caps.has_xsave) {
        uint64_t xcr0 = XCR0_X87 | XCR0_SSE;

        if (g_cpu_caps.has_avx && (g_cpu_caps.xcr0_supported & XCR0_AVX)) {
            xcr0 |= XCR0_AVX;
        }

        if (g_cpu_caps.has_avx512 &&
            (g_cpu_caps.xcr0_supported & XCR0_OPMASK) &&
            (g_cpu_caps.xcr0_supported & XCR0_ZMM_HI256) &&
            (g_cpu_caps.xcr0_supported & XCR0_HI16_ZMM)) {
            xcr0 |= XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM;
        }

        xsetbv(0, xcr0);

        uint32_t eax, ebx, ecx, edx;
        cpuid_count(0xD, 0, &eax, &ebx, &ecx, &edx);

        g_use_xsave = true;
        g_xsave_mask = xcr0;
        g_xsave_area_size = ebx;

        static volatile uint8_t xsave_line_done = 0;
        if (__atomic_exchange_n(&xsave_line_done, 1, __ATOMIC_RELAXED) == 0)
            kprintf("[FPU] XSAVE enabled: XCR0=0x%lx, area_size=%u bytes (per-process buffer %u)\n",
                    xcr0, g_xsave_area_size, g_xsave_area_size + 63);
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

    p[0] = 0x7F;
    p[1] = 0x03;
    p[24] = 0x80;
    p[25] = 0x1F;

    if (g_use_xsave) {
        p[512] = 0x03;
    }
}