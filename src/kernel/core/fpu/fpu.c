#include "ktypes.h"
#include "fpu.h"
#include "klib.h"
#include "cpuid.h"
#include "uaccess.h"

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

/* The components enable_fpu decides for itself, once per core, from that
 * core's own capabilities. Everything OUTSIDE this set that is present in
 * g_xsave_mask got there through fpu_xsave_register_extension and belongs to
 * the subsystem that registered it — see the inheritance step in enable_fpu. */
#define XCR0_BASE_SET   (XCR0_X87 | XCR0_SSE | XCR0_AVX | \
                         XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM)

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
 * This is the XCR0 door, and only user state components come through it.
 * PKRU (bit 9) is one. CET's two components are NOT: CET_U (11) and CET_S
 * (12) are SUPERVISOR state components, enumerated in CPUID.(EAX=0DH,ECX=1)
 * and enabled through IA32_XSS, never through XCR0 (Intel SDM Vol 1 §13.5).
 * CPUID.0DH:0 never reports them, so a caller asking for bit 11 or 12 is
 * refused here by the xcr0_supported test below — see the note in
 * cet_lifecycle.c.
 *
 * MUST be called BEFORE any process is spawned, because the FPU area
 * size in process_t is sized at allocation time from g_xsave_area_size. */
bool fpu_xsave_register_extension(uint64_t xcr0_bit, const char *name) {
    /* Both refusals speak, and they speak with kprintf. A registration that
     * does not happen is exactly the failure this door exists to expose: the
     * component stays out of g_xsave_mask, so no context switch saves or
     * restores it and every process inherits the previous one's copy of that
     * register. The caller does not print either — vmm_pku_init drops the
     * result — so silence here is indistinguishable from success. */
    if (!g_use_xsave) {
        kprintf("[FPU] xsave_register(%s): REFUSED — this processor has no "
                "XSAVE, so the component is never saved or restored and each "
                "process inherits the previous one's register\n",
                name ? name : "?");
        return false;
    }
    if (!(g_cpu_caps.xcr0_supported & xcr0_bit)) {
        kprintf("[FPU] xsave_register(%s): REFUSED — CPUID.(EAX=0DH,ECX=0) "
                "does not enumerate XCR0 component 0x%lx, so the component is "
                "never saved or restored and each process inherits the "
                "previous one's register\n",
                name ? name : "?", (unsigned long)xcr0_bit);
        return false;
    }
    if (__atomic_load_n(&g_xsave_mask, __ATOMIC_RELAXED) & xcr0_bit) {
        return true;  /* already registered */
    }

    /* Set the XCR0 bit alongside the existing mask. */
    uint64_t new_mask = __atomic_load_n(&g_xsave_mask, __ATOMIC_RELAXED) |
                        xcr0_bit;
    xsetbv(0, new_mask);

    /* Recompute area size for the new component set. */
    uint32_t eax, ebx, ecx, edx;
    cpuid_count(0xD, 0, &eax, &ebx, &ecx, &edx);

    __atomic_store_n(&g_xsave_mask, new_mask, __ATOMIC_RELAXED);
    __atomic_store_n(&g_xsave_area_size, ebx, __ATOMIC_RELAXED);

    /* kprintf, and it costs one line for the whole boot: only the core that
     * registers a component first gets this far. Every later core finds the
     * bit already in g_xsave_mask and returns above — enable_fpu inherits
     * registered components into that core's XCR0, so there is nothing left
     * for the AP to do here.
     *
     * It has to be visible on a board because this is the LAST word on the
     * per-process FPU buffer. enable_fpu prints the area size it computed,
     * but that line is printed before any extension is registered, so the
     * number an operator reads there is not the one process_create will use
     * — and the crossing of SLAB_LARGE_THRESHOLD that the note in enable_fpu
     * describes is decided by the number printed HERE. */
    kprintf("[FPU] xsave_register(%s): XCR0=0x%lx area_size=%u "
            "(per-process buffer %u)\n",
            name ? name : "?",
            (unsigned long)new_mask,
            (unsigned)ebx,
            (unsigned)ebx + 63u);
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
    /* And the other half of the same fact: whether STAC/CLAC may be issued at
     * all. Without SMAP those instructions are #UD, not no-ops — see the note
     * at the top of uaccess.h and the Braswell panic it came from. */
    uaccess_set_smap(g_cpu_caps.has_smap);

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
        /* THIS core's XSAVE enumeration, read before XCR0 is touched.
         * CPUID.(EAX=0DH,ECX=0) EDX:EAX is the set of state components this
         * processor will accept in XCR0, and XSETBV raises #GP(0) for a bit
         * outside it (Intel SDM Vol 1 §13.3).
         *
         * g_cpu_caps.xcr0_supported is NOT that set on an AP:
         * cpu_intersect_features_ap deliberately leaves the field at the
         * BSP's reading, calling it a descriptor of the current core rather
         * than a kernel-wide capability. Every per-core decision below that
         * has no intersected boolean of its own has to use this word
         * instead — an XSETBV that faults here used to happen before the AP had any IDT at all, and a #GP with no gate is a triple fault and a core that never reports in. That is no longer the shape of it: per_core_init_ap loads the kernel IDT before it calls this, and the trampoline carries a Handrail before that, so such a fault is now caught and named. The reason for asking THIS core rather than trusting g_cpu_caps is unchanged and is the real one — the bit may not be set on this core at all on a hybrid part, and a fault we can name is still a core that does not come up. Kept for that, not for the triple fault it no longer causes. Original note: an XSETBV that faults here happens before idt_load:
         * triple fault, and a core that never reports in. */
        uint32_t sup_eax, sup_ebx, sup_ecx, sup_edx;
        cpuid_count(0xD, 0, &sup_eax, &sup_ebx, &sup_ecx, &sup_edx);
        const uint64_t xcr0_this_core =
            ((uint64_t)sup_edx << 32) | (uint64_t)sup_eax;

        uint64_t xcr0 = XCR0_X87 | XCR0_SSE; // always enable x87 + SSE

        // Enable AVX if supported
        if (g_cpu_caps.has_avx && (g_cpu_caps.xcr0_supported & XCR0_AVX)) {
            xcr0 |= XCR0_AVX;
        }

        /* AVX-512 needs all three of its own components AND the AVX
         * component underneath them. Intel SDM Vol 1 §13.3: XSETBV raises
         * #GP(0) "if ECX = 0, EAX[7:5] is not 000b, and any bit is clear in
         * EAX[2:1] or EAX[7:5]" — asking for opmask / ZMM_Hi256 / Hi16_ZMM
         * while XCR0[2] stays clear is not a weaker configuration, it is a
         * fault.
         *
         * So the gate is `xcr0 & XCR0_AVX` — what this XSETBV will actually
         * carry — and not has_avx512 alone. A core that keeps
         * CPUID.07H.0:EBX[16] while losing CPUID.01H:ECX[28] (CPUID masking,
         * a firmware-masked or heterogeneous AP whose has_avx was cleared by
         * cpu_intersect_features_ap) would otherwise #GP right here. On an AP
         * that happens before its IDT is any use: triple fault, and a core
         * that never reports in. */
        if (g_cpu_caps.has_avx512 &&
            (xcr0 & XCR0_AVX) &&
            (g_cpu_caps.xcr0_supported & XCR0_OPMASK) &&
            (g_cpu_caps.xcr0_supported & XCR0_ZMM_HI256) &&
            (g_cpu_caps.xcr0_supported & XCR0_HI16_ZMM)) {
            xcr0 |= XCR0_OPMASK | XCR0_ZMM_HI256 | XCR0_HI16_ZMM;
        }

        /* Carry over every component another subsystem has already
         * registered through fpu_xsave_register_extension.
         *
         * g_xsave_mask is one word read by every core — fpu_save,
         * fpu_restore and the SAVE/RESTORE macros in context_switch.asm
         * load it on every switch. Bit 9 (PKRU) is not this function's to
         * decide; it arrives through that door, on whichever core called
         * it. Until this step existed an AP's XCR0 never carried it, so the
         * AP went on to publish a kernel-wide mask one component short of
         * what the BSP had arranged. vmm_pku_ap_init put the bit back into
         * this AP's XCR0 some thirty lines later in per_core_init_ap, and
         * on the happy path that closed the gap before anything looked.
         *
         * It does not close for an AP that comes up, runs this function,
         * and then stops or falls over before reaching vmm_pku_ap_init:
         * amp_boot_aps gives a core a deadline to report in and carries on
         * without one that misses it, and nothing rewrites the word
         * afterwards. The kernel then runs for the rest of its life with
         * PKRU out of the mask while CR4.PKE stays set on the cores that
         * did reach it — every context switch from then on leaves PKRU
         * unsaved and unrestored, and each process inherits the
         * protection-key register of the one before it.
         *
         * Only registered components are inherited. The base set stays a
         * per-core decision on purpose: a core that lost AVX in
         * cpu_intersect_features_ap must not be handed it back here.
         *
         * The gate is xcr0_this_core and not the kernel-wide
         * g_cpu_caps.xcr0_supported the AVX gates above lean on. Those gates
         * carry an intersected per-core boolean as well; this one has none,
         * so the BSP's enumeration on its own would be strictly weaker —
         * and a component the BSP enumerates while this core does not is
         * precisely the XSETBV #GP that read exists to prevent. No
         * dependency rule can be broken by this OR either: the base set is
         * subtracted explicitly, and what comes through the extension door
         * is an independent component. PKRU has no prerequisite of its own
         * — in particular not CR4.PKE, which this AP has not set yet
         * (vmm_pku_ap_init does that later): CR4.PKE gates RDPKRU/WRPKRU,
         * XCR0[9] gates only whether XSAVE manages the register. */
        xcr0 |= __atomic_load_n(&g_xsave_mask, __ATOMIC_RELAXED) &
                ~(uint64_t)XCR0_BASE_SET & xcr0_this_core;

        xsetbv(0, xcr0);

        // Re-query CPUID.0xD:0 with the actual XCR0 to get the correct size
        // EBX now reflects the size for the currently enabled features
        uint32_t eax, ebx, ecx, edx;
        cpuid_count(0xD, 0, &eax, &ebx, &ecx, &edx);

        g_use_xsave = true;

        /* Publish. The kernel-wide mask is the INTERSECTION of what the
         * cores enabled, never the opinion of whichever core ran last.
         *
         * It has to be a subset of every core's XCR0. XSAVE and XRSTOR use
         * RFBM = EDX:EAX & XCR0, so a mask wider than some core's XCR0 is a
         * component silently dropped on that core — and worse, XRSTOR
         * raises #GP(0) when the saved XSTATE_BV names a component that
         * core's XCR0 lacks (Intel SDM Vol 1 §13.8). A narrow core
         * publishing and a wide core publishing after it used to arrange
         * exactly that fault. An intersection cannot produce either, and it
         * does not depend on the order the cores came up in.
         *
         * The area size belongs to the FIRST publisher and is never touched
         * again here. It was computed for that core's whole XCR0, the mask
         * only ever narrows from there, and component offsets in the
         * standard XSAVE layout are architectural — so that size still
         * covers every later, smaller set. Raising it afterwards would be a
         * buffer overflow rather than a fix: process FPU areas are sized
         * from this word when they are allocated, and idle_process_init has
         * already run by the time any AP gets here. Growing it belongs to
         * fpu_xsave_register_extension alone, which runs on the BSP before
         * any process exists. */
        static volatile uint8_t xsave_published = 0;
        if (__atomic_exchange_n(&xsave_published, 1, __ATOMIC_RELAXED) == 0) {
            __atomic_store_n(&g_xsave_mask, xcr0, __ATOMIC_RELAXED);
            __atomic_store_n(&g_xsave_area_size, ebx, __ATOMIC_RELAXED);

            /* kprintf, and the same core that publishes the number is the
             * one that says it: this number sets the per-process FPU
             * buffer, and once it passes SLAB_LARGE_THRESHOLD every process
             * allocates out of the fixed kernel pool instead of the
             * growable slab. That crossing is invisible unless the number
             * itself is on the console.
             *
             * It is the BASE size — no extension is registered this early
             * in boot. The final number, the one process_create actually
             * uses, is printed by fpu_xsave_register_extension at the
             * moment it changes. A later core has nothing to add here; if
             * it has anything to say at all it says it below. */
            kprintf("[FPU] XSAVE enabled: XCR0=0x%lx, area_size=%u bytes (per-process buffer %u)\n",
                    xcr0, ebx, ebx + 63);
        } else {
            uint64_t before = __atomic_fetch_and(&g_xsave_mask, xcr0,
                                                 __ATOMIC_RELAXED);
            uint64_t lost = before & ~xcr0;
            if (lost) {
                /* Heterogeneous silicon, and it is not a detail to swallow.
                 * The cores that came up before this one still have these
                 * components enabled in their own XCR0 — user code there
                 * may still execute the instructions — while the kernel has
                 * just stopped carrying that state across a switch.
                 * Narrowing is the only mask that is safe everywhere, and
                 * from this core the older ones cannot be re-programmed, so
                 * naming it on the console is the rest of the answer. */
                kprintf("[FPU] XCR0 components 0x%lx are absent on this core "
                        "and have left the kernel-wide save set (0x%lx -> "
                        "0x%lx) — the cores already online keep them enabled "
                        "and their state is no longer carried across a "
                        "context switch\n",
                        (unsigned long)lost, (unsigned long)before,
                        (unsigned long)(before & xcr0));
            }
        }

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
