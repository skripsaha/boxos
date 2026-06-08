#ifndef PER_CORE_H
#define PER_CORE_H

#include "ktypes.h"
#include "notify.h"
#include "tss.h"
#include "gdt.h"
#include "amp.h"

#define PER_CORE_GDT_ENTRIES  7

// Per-core data for each CPU.
//
// PerCpuData MUST be at offset 0 — swapgs reads gs:0 for kernel_rsp.
// Each core gets its own GDT (because the TSS descriptor base differs),
// its own TSS (rsp0 + IST stacks are per-core), and its own PerCpuData
// (SYSCALL kernel stack is per-core).
//
// Layout is cache-line aligned (64 bytes) to avoid false sharing.
typedef struct {
    // --- offset 0x00: PerCpuData (swapgs target) ---
    PerCpuData          notify;

    // --- Core identity ---
    uint32_t            lapic_id;     // full-width xAPIC/x2APIC ID
    uint8_t             core_index;
    bool                is_kcore;
    bool                initialized;
    uint8_t             _pad0;

    // --- Per-core GDT (7 entries: null, kcode, kdata, udata, ucode, tss_lo, tss_hi) ---
    gdt_entry_t         gdt[PER_CORE_GDT_ENTRIES] __attribute__((aligned(16)));
    gdt_descriptor_t    gdt_desc;

    // --- Per-core TSS (rsp0 + IST stacks unique per core) ---
    tss_t               tss __attribute__((aligned(16)));

    // --- Kernel stack top for this core (boot stack or current process stack) ---
    uint64_t            kernel_stack_top;

    /* CET — per-CPU supervisor shadow-stack infrastructure.
     *
     * Populated by cet_lifecycle_init_supervisor_ssp() during per-core
     * init. Owns:
     *   - PL0_SSP page (4 KiB) backing IA32_PL0_SSP. Used when an
     *     interrupt/exception is delivered to CPL=0 with IST=0.
     *   - IA32_INTERRUPT_SSP_TABLE_ADDR backing page (4 KiB; only the
     *     first 8 × 8-byte entries are used) — one SSP per IST level.
     *   - Per-IST supervisor SSP pages (4 KiB × 5) for the IST vectors
     *     BoxOS uses (1=#DF, 2=NMI, 3=#MC, 4=#DB, 5=#SS).
     *
     * The MSRs IA32_PL0_SSP and IA32_INTERRUPT_SSP_TABLE_ADDR are written
     * by cet_lifecycle_init_supervisor_ssp on every core that brings up
     * CET. S_CET.SH_STK_EN stays 0 in this session — flipping it
     * requires a kernel-wide assembly audit (LRETQ in per_core_load_gdt
     * has no matching FAR CALL on the shadow stack; any code-injected
     * CALL/RET asymmetry would #CP at first kernel return). The
     * follow-up commit that audits assembly will flip the bit; until
     * then this is dormant infrastructure ready to activate. */
    uintptr_t           pl0_ssp_phys;          // PL0 SSP phys page (4 KiB)
    uintptr_t           pl0_ssp_top_va;        // IA32_PL0_SSP value (token VA)
    uintptr_t           isst_phys;             // ISST backing page phys
    uintptr_t           isst_va;               // ISST kernel-VA (table[0..7])
    uintptr_t           ist_ssp_phys[5];       // IST 1..5 SSP phys pages
    uintptr_t           ist_ssp_top_va[5];     // IST 1..5 SSP token VAs
    bool                cet_supv_ready;        // true after WRMSR's succeeded
    uint8_t             _pad1[7];
} __attribute__((aligned(64))) PerCoreData;

extern PerCoreData g_per_core[MAX_CORES];
extern volatile bool g_per_core_active;

// Initialize per-core data for BSP.
// Transitions BSP from static GDT/TSS to per-core copies.
// Called after amp_init() when PMM/VMM are ready, before amp_boot_aps().
void per_core_init_bsp(void);

// Initialize per-core data for an AP.
// Sets up GDT, TSS (with IST stacks), SYSCALL MSRs, LAPIC timer.
// Called from ap_entry_c().
void per_core_init_ap(uint8_t core_index, uint64_t stack_top);

// Update kernel RSP for current core.
// Sets both TSS.rsp0 (for INT/exception from ring 3) and
// PerCpuData.kernel_rsp (for SYSCALL via swapgs).
// Replaces separate tss_set_rsp0() + notify_set_kernel_rsp() calls.
void per_core_set_kernel_rsp(uint64_t rsp);

/* Capture the BSP's TSC value + current uptime in microseconds as the
 * anchor for per-AP TSC sync. Called from kernel_main after
 * cpu_calibrate_tsc and before any AP boots. The values feed
 * per_core_init_ap's IA32_TSC_ADJUST correction so multi-socket
 * systems with skewed power-on TSCs don't produce negative cross-core
 * cycle deltas. No-op (safe) if called multiple times. */
void per_core_record_bsp_tsc_anchor(uint64_t now_us);

#endif // PER_CORE_H
