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

/* Register an additional XCR0 component bit for inclusion in every
 * future XSAVE/XRSTOR sequence. Sets the XCR0 bit on this CPU, then
 * recomputes the per-component XSAVE area size via CPUID.0xD:0 and
 * updates g_xsave_mask + g_xsave_area_size atomically.
 *
 * Returns true on success. False when:
 *   - XSAVE is not supported
 *   - The bit is not declared supported in CPUID.0xD:0 (xcr0_supported)
 *   - Already registered
 *
 * Used by Phase 2H (PKRU bit 9), Phase 2K (CET_S bit 11 / CET_U bit 12)
 * to wire their per-thread state into the existing FPU context-switch
 * machinery without forking a parallel save/restore path.
 *
 * MUST be called BEFORE any process is spawned, because the FPU area
 * size in process_t is sized at allocation time from g_xsave_area_size. */
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
        return true;  /* already registered */
    }

    /* Set the XCR0 bit alongside the existing mask. */
    uint64_t new_mask = g_xsave_mask | xcr0_bit;
    xsetbv(0, new_mask);

    /* Recompute area size for the new component set. */
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

/* User FS base (TLS) context-switch gates — consumed by context_switch.asm.
 *
 * g_fsgsbase_active: CR4.FSGSBASE is enabled on every online core, so the
 *   switch path uses RDFSBASE/WRFSBASE (a few cycles). Set per-core in
 *   enable_fpu from the (progressively intersected) g_cpu_caps; any AP
 *   that loses the feature during cpu_intersect_features_ap clears it
 *   again in per_core_init_ap — before any user process exists, so the
 *   flag is final by first dispatch.
 *
 * g_user_fsbase_used: a process programmed its FS base via the kernel
 *   SET_FSBASE op on a pre-FSGSBASE CPU → switch path falls back to MSR
 *   0xC0000100. Stays 0 until TLS is actually used, keeping legacy
 *   configurations at zero per-switch cost. */
volatile uint8_t g_fsgsbase_active = 0;
volatile uint8_t g_user_fsbase_used = 0;

void enable_fpu(void) {
    uint64_t cr0, cr4;

    // Step 1: Configure CR0 — Intel SDM Vol 3A §2.5.
    //   EM (bit 2) = 0  : let SSE/x87 execute (don't emulate)
    //   MP (bit 1) = 1  : monitor coprocessor present
    //   WP (bit 16) = 1 : enforce supervisor write-protect on RO pages.
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);
    cr0 |=  (1ULL << 1);
    cr0 |=  (1ULL << 16);
    asm volatile("mov %0, %%cr0" :: "r"(cr0));

    // Step 2: CR4 bring-up — Intel SDM Vol 3A §2.5.
    asm volatile("mov %%cr4, %0" : "=r"(cr4));

    /* 5-level paging (LA57) interlock.
     *
     * The kernel now supports CR4.LA57 — vmm_init builds either a PML4
     * (4-level) or PML5 (5-level) top-level table based on g_cpu_caps.has_la57
     * and either inherits firmware's CR4.LA57=1 or performs a runtime
     * transition (see vmm_la57.asm + vmm_init's LA57 dance block) before
     * any user-mode fault can occur.
     *
     * On the BSP, this code runs BEFORE vmm_init builds the kernel context.
     * If firmware left CR4.LA57=1, that's fine — vmm_init detects the bit
     * and adopts 5-level natively. If firmware left CR4.LA57=0, vmm_init
     * will set it via the runtime dance after kernel mappings are ready.
     *
     * On APs, vmm_pku_ap_init / per_core_init_ap propagate CR4.LA57 via the
     * ap_trampoline's extra_cr4 mask before paging is enabled — by the time
     * enable_fpu runs on an AP, CR4.LA57 already matches g_vmm_la57_active.
     */

    cr4 |= (1ULL << 9);   // OSFXSR
    cr4 |= (1ULL << 10);  // OSXMMEXCPT

    if (g_cpu_caps.has_xsave) cr4 |= (1ULL << 18);

    if (g_cpu_caps.has_smep) cr4 |= (1ULL << 20);
    if (g_cpu_caps.has_smap) cr4 |= (1ULL << 21);

    /* UMIP (bit 11) — Intel SDM Vol 3A §2.5. */
    if (g_cpu_caps.has_umip) cr4 |= (1ULL << 11);

    /* FSGSBASE (bit 16) — Intel SDM Vol 3A §2.5. */
    if (g_cpu_caps.has_fsgsbase) cr4 |= (1ULL << 16);
    g_fsgsbase_active = g_cpu_caps.has_fsgsbase ? 1 : 0;

    cr4 |= (1ULL << 7);   // PGE

    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    debug_printf("[FPU] CR0.WP=on  CR4: SMEP=%s SMAP=%s UMIP=%s FSGSBASE=%s PGE=on\n",
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

        /* kprintf: this number sets the per-process FPU buffer, and once it
         * passes SLAB_LARGE_THRESHOLD every process allocates out of the
         * fixed kernel pool instead of the growable slab.  That crossing is
         * invisible unless the number itself is on the console. */
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

    // FCW at offset 0: 0x037F = all exceptions masked, 64-bit extended precision
    // (PC=11b, full 80-bit mantissa — NOT 53-bit double 0x027F), round-to-nearest.
    // Load-bearing for x87 long double: forcing 0x027F would silently halve precision.
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
