#ifndef ARCH_X86_64_CPUID_H
#define ARCH_X86_64_CPUID_H

#include "ktypes.h"

// CPUID Leaf Numbers
#define CPUID_LEAF_VENDOR        0x00000000
#define CPUID_LEAF_FEATURES      0x00000001
#define CPUID_LEAF_MONITOR       0x00000005   // MONITOR/MWAIT line size (EAX,EBX [15:0])
#define CPUID_LEAF_THERMAL_PM    0x00000006   // Thermal/Power Mgmt — has ARAT bit
#define CPUID_LEAF_EXT_FEATURES  0x00000007
#define CPUID_LEAF_XSAVE         0x0000000D
#define CPUID_LEAF_EXT_MAX       0x80000000
#define CPUID_LEAF_EXT_FEATURES2 0x80000001
#define CPUID_LEAF_APM           0x80000007
#define CPUID_LEAF_ADDR_SIZE     0x80000008

typedef struct {
    bool has_apic;              // On-chip APIC (CPUID.1:EDX[9])
    bool has_x2apic;            // x2APIC support (CPUID.1:ECX[21])
    bool has_tsc_deadline;      // LAPIC TSC-deadline timer (CPUID.1:ECX[24])
    bool has_monitor;           // MONITOR/MWAIT (CPUID.1:ECX[3])
    bool has_waitpkg;           // UMONITOR/UMWAIT support (CPUID.7.0:ECX[5])
    bool has_invariant_tsc;     // Invariant TSC
    bool has_xsave;             // XSAVE/XRSTOR support (CPUID.1:ECX[26])
    bool has_avx;               // AVX support (CPUID.1:ECX[28])
    bool has_avx512;            // AVX-512 Foundation (CPUID.7.0:EBX[16])
    bool has_smep;              // Supervisor Mode Execution Prevention (CPUID.7.0:EBX[7])
    bool has_smap;              // Supervisor Mode Access Prevention (CPUID.7.0:EBX[20])
    bool has_umip;              // User Mode Instruction Prevention (CPUID.7.0:ECX[2])
    bool has_fsgsbase;          // RDFSBASE/WRFSBASE/RDGSBASE/WRGSBASE (CPUID.7.0:EBX[0])
    bool has_la57;              // 5-level paging hardware support (CPUID.7.0:ECX[16])
    bool has_invpcid;           // INVPCID instruction (CPUID.7.0:EBX[10])
    bool has_nx;                // NX/XD bit support (CPUID.80000001h:EDX[20])
    bool has_pat;               // Page Attribute Table (CPUID.1:EDX[16])
    bool has_1gb_pages;         // 1GB pages (CPUID.80000001h:EDX bit 26, PDPE1GB)
    bool has_pcid;              // Process-Context Identifiers (CPUID.1:ECX bit 17)
    bool has_hypervisor;        // Hypervisor present (CPUID.1:ECX[31]) — see hypervisor.h
    bool has_tsc_adjust;        // IA32_TSC_ADJUST MSR (CPUID.7.0:EBX[1]) — per-AP TSC sync
    bool has_arat;              // Always Running APIC Timer (CPUID.6:EAX[2]) — LAPIC
                                // timer keeps ticking through C3+ deep idle. Without
                                // it, deep MWAIT/HLT stops LAPIC timer and scheduler
                                // hangs on the affected CPU.
    bool has_erms;              // Enhanced REP MOVSB/STOSB (CPUID.7.0:EBX[9]) — Intel
                                // SDM Vol 2B "REP/REPE/REPZ … MOVSB". With ERMS the
                                // CPU dispatches rep movsb/stosb as wide-internal
                                // bursts (full DRAM bandwidth on Ivy Bridge+), so the
                                // scalar 8-byte fallback can be replaced for bulk
                                // copies — see memcpy/memset in klib.
    bool has_fsrm;              // Fast Short REP MOV (CPUID.7.0:EDX[4]) — Ice Lake+.
                                // Extends ERMS efficiency down to very small n, so
                                // the bulk-copy threshold can drop further.
    /* Split-lock detection chain — CPUID.07H.0:EDX[30] enumerates that
     * IA32_CORE_CAPABILITIES (MSR 0xCF) exists; bit 5 of that MSR then
     * tells whether the CPU can raise #AC on cache-line-spanning LOCK
     * operands. Intel SDM Vol 4 Table 2-2 (IA32_CORE_CAPABILITIES) +
     * SDM Vol 3 §6.15 (#AC handling). Required on Tiger Lake / Ice Lake
     * Server / Sapphire Rapids / Emerald Rapids; absent on AMD and on
     * older Intel client silicon (pre-Tremont). When true, BoxOS
     * explicitly programs TEST_CTL (MSR 0x33) bit 29 = 0 at every
     * BSP/AP bringup, taking the "detection off / accept slow bus lock
     * on split access" policy — see cpu_test_ctl_init for rationale. */
    bool has_core_capabilities; // CPUID.7.0:EDX[30] — MSR 0xCF readable
    bool has_split_lock_detect; // IA32_CORE_CAPABILITIES.bit5 — CPU can raise #AC
    /* MONITOR/UMONITOR cacheline granularity — CPUID.05H. Intel SDM Vol 2A
     * UMONITOR: "The address range determined by the CPUID monitor leaf
     * function". EAX[15:0] = smallest line size, EBX[15:0] = largest.
     * 0 = leaf not implemented (treat as 64 B default for cacheline-shape
     * decisions; UMONITOR still works correctly — granularity is a perf/
     * false-sharing concern, not a correctness one). Used by Brook +
     * Touch + ResultRing to verify the assumed 64 B cacheline padding is
     * compatible with the running silicon (Atom Tremont/Goldmont report
     * 32 B; some server CPUs report 128 B). */
    uint16_t monitor_line_min;
    uint16_t monitor_line_max;
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

/* Per-AP capability intersection — call once on every AP after it has
 * entered long mode but before any non-trivial kernel code path on
 * that AP touches CPU features. ANDs the local CPUID feature bits
 * into g_cpu_caps so the kernel-wide flags reflect the INTERSECTION
 * of every online core's capabilities (safe on heterogeneous P+E /
 * big.LITTLE CPUs). */
void cpu_intersect_features_ap(void);

/* IA32_UMWAIT_CONTROL programmer — Intel SDM Vol 4 §2.5.1 (MSR 0xE1).
 * Sets the OS-imposed maximum UMWAIT/TPAUSE residency in TSC quanta so
 * the wait loop top can re-poll within a bounded interval even if a
 * monitor wake-event is missed (microcode / cache-coherence quirk).
 *
 * Must be called AFTER cpu_calibrate_tsc() (needs tsc_freq_khz) and on
 * each AP after cpu_intersect_features_ap() (so a P-core's MSR write
 * isn't issued on an E-core that has WAITPKG disabled).
 *
 * No-op on non-WAITPKG silicon (writing 0xE1 on AMD or pre-Tremont
 * Intel would #GP). */
void cpu_umwait_control_init(uint64_t tsc_freq_khz);

/* TEST_CTL MSR (0x33) bit 29 programmer — Intel SDM Vol 4 §2.5 / Table
 * 2-2 (TEST_CTL). When bit 29 is set, any cache-line-spanning LOCK-
 * prefixed instruction on this logical processor raises #AC (vector 17,
 * error_code = 0). When clear, the CPU silently asserts the system bus
 * lock and serves the access (slow but correct).
 *
 * BoxOS policy: explicitly CLEAR bit 29 on every BSP and AP whose
 * has_split_lock_detect == true. Our internal atomics are all 8-byte
 * aligned within single cachelines (kring.c / touch_ring.c Vyukov gates,
 * brook.c / bay.c headers), so we will never trigger split-lock. But
 * userspace processes can race-corrupt their own state and emit
 * unaligned LOCK ops; we don't want a buggy boxlib to silently turn
 * into a #AC kernel-panic chain when running on Tiger Lake+. The
 * trade-off is the (extremely rare) silent slowdown if userspace ever
 * actually executes a split-lock RMW vs. predictable system stability.
 *
 * Idempotent — re-running on the same core just re-writes the same
 * bit clear. Safe to call before or after cpu_umwait_control_init. */
void cpu_test_ctl_init(void);

/* Read the current microcode revision running on THIS logical CPU.
 *
 * Intel sequence (SDM Vol 3A §9.11.7.1 "Determining the Signature"):
 *   1. wrmsr(IA32_BIOS_SIGN_ID, 0)         — clear the MSR
 *   2. cpuid(EAX=1)                         — side-effect: populates
 *                                             MSR with running revision
 *   3. revision = rdmsr(IA32_BIOS_SIGN_ID) >> 32
 *
 * AMD: the same MSR (0x8B) directly reports the current PatchLevel
 * without the cpuid-side-effect dance; the cpuid step is harmless on
 * AMD so the same sequence works portably. The MSR layout differs
 * (AMD packs revision in the LOW 32 bits, Intel in the high 32 bits) —
 * we read both halves and return the non-zero one, biased toward
 * Intel's high-half convention when both are populated. Returns 0
 * when no microcode update is present (Intel: the post-cpuid MSR
 * read returns 0; AMD: factory CPUs report 0 too). */
uint32_t cpu_microcode_revision(void);

/* CPU vendor / family / model / stepping decoded from CPUID leaf 1.
 * The "effective family" computation follows Intel SDM Vol 2A "CPUID":
 *   effective_family = base_family + (base_family==0x0F ? ext_family : 0)
 *   effective_model  = base_model  | ((base_family==0x06||0x0F)
 *                                      ? ext_model<<4 : 0)
 * AMD APM Vol 3 §3.3 uses identical rules so the routine works for
 * both vendors. */
typedef struct {
    char     vendor[13];      /* "GenuineIntel"/"AuthenticAMD"/etc., NUL-terminated */
    uint32_t family;          /* effective family */
    uint32_t model;           /* effective model  */
    uint32_t stepping;        /* CPUID.1:EAX[3:0] */
    uint32_t type;            /* CPUID.1:EAX[13:12] (Intel only) */
    uint32_t microcode_rev;   /* result of cpu_microcode_revision() at probe */
    uint32_t apic_id;         /* CPUID.1:EBX[31:24] (xAPIC) — packed here for
                                 fast operator log without an extra MSR read */
} cpu_identity_t;

/* Read the running CPU's identity into *out. Always succeeds; vendor
 * is the canonical 12-char string and the numeric fields hold the
 * decoded values above. */
void cpu_read_identity(cpu_identity_t* out);

/* Log "[CPU] vendor F:M:S microcode=0xNNNN" via kprintf. `prefix`
 * lets callers add a tag (e.g. "BSP" / "AP %u"). NULL prefix = no
 * tag. Used by BSP init and each AP entry path to make the boot log
 * a record of exactly what silicon the kernel saw. */
void cpu_log_identity(const char* prefix);

static inline uint8_t cpuid_get_maxphyaddr(void) {
    uint32_t eax, ebx, ecx, edx;

    cpuid(CPUID_LEAF_EXT_MAX, &eax, &ebx, &ecx, &edx);

    if (eax < CPUID_LEAF_ADDR_SIZE) {
        return 36;
    }

    cpuid(CPUID_LEAF_ADDR_SIZE, &eax, &ebx, &ecx, &edx);
    uint8_t phys_bits = (uint8_t)(eax & 0xFF);

    if (phys_bits < 32 || phys_bits > 52) {
        return 36;
    }

    return phys_bits;
}

static inline uint8_t cpuid_get_maxvirtaddr(void) {
    uint32_t eax, ebx, ecx, edx;

    cpuid(CPUID_LEAF_EXT_MAX, &eax, &ebx, &ecx, &edx);

    if (eax < CPUID_LEAF_ADDR_SIZE) {
        return 48;
    }

    cpuid(CPUID_LEAF_ADDR_SIZE, &eax, &ebx, &ecx, &edx);
    uint8_t virt_bits = (uint8_t)((eax >> 8) & 0xFF);

    if (virt_bits < 48 || virt_bits > 57) {
        return 48;
    }

    return virt_bits;
}

#endif // ARCH_X86_64_CPUID_H
