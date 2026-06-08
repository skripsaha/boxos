#include "cpuid.h"
#include "klib.h"

cpu_capabilities_t g_cpu_caps;

/* MSR addresses used by cpu_detect_features and cpu_test_ctl_init.
 *   IA32_BIOS_SIGN_ID  — Intel SDM Vol 3A §9.11.7.1 / Vol 4 Table 2-2.
 *   IA32_CORE_CAPABILITIES — Intel SDM Vol 4 Table 2-2 (MSR 0xCF).
 *   TEST_CTL            — Intel SDM Vol 4 Table 2-2 (MSR 0x33; bit 29
 *                         enables #AC on split-locked LOCK access). */
#define MSR_IA32_BIOS_SIGN_ID       0x0000008Bu
#define MSR_IA32_CORE_CAPABILITIES  0x000000CFu
#define MSR_TEST_CTL                0x00000033u
#define TEST_CTL_SPLIT_LOCK_AC_BIT  29u

static inline uint64_t cpu_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpu_wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : :
                     "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

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
        // Hypervisor present (CPUID.1:ECX[31]) — industry convention; Intel SDM
        // Vol 2A "CPUID — Hypervisor present". Any hypervisor sets it; bare
        // silicon leaves it 0. Used by hypervisor_detect() to decide whether
        // to probe leaf 0x40000000.
        g_cpu_caps.has_hypervisor = (ecx & (1u << 31)) != 0;
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
        // EBX[1]  TSC_ADJUST — Intel SDM Vol 3A §17.17.3 (IA32_TSC_ADJUST MSR).
        // Required for per-AP TSC sync; without it, AP TSC skew vs. BSP can
        // only be detected (not corrected) on BIOSes that leave the TSC offset
        // unsynchronised across sockets.
        g_cpu_caps.has_tsc_adjust = (ebx & (1 << 1)) != 0;
        // EBX[9]  ERMS  — Intel SDM Vol 1 §7.3.9.4 "Enhanced REP MOVSB and
        //                 STOSB Operation". Microarchitecturally accelerates
        //                 rep movsb / rep stosb to full memory bandwidth.
        // EDX[4]  FSRM  — Intel SDM Vol 1: Fast Short REP MOV. Pushes the
        //                 ERMS efficiency down to very small n on Ice Lake+.
        g_cpu_caps.has_erms = (ebx & (1 << 9))  != 0;
        g_cpu_caps.has_fsrm = (edx & (1 << 4))  != 0;
        /* CPUID.7.0:EDX[30] — IA32_CORE_CAPABILITIES MSR exists.
         * Intel SDM Vol 4 Table 2-2 (CORE_CAPABILITIES). When set, the
         * kernel may read MSR 0xCF; bit 5 of that MSR gates whether the
         * CPU can raise #AC on split-lock LOCK ops. The MSR read itself
         * is gated on this CPUID bit because reading 0xCF on a CPU that
         * doesn't claim CORE_CAPABILITIES would #GP. */
        g_cpu_caps.has_core_capabilities = (edx & (1u << 30)) != 0;
        /* Phase 2H — Protection Keys (Intel SDM Vol 3A §4.6.2 / §4.6.3).
         * PKU = userspace 16 keys via IA32_PKRU. PKS = supervisor variant. */
        g_cpu_caps.has_pku  = (ecx & (1u << 3))  != 0;
        g_cpu_caps.has_pks  = (ecx & (1u << 31)) != 0;
        g_cpu_caps.has_tme   = (ecx & (1u << 13)) != 0;
        g_cpu_caps.has_shstk = (ecx & (1u << 7))  != 0;
        g_cpu_caps.has_ibt   = (edx & (1u << 20)) != 0;
        /* PCONFIG instruction presence — CPUID.07H.0:EDX[18]. Intel SDM
         * Vol 2D PCONFIG: "#UD if CPUID.07H.0:EDX[18] = 0". TME and
         * PCONFIG are co-introduced on Ice Lake-SP / Sapphire Rapids,
         * but the SDM lets them enumerate independently. We gate
         * tme_init_bsp on BOTH bits so a hypothetical TME-yes /
         * PCONFIG-no CPU doesn't kernel-panic on the first MKTME key
         * program. */
        g_cpu_caps.has_pconfig = (edx & (1u << 18)) != 0;
        /* Phase 2I — Linear Address Masking lives in CPUID.7.1:EAX[26]
         * (subleaf 1, distinct from the canonical subleaf 0 above).
         * Probe only if subleaf 1 is reachable per CPUID.07H.0:EAX
         * which reports the maximum sub-leaf index. */
        if (eax >= 1) {
            uint32_t lam_eax, lam_ebx, lam_ecx, lam_edx;
            cpuid_count(CPUID_LEAF_EXT_FEATURES, 1,
                        &lam_eax, &lam_ebx, &lam_ecx, &lam_edx);
            g_cpu_caps.has_lam = (lam_eax & (1u << 26)) != 0;
        }
    }

    /* ─── TDX-host detection — CPUID.21H ───────────────────────────
     * Intel Trust Domain Extensions present a vendor string via
     * CPUID.21H subleaf 0:
     *   EBX:EDX:ECX = "IntelTDX " (12 bytes, little-endian per-reg)
     * A non-TDX CPU returns 0 for these registers. CPUID.0H:EAX
     * exposes the max basic leaf; CPUID.21H is reachable iff
     * max_basic_leaf >= 0x21.
     *
     * Detecting TDX gates the IA32_MKTME_KEYID_PARTITIONING (MSR
     * 0x87) read in tme_init_bsp — that MSR is TDX-architectural
     * and #GP's on non-TDX silicon, so the gate is REQUIRED for
     * real-HW safety. */
    g_cpu_caps.has_tdx = false;
    if (g_cpu_caps.max_basic_leaf >= 0x21) {
        cpuid_count(0x21, 0, &eax, &ebx, &ecx, &edx);
        /* "IntelTDX    " (12 bytes — 8 ASCII + 4 spaces) split EBX:EDX:ECX
         * per Intel TDX Module Base Architecture Specification §3.5
         * "CPUID Vendor String" + cross-verified against Linux source
         * (arch/x86/coco/tdx/tdx.c #define TDX_IDENT "IntelTDX    ").
         *
         * Layout in memory: bytes 0..11 = 'I','n','t','e','l','T','D','X',
         *                              ' ',' ',' ',' '
         * EBX (LE uint32 of bytes 0..3 = 'I','n','t','e') = 0x65746E49
         * EDX (LE uint32 of bytes 4..7 = 'l','T','D','X') = 0x5844546C
         * ECX (LE uint32 of bytes 8..11 = ' '×4)          = 0x20202020 */
        if (ebx == 0x65746E49u && edx == 0x5844546Cu && ecx == 0x20202020u) {
            g_cpu_caps.has_tdx = true;
        }
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

    /* ARAT (Always Running APIC Timer) — CPUID.06H:EAX[2]. Intel SDM
     * Vol 3A §10.5.4.1: "The local APIC timer functions independently
     * of the processor's power management state and continues to run
     * even when the processor enters a low-power state". Without
     * ARAT, MWAIT C3+ or deep HLT halt the LAPIC's internal timer
     * counter — the next scheduled tick simply never fires. */
    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_THERMAL_PM) {
        cpuid(CPUID_LEAF_THERMAL_PM, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_arat = (eax & (1u << 2)) != 0;
    }

    /* MONITOR/UMONITOR cacheline granularity — CPUID.05H.
     *   EAX[15:0] = smallest monitor line size in bytes
     *   EBX[15:0] = largest  monitor line size in bytes
     * Intel SDM Vol 2A "CPUID — CPU Identification" leaf 5 + UMONITOR
     * "The address range determined by the CPUID monitor leaf function".
     *
     * Real-HW data points:
     *   - x86 desktop/server (Sandy Bridge..Sapphire Rapids): 64 B
     *   - Intel Atom Tremont / Goldmont: 32 B
     *   - Some Xeon Scalable with adjacent-line prefetcher fused: 128 B
     *
     * Leaf 5 unimplemented (max_basic_leaf < 5) is rare on UMONITOR-capable
     * silicon — WAITPKG implies leaf 5 in practice. We default min=max=64
     * so downstream consumers (Brook BrookHeader sizing, Touch ring
     * padding) keep working on the absent-leaf case. */
    g_cpu_caps.monitor_line_min = 64;
    g_cpu_caps.monitor_line_max = 64;
    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_MONITOR) {
        cpuid(CPUID_LEAF_MONITOR, &eax, &ebx, &ecx, &edx);
        uint16_t min_line = (uint16_t)(eax & 0xFFFFu);
        uint16_t max_line = (uint16_t)(ebx & 0xFFFFu);
        if (min_line == 0) min_line = 64;
        if (max_line == 0) max_line = 64;
        g_cpu_caps.monitor_line_min = min_line;
        g_cpu_caps.monitor_line_max = max_line;
    }

    /* IA32_CORE_CAPABILITIES — Intel SDM Vol 4 Table 2-2 (MSR 0xCF).
     * Bit 5 = SPLIT_LOCK_DETECT_SUPPORTED: the CPU can raise #AC on a
     * cache-line-spanning LOCK access when TEST_CTL (MSR 0x33) bit 29
     * is set. We only read 0xCF if CPUID.7.0:EDX[30] said the MSR is
     * implemented, otherwise the read would #GP on Sandy Bridge era
     * silicon. Used by cpu_test_ctl_init to gate the bit-29 program. */
    g_cpu_caps.has_split_lock_detect = false;
    if (g_cpu_caps.has_core_capabilities) {
        uint64_t core_cap = cpu_rdmsr(MSR_IA32_CORE_CAPABILITIES);
        g_cpu_caps.has_split_lock_detect = (core_cap & (1ULL << 5)) != 0;
    }
}

