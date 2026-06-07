#include "per_core.h"
#include "amp.h"
#include "lapic.h"
#include "pmm.h"
#include "vmm.h"
#include "klib.h"
#include "irqchip.h"
#include "fpu.h"
#include "cpuid.h"
#include "atomics.h"      // rdtsc()
#include "pvclock.h"      // pvclock_init_ap()
#include "cpu_calibrate.h"// cpu_get_tsc_freq_khz()
#include "pit.h"          // pit_get_uptime_us() — HPET-backed wall clock
#include "cpu_caps_page.h"// cpu_caps_page_refresh_features()
#include "memtag.h"       // MemTagVerifyPteMetadataBits() — Phase 2D M5
#include "mce.h"          // mce_ap_init() — Phase 2F

PerCoreData g_per_core[MAX_CORES] __attribute__((aligned(64)));
volatile bool g_per_core_active = false;

/* TSC-sync anchor captured by the BSP at calibration time. The triple
 * (anchor_tsc, anchor_us, anchor_khz) must be read as a CONSISTENT
 * snapshot — the periodic recalibrator can republish tsc_freq_khz at
 * any time, and an AP that combines a fresh anchor_tsc with a stale
 * tsc_khz (or vice versa) would compute a wild "expected" TSC and
 * write a multi-second skew through IA32_TSC_ADJUST.
 *
 * Discipline: write `anchor_khz` and `anchor_us` first (relaxed),
 * then `anchor_tsc` with RELEASE — APs gate on `anchor_tsc != 0` with
 * ACQUIRE, which synchronises with the earlier writes.
 *
 * Why anchor here and not in cpu_calibrate.c: the anchor needs to be
 * a single shared kernel global accessible from per_core_init_ap()
 * before any scheduler / process plumbing. cpu_calibrate.c knows about
 * calibration; per_core.c owns AP bring-up — that's where the AP-side
 * sync code lives, so the BSP-side capture lives next to it. */
volatile uint64_t g_bsp_tsc_anchor     = 0;   /* TSC the BSP read at anchor time */
volatile uint64_t g_bsp_tsc_anchor_us  = 0;   /* uptime in µs at anchor time */
volatile uint64_t g_bsp_tsc_anchor_khz = 0;   /* tsc_freq_khz used for extrapolation */

/* Per-Intel SDM §17.17 typical inter-socket TSC skew on a well-behaved
 * BIOS is < 50 cycles. AP_TSC_SKEW_THRESHOLD_US converts the audit
 * recommendation ("≤4 µs") into a rate-relative cycle count at sync
 * time, so the absolute threshold scales with the actual CPU clock —
 * a 1 GHz Bochs CPU and a 5 GHz Xeon both get the same wall-clock
 * tolerance. */
#define AP_TSC_SKEW_THRESHOLD_US   4ULL

