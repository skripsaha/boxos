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
    uint8_t             core_index;
    uint8_t             lapic_id;
    bool                is_kcore;
    bool                initialized;
    uint32_t            _pad0;

    // --- Per-core GDT (7 entries: null, kcode, kdata, udata, ucode, tss_lo, tss_hi) ---
    gdt_entry_t         gdt[PER_CORE_GDT_ENTRIES] __attribute__((aligned(16)));
    gdt_descriptor_t    gdt_desc;

    // --- Per-core TSS (rsp0 + IST stacks unique per core) ---
    tss_t               tss __attribute__((aligned(16)));

    // --- Kernel stack top for this core (boot stack or current process stack) ---
    uint64_t            kernel_stack_top;
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

#endif // PER_CORE_H