/* IA32_UMWAIT_CONTROL — defined and called below, after cpu_wrmsr is
 * visible. The body lives next to the other MSR helpers further down
 * this file. */

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
/* IA32_UMWAIT_CONTROL — Intel SDM Vol 4 §2.5.1 (MSR 0xE1).
 *   bit 0       : C0.2 disable (1 = OS forbids C0.2, all requests revert
 *                  to C0.1; we leave 0 = both states allowed).
 *   bits 31:2   : OS-imposed maximum residency in TSC-quanta. 0 means
 *                  no OS cap (CPU enforces its own internal default).
 *
 * Hardware reset value: 0 — but Intel reference BIOS sets a small cap
 * (~100,000 TSC ticks ≈ 33 µs @ 3 GHz) before handing off to the OS.
 * That makes the user-facing UMWAIT deadline (e.g. Brook's 50 ms)
 * meaningless: the CPU returns CF=1 every ~33 µs and the wait loop
 * re-enters thousands of times per ms — both extra power draw and
 * extra instruction issue.
 *
 * BoxOS policy: program bits 31:2 to ~2 ms worth of TSC quanta. The
 * cap is a safety net against silicon quirks where a monitor wake
 * event is missed (microcode/cache-coherence pathology), so the loop
 * top still re-polls within bounded time. With a 2 ms cap we burn
 * ~500 UMWAIT enter/exit cycles per second of idle waiting — under
 * 1 % of one core, vs the default's tens of thousands. Producer/
 * consumer cursor wakes still arrive immediately via the monitored
 * cacheline; the cap is the safety net, not the wake mechanism.
 *
 * Why not 0 (no cap)? On adversarial silicon a missed monitor wake
 * could park us indefinitely; a bounded cap costs almost nothing and
 * guarantees forward progress.
 *
 * Why 2 ms and not larger? Matches the scheduler tick
 * (CONFIG_SCHED_DEFAULT_TICK_HZ ~ 500 Hz). Streaming primitives
 * (Brook) want sub-scheduler-tick wake granularity in the pathological
 * miss case. */