void per_core_record_bsp_tsc_anchor(uint64_t now_us)
{
    /* Single-writer (BSP, during boot). Pre-fetch tsc_khz from the
     * calibrated value at this exact moment, then publish the triple
     * in the correct order: companions first (relaxed), gating
     * anchor_tsc last (release). AP-side acquire on anchor_tsc
     * synchronises-with our release and guarantees visibility of
     * both anchor_us AND anchor_khz before extrapolation uses them.
     *
     * The earlier (us-then-tsc-only) pattern allowed an AP combining
     * a fresh anchor with the periodic recal's newly-published
     * tsc_khz — extrapolation would scale the elapsed time by the
     * wrong rate and the IA32_TSC_ADJUST correction would jump the
     * AP TSC by the difference. */
    extern uint64_t cpu_get_tsc_freq_khz(void);
    uint64_t khz_now = cpu_get_tsc_freq_khz();
    __atomic_store_n(&g_bsp_tsc_anchor_us,  now_us,  __ATOMIC_RELAXED);
    __atomic_store_n(&g_bsp_tsc_anchor_khz, khz_now, __ATOMIC_RELAXED);
    __atomic_store_n(&g_bsp_tsc_anchor,     rdtsc(), __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// MSR helpers (local to this file)
// ---------------------------------------------------------------------------
#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_SFMASK          0xC0000084
#define MSR_GS_BASE         0xC0000101   // active GS.base
#define MSR_KERNEL_GS_BASE  0xC0000102   // swapgs shadow
#define MSR_IA32_TSC        0x00000010   // Time Stamp Counter
#define MSR_IA32_TSC_ADJUST 0x0000003B   // Per-logical-processor TSC offset
                                          // (Intel SDM Vol 3A §17.17.3)

#define EFER_SCE            (1ULL << 0)
#define EFER_NXE            (1ULL << 11)
#define SFMASK_VALUE        ((1ULL << 9) | (1ULL << 8) | (1ULL << 10))

static inline uint64_t rdmsr_pc(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr_pc(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr),
                     "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)));
}

// ---------------------------------------------------------------------------
// GDT helpers
// ---------------------------------------------------------------------------

// Write a 16-byte TSS descriptor at GDT entries [5] and [6].
static void per_core_gdt_set_tss(gdt_entry_t* gdt, tss_t* tss) {
    uint64_t base  = (uint64_t)tss;
    uint64_t limit = sizeof(tss_t) - 1;

    // Entry 5: lower half
    gdt[5].limit_low   = limit & 0xFFFF;
    gdt[5].base_low    = base & 0xFFFF;
    gdt[5].base_middle = (base >> 16) & 0xFF;
    gdt[5].access      = 0x89;  // Present, DPL=0, Available 64-bit TSS
    gdt[5].granularity = (limit >> 16) & 0x0F;
    gdt[5].base_high   = (base >> 24) & 0xFF;

    // Entry 6: upper 32 bits of base address
    uint64_t* entry6 = (uint64_t*)&gdt[6];
    *entry6 = (base >> 32);
}

// Load GDT from descriptor pointer, then reload CS via far return and
// all data segment registers.  Identical logic to gdt_load_asm() in gdt.c.
static void per_core_load_gdt(gdt_descriptor_t* desc) {
    __asm__ volatile (
        "lgdt (%0)\n\t"
        "pushq %1\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "movw %w2, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        /* Deliberately do NOT reload %gs here. In 64-bit mode a selector load
         * resets the segment's hidden base to the descriptor base (0), which
         * would wipe the per-cpu GS base that per_core_load_gs() programmed
         * via IA32_GS_BASE. The kernel addresses per-cpu data through that MSR
         * base, not the %gs selector, so we leave %gs untouched. */
        "movw %%ax, %%ss"
        :
        : "r" ((uint64_t)desc),
          "i" ((uint64_t)GDT_KERNEL_CODE),
          "i" ((uint32_t)GDT_KERNEL_DATA)
        : "memory", "rax"
    );
}

static void per_core_load_tss(void) {
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS));
}

// ---------------------------------------------------------------------------
// IST stack allocation (PMM + guard pages)
// ---------------------------------------------------------------------------

static void per_core_alloc_ist(tss_t* tss, uint8_t core_index) {
    vmm_context_t* kctx = vmm_get_kernel_context();

    for (int i = 0; i < IST_COUNT; i++) {
        size_t total_pages = IST_GUARD_PAGES + IST_STACK_PAGES;
        void* phys = pmm_alloc(total_pages);
        if (!phys) {
            kprintf("[PER_CORE] FATAL: IST%d alloc failed for core %u\n",
                    i + 1, core_index);
            while (1) { __asm__ volatile("cli; hlt"); }
        }

        void* virt_base = vmm_phys_to_virt((uintptr_t)phys);

        // Unmap guard page (first page) — access triggers page fault
        pte_t* guard_pte = vmm_get_or_create_pte(kctx, (uintptr_t)virt_base);
        if (!guard_pte) {
            kprintf("[PER_CORE] FATAL: IST%d guard page PTE failed for core %u\n",
                    i + 1, core_index);
            while (1) { __asm__ volatile("cli; hlt"); }
        }
        *guard_pte = 0;
        /* Cross-core shootdown so peer cores see the cleared guard
         * PTE through the demoted 4 KB leaf rather than their cached
         * huge Pull-Map entry. */
        vmm_shootdown_page(kctx, (uintptr_t)virt_base);

        // Stack grows down: top = base + (guard + data) * PAGE_SIZE - 16
        uint64_t stack_top = (uint64_t)virt_base + (total_pages * 4096) - 16;

        switch (i) {
            case 0: tss->ist1 = stack_top; break;  // Double Fault
            case 1: tss->ist2 = stack_top; break;  // NMI
            case 2: tss->ist3 = stack_top; break;  // Machine Check
            case 3: tss->ist4 = stack_top; break;  // Debug
            case 4: tss->ist5 = stack_top; break;  // Stack Fault
        }
    }
}

// ---------------------------------------------------------------------------
// GS-base invariant (see notify.h)
// ---------------------------------------------------------------------------
//
// Establish, for THIS cpu: active GS.base -> per-cpu PerCpuData; swapgs shadow
// (IA32_KERNEL_GS_BASE) -> 0 (user placeholder). After this, kernel code may
// read per-cpu fields via %gs in ANY context — the entry stubs (isr.asm,
// notify_entry.asm, jump_to_userspace) keep the invariant across ring
// crossings. MUST run before any path on this cpu calls amp_get_core_index()
// while g_per_core_active is set, because that reads core_index through %gs
// (e.g. vmm_shootdown_page() invoked from per_core_alloc_ist()).
static void per_core_load_gs(PerCoreData* pc) {
    pc->notify.self       = (uint64_t)&pc->notify;
    pc->notify.core_index = pc->core_index;
    wrmsr_pc(MSR_GS_BASE, (uint64_t)&pc->notify);   // active  = per-cpu (kernel view)
    wrmsr_pc(MSR_KERNEL_GS_BASE, 0);                // shadow = user placeholder
}

// ---------------------------------------------------------------------------
// SYSCALL / Notify MSR setup (per-core)
// ---------------------------------------------------------------------------

static void per_core_setup_notify_msrs(PerCoreData* pc) {
    // EFER: enable SYSCALL + NX
    uint64_t efer = rdmsr_pc(MSR_EFER);
    efer |= EFER_SCE | EFER_NXE;
    wrmsr_pc(MSR_EFER, efer);

    // STAR: kernel CS/SS in [47:32], user base in [63:48]
    //   Entry: CS = STAR[47:32], SS = STAR[47:32]+8
    //   Exit:  SS = STAR[63:48]+8, CS = STAR[63:48]+16
    uint64_t star = ((uint64_t)GDT_KERNEL_DATA << 48) |
                    ((uint64_t)GDT_KERNEL_CODE << 32);
    wrmsr_pc(MSR_STAR, star);

    // LSTAR: SYSCALL entry point
    wrmsr_pc(MSR_LSTAR, (uint64_t)notify_entry);

    // SFMASK: clear IF, TF, DF on SYSCALL entry
    wrmsr_pc(MSR_SFMASK, SFMASK_VALUE);

    // PerCpuData (at offset 0 of PerCoreData). The GS base itself is set by
    // per_core_load_gs() earlier in init — see the invariant in notify.h. We
    // must NOT write IA32_KERNEL_GS_BASE here: under the new invariant the
    // shadow holds the *user* GS base (0), not the per-cpu pointer.
    pc->notify.kernel_rsp = pc->kernel_stack_top;
    pc->notify.user_rsp   = 0;
    pc->notify.self        = (uint64_t)&pc->notify;
}

// ===========================================================================
// Public API
// ===========================================================================