#define MSR_IA32_UMWAIT_CONTROL    0x000000E1
#define UMWAIT_CONTROL_CAP_MS      2u

static uint64_t umwait_control_compose(uint64_t tsc_freq_khz) {
    /* MSR field is bits[31:2] = max TSC quanta; bits[1:0] are reserved
     * for the C0.2-disable flag (bit 0) and one reserved bit (bit 1).
     * Mask the cap to ~3 so the encoded value lands on the spec layout.
     *
     * tsc_freq_khz × cap_ms = ticks per cap_ms; clamp to 32-bit field. */
    uint64_t cap_ticks = tsc_freq_khz * (uint64_t)UMWAIT_CONTROL_CAP_MS;
    if (cap_ticks > 0xFFFFFFFCULL) cap_ticks = 0xFFFFFFFCULL;
    return cap_ticks & ~3ULL;
}

void cpu_umwait_control_init(uint64_t tsc_freq_khz) {
    /* Gated on WAITPKG — non-WAITPKG CPUs ignore MSR 0xE1 (it's
     * architectural to UMWAIT/TPAUSE only). Writing on AMD or
     * pre-Tremont Intel would #GP. */
    if (!g_cpu_caps.has_waitpkg) return;
    if (tsc_freq_khz == 0)       return;   /* calibration not done yet */

    uint64_t v = umwait_control_compose(tsc_freq_khz);
    cpu_wrmsr(MSR_IA32_UMWAIT_CONTROL, v);
}