void per_core_init_bsp(void) {
    uint8_t bsp_idx = g_amp.bsp_index;
    PerCoreData* pc = &g_per_core[bsp_idx];

    kprintf("[PER_CORE] Initializing BSP per-core data (core %u)...\n", bsp_idx);

    memset(pc, 0, sizeof(PerCoreData));
    pc->core_index = bsp_idx;
    pc->lapic_id   = g_amp.bsp_lapic_id;
    pc->is_kcore   = g_amp.cores[bsp_idx].is_kcore;

    // Program the per-cpu GS base now. per_core_load_gdt() below no longer
    // reloads %gs, so this base survives to the end of init (and beyond),
    // making amp_get_core_index()'s gs:core_index read valid once
    // g_per_core_active flips on at the bottom of this function.
    per_core_load_gs(pc);

    // ---- GDT ----
    // Copy BSP's static GDT entries (segments 0-4 are identical across all cores)
    gdt_copy_entries(pc->gdt, PER_CORE_GDT_ENTRIES);

    // Copy BSP's current TSS (already has dynamic IST stacks from
    // tss_setup_dynamic_stacks(), so we inherit them).
    tss_t* bsp_tss = tss_get_ptr();
    memcpy(&pc->tss, bsp_tss, sizeof(tss_t));

    // Inherit current kernel stack from the static TSS
    pc->kernel_stack_top = pc->tss.rsp0;

    // Point TSS descriptor in per-core GDT to THIS core's TSS.
    // Must set access = 0x89 (available), not 0x8B (busy from prior ltr).
    per_core_gdt_set_tss(pc->gdt, &pc->tss);

    // GDT descriptor
    pc->gdt_desc.limit = sizeof(pc->gdt) - 1;
    pc->gdt_desc.base  = (uint64_t)&pc->gdt;

    // Load per-core GDT (lgdt + reload all segment registers)
    per_core_load_gdt(&pc->gdt_desc);

    // Load per-core TSS (ltr — reads TSS descriptor from the new GDT)
    per_core_load_tss();

    // ---- Notify MSRs + PerCpuData ----
    per_core_setup_notify_msrs(pc);

    pc->initialized = true;
    __atomic_store_n(&g_per_core_active, true, __ATOMIC_RELEASE);

    kprintf("[PER_CORE] BSP ready: GDT=0x%lx TSS=0x%lx KernelGSBASE=0x%lx\n",
            (uint64_t)&pc->gdt, (uint64_t)&pc->tss, (uint64_t)&pc->notify);
}