void cpu_test_ctl_init(void) {
    /* Gated on CPUID-reported capability: writing TEST_CTL on a CPU
     * that doesn't enumerate IA32_CORE_CAPABILITIES would #GP on
     * pre-Tremont Intel and on AMD/Hygon. The has_split_lock_detect
     * field already encapsulates "CPUID.7.0:EDX[30] set AND MSR 0xCF
     * bit 5 set"; if false we have nothing to program here. */
    if (!g_cpu_caps.has_split_lock_detect) return;

    /* Read-modify-write so we only flip bit 29 — other TEST_CTL bits
     * (notably bit 31, the SDM-reserved bus-lock detect on
     * Sapphire Rapids+ via secondary MSR 0x1B4, but historically also
     * occupying TEST_CTL on some part) are preserved. Intel SDM Vol 4
     * Table 2-2 states unused TEST_CTL bits must read back unmodified;
     * a blanket cpu_wrmsr(MSR_TEST_CTL, 0) is therefore unsafe. */
    uint64_t v = cpu_rdmsr(MSR_TEST_CTL);
    v &= ~(1ULL << TEST_CTL_SPLIT_LOCK_AC_BIT);
    cpu_wrmsr(MSR_TEST_CTL, v);
}

uint32_t cpu_microcode_revision(void) {
    /* Vendor-specific MSR 0x8B semantics — do NOT use one sequence for both:
     *
     *   Intel (SDM Vol 3A §9.11.7.1): the OS must (a) write 0 to MSR 0x8B,
     *     (b) execute CPUID(EAX=1), and (c) read MSR 0x8B; the CPU populates
     *     bits [63:32] with the running microcode revision as a side effect
     *     of CPUID(1). Bits [31:0] are reserved. Skipping the pre-clear
     *     can return a stale value the BIOS left behind.
     *
     *   AMD (APM Vol 2 / MSR PatchLevel C001_0020, aliased at 0x8B on many
     *     parts): MSR holds the running PatchLevel in bits [31:0] at all
     *     times. CPUID has NO side effect on it. Writing 0 from the OS is
     *     destructive on parts that treat 0x8B as writable — it silently
     *     loses the BIOS-loaded patch revision and the next read returns 0.
     *
     * Branch on vendor: pre-clear-then-CPUID is Intel-only; AMD/Hygon/VIA
     * just read directly. */
    uint32_t a, b, c, d;
    cpuid(CPUID_LEAF_VENDOR, &a, &b, &c, &d);
    bool is_intel = (b == 0x756E6547u /* "Genu" */ &&
                     d == 0x49656E69u /* "ineI" */ &&
                     c == 0x6C65746Eu /* "ntel" */);

    if (is_intel) {
        cpu_wrmsr(MSR_IA32_BIOS_SIGN_ID, 0);
        cpuid(CPUID_LEAF_FEATURES, &a, &b, &c, &d);
        return (uint32_t)(cpu_rdmsr(MSR_IA32_BIOS_SIGN_ID) >> 32);
    }

    /* AMD/Hygon/VIA/Zhaoxin: low 32 bits hold the PatchLevel directly. */
    return (uint32_t)(cpu_rdmsr(MSR_IA32_BIOS_SIGN_ID) & 0xFFFFFFFFu);
}

void cpu_read_identity(cpu_identity_t* out) {
    if (!out) return;

    uint32_t a, b, c, d;

    /* Vendor (CPUID 0). EBX-EDX-ECX order per Intel SDM Vol 2A. */
    cpuid(CPUID_LEAF_VENDOR, &a, &b, &c, &d);
    *((uint32_t*)&out->vendor[0]) = b;
    *((uint32_t*)&out->vendor[4]) = d;
    *((uint32_t*)&out->vendor[8]) = c;
    out->vendor[12] = '\0';

    /* CPUID(1).EAX layout:
     *   3:0   stepping
     *   7:4   base_model
     *   11:8  base_family
     *   13:12 type   (Intel; reserved/0 on AMD)
     *   19:16 ext_model
     *   27:20 ext_family
     * Apply the Intel/AMD effective-family/model rules — see header. */
    cpuid(CPUID_LEAF_FEATURES, &a, &b, &c, &d);
    uint32_t base_family = (a >> 8)  & 0xF;
    uint32_t base_model  = (a >> 4)  & 0xF;
    uint32_t ext_family  = (a >> 20) & 0xFF;
    uint32_t ext_model   = (a >> 16) & 0xF;

    out->stepping = a & 0xF;
    out->type     = (a >> 12) & 0x3;
    out->family   = base_family + (base_family == 0x0F ? ext_family : 0);
    out->model    = base_model  | ((base_family == 0x06 || base_family == 0x0F)
                                    ? (ext_model << 4) : 0);
    out->apic_id  = (b >> 24) & 0xFF;

    /* Microcode revision LAST — cpu_microcode_revision itself runs
     * CPUID(1) as part of its sequence, so we don't double-cost it
     * here (the side-effect populates the MSR which we then read). */
    out->microcode_rev = cpu_microcode_revision();
}

void cpu_log_identity(const char* prefix) {
    cpu_identity_t id;
    cpu_read_identity(&id);

    /* Microcode revision == 0 means either "no patch loaded" (factory
     * silicon) or the BIOS did not run the update. Log it as "none"
     * for operator clarity rather than printing literal 0. */
    if (id.microcode_rev) {
        kprintf("[CPU] %s%svendor=%s family=0x%x model=0x%x stepping=%u "
                "apic=%u microcode=0x%08x\n",
                prefix ? prefix : "", prefix ? " " : "",
                id.vendor, id.family, id.model, id.stepping,
                id.apic_id, id.microcode_rev);
    } else {
        kprintf("[CPU] %s%svendor=%s family=0x%x model=0x%x stepping=%u "
                "apic=%u microcode=none\n",
                prefix ? prefix : "", prefix ? " " : "",
                id.vendor, id.family, id.model, id.stepping, id.apic_id);
    }
}