void per_core_init_ap(uint8_t core_index, uint64_t stack_top) {
    /* Intersect THIS AP's CPUID feature bits with the kernel-wide
     * g_cpu_caps. On homogeneous CPUs this is a no-op (every AP
     * AND's identical bits in). On heterogeneous Intel P+E (Alder
     * Lake and later) or ARM big.LITTLE it prevents kernel code from
     * later emitting an instruction the weakest core can't execute,
     * which would otherwise #UD on that AP. Runs BEFORE any g_cpu_caps
     * consumer on this AP — per_core data init below only reads
     * core-static structs, no CPU features yet. */
    cpu_intersect_features_ap();

    /* Re-publish post-intersect feature bits into the userspace caps
     * page so a UMWAIT or RDPKRU/WRPKRU caller running on this AP (or
     * any later AP) sees the post-intersect value. On homogeneous CPUs
     * the bits are unchanged; on Intel hybrid (Alder/Raptor Lake) the
     * has_waitpkg bit may have flipped 1→0 because an E-core lacks
     * WAITPKG, and without this refresh userspace would read stale
     * `1` and #UD here. Same logic covers has_pku for future hybrids
     * that disable PKU on a core class. */
    cpu_caps_page_refresh_features();

    /* MemTag Phase 2D M5 — verify PTE bits 52-58 are still "Ignored" on
     * THIS AP. On hybrid CPUs an AP may report different CR4.PKE/CR4.CET
     * state than the BSP; the probe logs the per-AP result. Failure
     * here (MAXPHYADDR > 52 on a future arch) is informational — the
     * encoding is still memory-safe, just degraded to fall-back-only
     * via MemRegionFromPte's phys verification. */
    (void)MemTagVerifyPteMetadataBits();

    /* Program IA32_UMWAIT_CONTROL on this AP. WAITPKG-gated inside;
     * no-op on AMD or E-cores that lack it. Must happen AFTER the
     * intersect above so a P-core's MSR write isn't issued on an
     * E-core where WAITPKG is off. Needs tsc_freq_khz so cpu_calibrate
     * must have completed on the BSP — which it has by the time the
     * first AP enters this function (main.c sequences cpu_calibrate
     * before amp_boot_aps). */
    cpu_umwait_control_init(cpu_get_tsc_freq_khz());

    /* Clear TEST_CTL bit 29 on this AP so split-lock LOCK ops don't
     * raise #AC. The MSR is per-logical-processor (Intel SDM Vol 4
     * Table 2-2 "Scope: Thread"); BSP programmed its own copy in
     * main.c. Gated inside on has_split_lock_detect — no-op on AMD
     * and pre-Tremont Intel. Must follow the intersect above so the
     * MSR isn't written on an AP that the intersect decided lacks
     * the architectural capability. */
    cpu_test_ctl_init();

    PerCoreData* pc = &g_per_core[core_index];

    memset(pc, 0, sizeof(PerCoreData));
    pc->core_index       = core_index;
    pc->lapic_id         = g_amp.cores[core_index].lapic_id;
    pc->is_kcore         = g_amp.cores[core_index].is_kcore;
    pc->kernel_stack_top = stack_top;

    // Program the per-cpu GS base NOW — before per_core_alloc_ist() below,
    // which calls vmm_shootdown_page() -> amp_get_core_index(). On an AP
    // g_per_core_active is already set (by the BSP), so amp_get_core_index()
    // takes the %gs fast path and the base must already be live. Because
    // per_core_load_gdt() no longer reloads %gs, this base survives the rest
    // of init — a single write here is enough.
    per_core_load_gs(pc);

    // ---- GDT ----
    // Copy code/data segments (0-4) from BSP's per-core GDT
    PerCoreData* bsp = &g_per_core[g_amp.bsp_index];
    memcpy(pc->gdt, bsp->gdt, sizeof(gdt_entry_t) * 5);

    // ---- TSS ----
    memset(&pc->tss, 0, sizeof(tss_t));
    pc->tss.rsp0       = stack_top;
    pc->tss.iomap_base = sizeof(tss_t);

    // Allocate per-core IST stacks (Double Fault, NMI, Machine Check, Debug, Stack Fault)
    per_core_alloc_ist(&pc->tss, core_index);

    // Point TSS descriptor to this core's TSS
    per_core_gdt_set_tss(pc->gdt, &pc->tss);

    // GDT descriptor
    pc->gdt_desc.limit = sizeof(pc->gdt) - 1;
    pc->gdt_desc.base  = (uint64_t)&pc->gdt;

    // Load per-core GDT
    per_core_load_gdt(&pc->gdt_desc);

    // Load per-core TSS
    per_core_load_tss();

    // ---- FPU/SSE/AVX ----
    enable_fpu();

    /* Re-program IA32_PAT on this AP. Intel SDM Vol 3A §11.12.4: PAT is
     * per-logical-processor. Without this call, an AP keeps the firmware
     * reset default (PA6 = UC-) while the BSP programmed PA6 = WC for
     * framebuffer mappings — any framebuffer write issued by code that
     * lands on this AP gets coalesced as UC-, not WC. Manifests as
     * torn/re-ordered pixels on real HW after the first AP comes online. */
    vmm_pat_init();

    /* MemTag Phase 2E — verify this AP's IA32_PAT matches the BSP-cached
     * value. Intel SDM Vol 3A §11.12.4 mandates an identical PAT across
     * all coherent logical processors; a divergence is a hardware bug or
     * firmware misconfig and would cause silent cache-type splits between
     * cores (e.g. AP-rendered framebuffer pixels coalesced as UC- while
     * BSP-rendered pixels are WC). MemTagVerifyPatMsr also catches the
     * race where an AP would run vmm_pat_init AFTER vmm_pat_init's
     * RELEASE-store on BSP but before any consistency check fired. */
    (void)MemTagVerifyPatMsr();

    /* Phase 2F — Machine Check Architecture per-AP bring-up. Enables
     * CR4.MCE on this AP, programs every reporting bank's IA32_MC<i>_CTL,
     * clears stale status. Intel SDM Vol 3B §15.3.2: each logical
     * processor has its own MCA registers; without this call the AP
     * either ignores hardware errors (no #MC delivery) or remains in
     * "MCIP=1" state from a firmware-injected probe error and silently
     * drops the next real fault. */
    mce_ap_init();

    /* Phase 2H — Protection Keys per-AP. Sets CR4.PKE/PKS + XCR0.PKRU
     * to match the BSP-decided policy. AP-local CR4/XCR0 are required
     * by Intel SDM Vol 3A §4.6.2: both registers are per-logical-
     * processor, so the BSP write doesn't propagate. Without this
     * call, this AP would #PF with PF.PK=0 (key check off) while the
     * BSP fires PF.PK=1, splitting the security model across cores. */
    vmm_pku_ap_init();

    /* Phase 2I — LAM per-AP probe (observe-only). Logs this AP's CR3
     * LAM bits + CR4.LAM_SUP state. Catches a hybrid SKU's AP that
     * lost has_lam during cpu_intersect_features_ap. */
    vmm_lam_ap_probe();

    /* Phase 2J — TME per-AP probe. TME activation is platform-wide,
     * but every AP should see identical state; the probe verifies and
     * logs any divergence (firmware bug class on multi-socket systems
     * where one socket's TME programming drifted). */
    vmm_tme_ap_probe();

    /* Phase 2K — CET per-AP probe. Mirrors BSP path to log per-AP
     * CR4.CET state + IA32_S_CET / IA32_U_CET. */
    vmm_cet_ap_probe();

    /* Enable CR4.PCIDE on this AP if the BSP turned PCID on. Intel SDM
     * Vol 3A §4.10.4.1: CR4.PCIDE is per-logical-processor. Without
     * this, an AP running with PCIDE=0 will #GP the moment the scheduler
     * loads CR3 with a user process's PCID in bits 11:0 (the bits are
     * reserved when PCIDE=0). The pre-condition for enabling PCIDE —
     * CR3[11:0] = 0 — holds here because the AP trampoline loaded CR3
     * with the kernel PML4 (PCID 0) and no context switch has happened
     * yet on this AP. Without this fix, multi-core boots only work on
     * QEMU TCG, which is lax about that reserved-bit rule; real Intel
     * silicon and KVM enforce it strictly. */
    if (vmm_pcid_active()) {
        uint64_t cr4_pcid;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4_pcid));
        cr4_pcid |= (1ULL << 17);   /* CR4.PCIDE */
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4_pcid) : "memory");
    }

    /* Per-AP TSC sync via IA32_TSC_ADJUST. Intel SDM Vol 3A §17.17.3:
     * the MSR is per-logical-processor; writing it adjusts the TSC value
     * read by RDTSC on that logical processor by the delta written.
     *
     * Real-HW problem this fixes: on multi-socket boards the BIOS often
     * leaves each socket's TSC slightly skewed at power-on (typical
     * skew is < 100 cycles, but worst-case observed values on shipping
     * boards exceed 1 ms). The kernel scheduler and lock subsystem
     * compare TSC values across cores for things like spinlock
     * contention timeouts; large skews cause spurious timeouts or
     * negative deltas.
     *
     * Algorithm: compute the expected in-sync TSC by extrapolating from
     * the BSP anchor over the wall-clock time that has elapsed since.
     * The wall clock is HPET-backed via hpet_now_us() — already in use
     * across the kernel and immune to PIT freq reprogramming. If the
     * AP-local rdtsc differs from expected by more than
     * AP_TSC_SKEW_THRESHOLD_CYC, write IA32_TSC_ADJUST to align so
     * future rdtsc reads on this AP match the BSP. */
    if (g_cpu_caps.has_tsc_adjust && g_bsp_tsc_anchor != 0) {
        /* Snapshot ALL three anchor fields under one acquire-release
         * fence pair. The BSP publishes (us, khz, tsc) in that order;
         * we read in the reverse: anchor TSC last with ACQUIRE
         * pairs with the BSP's RELEASE on anchor TSC, which makes
         * the earlier (relaxed) us/khz stores visible to us. Using
         * cpu_get_tsc_freq_khz() here would race with the periodic
         * recalibration publishing a new rate against the OLD
         * anchor — using anchor_khz keeps the triple consistent. */
        uint64_t bsp_anchor_us  = __atomic_load_n(&g_bsp_tsc_anchor_us,  __ATOMIC_RELAXED);
        uint64_t bsp_anchor_khz = __atomic_load_n(&g_bsp_tsc_anchor_khz, __ATOMIC_RELAXED);
        uint64_t bsp_anchor     = __atomic_load_n(&g_bsp_tsc_anchor,     __ATOMIC_ACQUIRE);
        uint64_t now_us         = pit_get_uptime_us();   /* HPET-backed when present */

        if (bsp_anchor != 0 && bsp_anchor_khz != 0 && now_us > bsp_anchor_us) {
            /* expected_tsc = bsp_anchor + (elapsed_us × tsc_khz / 1000)
             *
             *   elapsed_us  is bounded by boot duration (< 60 s on real
             *               HW), tsc_khz < 10_000_000 → product fits
             *               uint64_t with > 18 bits to spare.
             *   /1000       converts µs × kHz → cycles. */
            uint64_t elapsed_us  = now_us - bsp_anchor_us;
            uint64_t expected    = bsp_anchor + (elapsed_us * bsp_anchor_khz / 1000ULL);
            uint64_t my_tsc      = rdtsc();
            uint64_t cur_adj     = rdmsr_pc(MSR_IA32_TSC_ADJUST);

            /* Rate-relative threshold: AP_TSC_SKEW_THRESHOLD_US wall-
             * clock µs converted to TSC cycles at the actual CPU rate.
             * Keeps tolerance physically meaningful from 1 GHz Bochs
             * to 5 GHz Xeon. */
            uint64_t threshold_cyc = (bsp_anchor_khz * AP_TSC_SKEW_THRESHOLD_US) / 1000ULL;
            if (threshold_cyc < 1000ULL) threshold_cyc = 1000ULL;  /* floor */

            int64_t skew     = (int64_t)(my_tsc - expected);
            int64_t abs_skew = skew < 0 ? -skew : skew;
            if ((uint64_t)abs_skew > threshold_cyc) {
                /* IA32_TSC_ADJUST adjusts visible TSC by the delta
                 * written — a positive skew (we read AHEAD of expected)
                 * needs ADJUST -= skew so a future rdtsc returns the
                 * synchronised value. */
                uint64_t new_adj = cur_adj - (uint64_t)skew;
                wrmsr_pc(MSR_IA32_TSC_ADJUST, new_adj);
                debug_printf("[PER_CORE] Core %u TSC skew %ld cycles "
                             "corrected via IA32_TSC_ADJUST (was 0x%lx → 0x%lx, threshold=%lu)\n",
                             core_index, (long)skew,
                             (unsigned long)cur_adj, (unsigned long)new_adj,
                             (unsigned long)threshold_cyc);
            }
        }
    }

    /* Activate kvmclock on this VCPU. KVM only updates the
     * pvclock_vcpu_time_info slot whose physical address was written
     * to MSR_KVM_SYSTEM_TIME_NEW on THAT VCPU — so each AP must wrmsr
     * locally. No-op on bare metal / non-KVM hosts. */
    pvclock_init_ap(core_index);

    // ---- SYSCALL MSRs + PerCpuData + KernelGSBASE ----
    per_core_setup_notify_msrs(pc);

    // ---- LAPIC ----
    lapic_enable();

    if (pc->is_kcore) {
        // K-Cores run kcore_run_loop() — HLT until an IPI_WAKE doorbell — and
        // never call schedule(). A periodic LAPIC timer would only wake them
        // ~100x/s to do nothing but EOI (see idt.c LAPIC_TIMER_VECTOR), burning
        // real power and heat on hardware. Keep the timer LVT masked.
        lapic_timer_stop();
    } else {
        // App Cores are preemptively scheduled by the LAPIC timer (calibrated
        // against the per-core TSC in lapic_timer_init).
        lapic_timer_init(LAPIC_TIMER_VECTOR, 100);
    }

    pc->initialized = true;

    kprintf("[PER_CORE] Core %u ready: GDT=0x%lx TSS=0x%lx timer=%s %s\n",
            core_index, (uint64_t)&pc->gdt, (uint64_t)&pc->tss,
            pc->is_kcore ? "masked" : "100Hz",
            pc->is_kcore ? "[K-Core]" : "[App Core]");
}

void per_core_set_kernel_rsp(uint64_t rsp) {
    if (!__atomic_load_n(&g_per_core_active, __ATOMIC_ACQUIRE)) {
        // Early boot: use static BSP TSS + PerCpuData
        tss_set_rsp0(rsp);
        notify_set_kernel_rsp(rsp);
        return;
    }

    uint8_t idx = amp_get_core_index();
    PerCoreData* pc = &g_per_core[idx];
    pc->tss.rsp0         = rsp;
    pc->notify.kernel_rsp = rsp;
    pc->kernel_stack_top  = rsp;
}