void cpu_intersect_features_ap(void) {
    uint32_t eax, ebx, ecx, edx;

    /* Snapshot the BSP's pre-intersect view of the booleans we care
     * about so we can report which (if any) flipped to 0 on this AP.
     * Reading after the AND would always show the post-intersect value
     * and miss the moment the bit was lost. */
    bool bsp_x2apic       = g_cpu_caps.has_x2apic;
    bool bsp_tsc_deadline = g_cpu_caps.has_tsc_deadline;
    bool bsp_monitor      = g_cpu_caps.has_monitor;
    bool bsp_xsave        = g_cpu_caps.has_xsave;
    bool bsp_avx          = g_cpu_caps.has_avx;
    bool bsp_pcid         = g_cpu_caps.has_pcid;
    bool bsp_pat          = g_cpu_caps.has_pat;
    bool bsp_avx512       = g_cpu_caps.has_avx512;
    bool bsp_fsgsbase     = g_cpu_caps.has_fsgsbase;
    bool bsp_smep         = g_cpu_caps.has_smep;
    bool bsp_smap         = g_cpu_caps.has_smap;
    bool bsp_umip         = g_cpu_caps.has_umip;
    bool bsp_invpcid      = g_cpu_caps.has_invpcid;
    bool bsp_tsc_adjust   = g_cpu_caps.has_tsc_adjust;
    bool bsp_erms         = g_cpu_caps.has_erms;
    bool bsp_fsrm         = g_cpu_caps.has_fsrm;
    bool bsp_inv_tsc      = g_cpu_caps.has_invariant_tsc;
    bool bsp_arat         = g_cpu_caps.has_arat;
    bool bsp_core_caps    = g_cpu_caps.has_core_capabilities;
    bool bsp_split_lock   = g_cpu_caps.has_split_lock_detect;

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
        /* has_hypervisor is package-wide — the host either runs us
         * virtualized or doesn't. We intersect (AND) defensively in case
         * one AP somehow ends up directly on bare silicon (impossible
         * under any sane hypervisor scheduler, but cheap). */
        g_cpu_caps.has_hypervisor &= ((ecx & (1u << 31)) != 0);
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
        /* TSC_ADJUST is a per-logical-processor MSR; the architectural
         * capability bit (CPUID.7.0:EBX[1]) is uniform across the
         * package on every real CPU. Intersect for safety. */
        g_cpu_caps.has_tsc_adjust &= ((ebx & (1 << 1))  != 0);
        /* ERMS/FSRM — uniform across a homogeneous package; on Alder
         * Lake-class hybrid CPUs both classes implement ERMS, so the
         * intersection is the BSP value in practice. */
        g_cpu_caps.has_erms       &= ((ebx & (1 << 9))  != 0);
        g_cpu_caps.has_fsrm       &= ((edx & (1 << 4))  != 0);
        /* CORE_CAPABILITIES is package-architectural on shipping silicon,
         * but the SDM doesn't forbid heterogeneous packages with mixed
         * support — intersect defensively so a missing AP bit forces
         * the kernel-wide split-lock policy off. */
        g_cpu_caps.has_core_capabilities &= ((edx & (1u << 30)) != 0);
        /* Phase 2H — Protection Keys; intersect across every AP so a
         * hybrid SKU with PKU on P-cores and absent on E-cores forces
         * PKE off package-wide rather than #GP an E-core that runs a
         * PKU-aware codepath. */
        g_cpu_caps.has_pku &= ((ecx & (1u << 3))  != 0);
        g_cpu_caps.has_pks &= ((ecx & (1u << 31)) != 0);
        g_cpu_caps.has_tme   &= ((ecx & (1u << 13)) != 0);
        g_cpu_caps.has_shstk &= ((ecx & (1u << 7))  != 0);
        g_cpu_caps.has_ibt   &= ((edx & (1u << 20)) != 0);
        g_cpu_caps.has_pconfig &= ((edx & (1u << 18)) != 0);
        /* TDX vendor string re-check on AP. A hybrid socket where
         * some APs lack TDX would force the host-wide flag off — the
         * MSR 0x87 read becomes unsafe even if the BSP supported it. */
        if (g_cpu_caps.has_tdx) {
            uint32_t tdx_eax, tdx_ebx, tdx_ecx, tdx_edx;
            if (eax >= 0x21) {
                cpuid_count(0x21, 0, &tdx_eax, &tdx_ebx, &tdx_ecx, &tdx_edx);
                bool tdx_match = (tdx_ebx == 0x65746E49u &&
                                  tdx_edx == 0x5844546Cu &&
                                  tdx_ecx == 0x20202020u);
                g_cpu_caps.has_tdx &= tdx_match;
            } else {
                g_cpu_caps.has_tdx = false;
            }
        }
        if (eax >= 1) {
            uint32_t lam_eax, lam_ebx, lam_ecx, lam_edx;
            cpuid_count(CPUID_LEAF_EXT_FEATURES, 1,
                        &lam_eax, &lam_ebx, &lam_ecx, &lam_edx);
            g_cpu_caps.has_lam &= ((lam_eax & (1u << 26)) != 0);
        } else {
            g_cpu_caps.has_lam = false;  /* AP doesn't expose subleaf 1 */
        }
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
    /* ARAT — per-package architectural feature; intersect defensively
     * in case some heterogeneous boxes mix ARAT-capable and non-capable
     * cores (no shipping silicon does this, but the SDM does not
     * forbid it). */
    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_THERMAL_PM) {
        cpuid(CPUID_LEAF_THERMAL_PM, &eax, &ebx, &ecx, &edx);
        g_cpu_caps.has_arat &= ((eax & (1u << 2)) != 0);
    }

    /* Split-lock detection re-check on this AP: the IA32_CORE_CAPABILITIES
     * MSR is per-logical-processor (Intel SDM Vol 4 Table 2-2 "Scope:
     * Thread"). If the BSP claimed support but a later AP doesn't,
     * intersect (= AND) the bit off so cpu_test_ctl_init on that AP is
     * a no-op and the global policy reflects the weakest core. */
    if (g_cpu_caps.has_core_capabilities) {
        uint64_t core_cap = cpu_rdmsr(MSR_IA32_CORE_CAPABILITIES);
        g_cpu_caps.has_split_lock_detect &= (core_cap & (1ULL << 5)) != 0;
    } else {
        /* If the package-level CORE_CAPABILITIES bit got intersected
         * off above, the MSR is unreadable on this AP — force the
         * derived bit off too so no AP attempts to write TEST_CTL. */
        g_cpu_caps.has_split_lock_detect = false;
    }

    /* MONITOR/UMONITOR cacheline range — intersect by widening to the
     * worst case. The "smallest line size" we may encounter on ANY
     * core is the MIN across all cores; the "largest" is the MAX.
     * Downstream consumers (Brook BrookHeader padding sanity, Touch
     * ring layout) read these to decide whether the kernel-wide 64 B
     * cacheline assumption is safe — they want to know the worst
     * case any AP could see. */
    if (g_cpu_caps.max_basic_leaf >= CPUID_LEAF_MONITOR) {
        cpuid(CPUID_LEAF_MONITOR, &eax, &ebx, &ecx, &edx);
        uint16_t ap_min = (uint16_t)(eax & 0xFFFFu);
        uint16_t ap_max = (uint16_t)(ebx & 0xFFFFu);
        if (ap_min != 0 && ap_min < g_cpu_caps.monitor_line_min)
            g_cpu_caps.monitor_line_min = ap_min;
        if (ap_max != 0 && ap_max > g_cpu_caps.monitor_line_max)
            g_cpu_caps.monitor_line_max = ap_max;
    }

    /* Heterogeneity log — only fires when this AP actually flipped a
     * cached BSP boolean to 0, so the boot log shows exactly which
     * features the kernel had to drop because some AP couldn't
     * service them. Quiet on homogeneous packages (the common case).
     * The kernel-wide policy is already correct (the bit is now off
     * for every consumer) — this is operator visibility, not enforcement. */
    #define _LOG_DROP(name, was) \
        do { if ((was) && !g_cpu_caps.has_##name) { \
            kprintf("[CPU] AP feature drop: " #name " unavailable on this core; " \
                    "kernel-wide " #name " now off\n"); \
        } } while (0)
    _LOG_DROP(x2apic,       bsp_x2apic);
    _LOG_DROP(tsc_deadline, bsp_tsc_deadline);
    _LOG_DROP(monitor,      bsp_monitor);
    _LOG_DROP(xsave,        bsp_xsave);
    _LOG_DROP(avx,          bsp_avx);
    _LOG_DROP(pcid,         bsp_pcid);
    _LOG_DROP(pat,          bsp_pat);
    _LOG_DROP(avx512,       bsp_avx512);
    _LOG_DROP(fsgsbase,     bsp_fsgsbase);
    _LOG_DROP(smep,         bsp_smep);
    _LOG_DROP(smap,         bsp_smap);
    _LOG_DROP(umip,         bsp_umip);
    _LOG_DROP(invpcid,      bsp_invpcid);
    _LOG_DROP(tsc_adjust,   bsp_tsc_adjust);
    _LOG_DROP(erms,         bsp_erms);
    _LOG_DROP(fsrm,         bsp_fsrm);
    _LOG_DROP(invariant_tsc, bsp_inv_tsc);
    _LOG_DROP(arat,         bsp_arat);
    _LOG_DROP(core_capabilities, bsp_core_caps);
    _LOG_DROP(split_lock_detect, bsp_split_lock);
    #undef _LOG_DROP
}
