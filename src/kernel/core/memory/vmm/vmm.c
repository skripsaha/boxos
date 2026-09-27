#include "vmm.h"
#include "pmm.h"
#include "memtag.h"
#include "klib.h"
#include "klib_logring.h"
#include "io.h"
#include "atomics.h"
#include "error.h"
#include "cpuid.h"
#include "process.h"
#include "cpu_caps_page.h"
#include "boxos_addresses.h"
#include "clockboard.h"
#include "amp.h"
#include "video.h"
#include "e820.h"
#include "touch.h"
#include "logbook.h"   /* TouchPublish for pku:fault:denied (Phase 2H) */
#include "fpu.h"     /* fpu_xsave_register_extension — XCR0 RMW + xsave area resize */
#include "cabin_layout.h"
#include "acpi.h"
#include "hypervisor.h"  /* hv_present, hv_vendor_name — LA57 dance gate */

static vmm_context_t *kernel_context = NULL;
static vmm_context_t *current_context = NULL;
static bool vmm_initialized = false;
static bool g_pull_map_active = false;
static char last_error[256] = {0};
static volatile vmm_stats_t global_stats = {0};

#define PCID_MAX 4095
#define PCID_KERNEL 0
#define CR3_NOFLUSH (1ULL << 63)

static bool g_pcid_active = false;
static uint16_t pcid_next = 1;
static uint16_t pcid_free_stack[PCID_MAX];
static size_t pcid_free_count = 0;
static spinlock_t pcid_lock = {0};

uint8_t vmm_maxphyaddr = 36;
uint64_t vmm_pte_addr_mask = 0x0000000FFFFFF000ULL;
/* Wider variant for TME-MK paths. Initialized to the same value as
 * vmm_pte_addr_mask at boot; widened by tme_init_bsp once num_keyid_bits
 * is known. See vmm_set_keyid_widening below. */
uint64_t vmm_pte_addr_mask_with_keyid = 0x0000000FFFFFF000ULL;

/* Which flag bits this machine may legally put into a paging-structure
 * entry. Starts WITHOUT bit 63, because bit 63 is a reserved bit until
 * IA32_EFER.NXE is 1 (Intel SDM Vol 3A §4.5) and an entry carrying a
 * reserved bit faults on the first access of any kind — read, write or
 * fetch — with RSVD set in the #PF error code.
 *
 * vmm_note_no_execute() opens the bit once the CPU module has taken NXE
 * up. Until then, and forever on a machine that has no NX, every
 * VMM_FLAG_NO_EXECUTE a caller asks for is dropped: the mapping loses its
 * hardening and stays a valid mapping, which is the only one of the two
 * outcomes a machine can boot through. */
uint64_t vmm_pte_flags_mask = VMM_PTE_FLAGS_MASK & ~VMM_FLAG_NO_EXECUTE;

void vmm_note_no_execute(bool usable)
{
    vmm_pte_flags_mask = usable
        ? VMM_PTE_FLAGS_MASK
        : (VMM_PTE_FLAGS_MASK & ~VMM_FLAG_NO_EXECUTE);
}

/* 5-level paging (LA57) runtime state — see vmm.h for the contract.
 * Defaults to 4-level; vmm_init upgrades to 5-level when the CPU advertises
 * CPUID.07H.0:ECX[16] AND the runtime LA57 transition trampoline succeeds. */
int  g_vmm_paging_levels = 4;
bool g_vmm_la57_active   = false;

/* Implemented in vmm_la57.asm — switches the calling CPU from 4-level to
 * 5-level paging with CR3 = pml5_phys. pml5_phys MUST be < 4 GB. */
extern void vmm_la57_runtime_enable(uint64_t pml5_phys);

/* Return the PML4 backing `ctx`. Under 4-level paging, ctx->pml4 IS the
 * PML4. Under 5-level, ctx->pml4 is the PML5 root and the kernel-shared
 * PML4 is reachable via PML5[511] (any kernel VA's PML5 index is 511 since
 * canonical sign-extension forces bits 56:48 = all 1 for kernel half).
 *
 * Returns NULL if the 5-level PML5[511] entry hasn't been populated yet —
 * caller in vmm_init populates it on first kernel mapping via the walker.
 *
 * Used by the direct-poke sites (Pull Map root install, ctx-copy loops in
 * create/destroy) that need an unambiguous PML4 handle rather than a walk
 * by VA. */
static inline page_table_t *vmm_kernel_pml4_of(vmm_context_t *ctx) {
    if (g_vmm_paging_levels == 4) {
        return ctx->pml4;
    }
    pte_t pml5_e = ctx->pml4->entries[511];
    if (!(pml5_e & VMM_FLAG_PRESENT)) return NULL;
    return (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pml5_e));
}

/* Walk PML5 → PML4 for `virt_addr` under 5-level paging. Returns the PML4
 * for `virt_addr`'s PML5 index slot. Under 4-level this is a no-op
 * returning ctx->pml4. With `create=true`, allocates a fresh PML4 + CAS-
 * publishes it when the PML5 entry is empty; returns NULL on alloc fail.
 *
 * CAS-publish handles the cross-core race the existing 4-level walker
 * already handles at PML4→PDPT level (vmm_get_or_create_table:1187+). */
static page_table_t *vmm_walk_pml5_to_pml4(vmm_context_t *ctx,
                                            uintptr_t virt_addr,
                                            bool create) {
    if (g_vmm_paging_levels == 4) {
        return ctx->pml4;
    }
    uint32_t pml5_idx = VMM_PML5_INDEX(virt_addr);
    pte_t *pml5_entry = &ctx->pml4->entries[pml5_idx];

    if (!(*pml5_entry & VMM_FLAG_PRESENT)) {
        if (!create) return NULL;
        uintptr_t new_pml4_phys = vmm_alloc_page_table();
        if (!new_pml4_phys) {
            vmm_set_error("Failed to allocate PML4 under PML5");
            return NULL;
        }
        pte_t new_entry = vmm_make_pte(new_pml4_phys,
                                       VMM_FLAGS_KERNEL_RW | VMM_FLAG_USER);
        pte_t expected = 0;
        bool  won = __atomic_compare_exchange_n(pml5_entry, &expected,
                                                 new_entry, false,
                                                 __ATOMIC_RELEASE,
                                                 __ATOMIC_ACQUIRE);
        if (!won) {
            pmm_free((void *)new_pml4_phys, 1);
        }
    }
    uintptr_t pml4_phys = vmm_pte_to_phys(*pml5_entry);
    return (page_table_t *)vmm_phys_to_virt(pml4_phys);
}

static uintptr_t kernel_heap_current = VMM_KERNEL_HEAP_BASE;
static spinlock_t kernel_heap_lock = {0};

static uintptr_t kernel_mmio_current = VMM_KERNEL_MMIO_BASE;

/* Kernel-MMIO VA free list — sorted by base, coalesced on insert.
 *
 * Without this, vmm_unmap_mmio leaked virtual address space forever:
 * every PCIe rebind, framebuffer mode-change, or AHCI re-init would
 * burn a fresh chunk of the 1 GiB VMM_KERNEL_MMIO_SIZE window. On a
 * real PC with hot-plug and DXE driver re-init the kernel would run
 * out of MMIO VA after hours. The free list rebuilds reclaimed ranges
 * into the same allocator so the workload is steady-state-bounded.
 *
 * `kernel_mmio_lock` protects this list AND `kernel_mmio_current` —
 * one lock for both so allocate/free races against the bump cursor
 * are linearised. Nodes themselves are kmalloc'd; vmm_unmap_mmio is
 * never called from IRQ context (only driver shutdown / hot-unplug),
 * so kmalloc is safe. */
typedef struct MmioFreeNode {
    uintptr_t base;          /* page-aligned, page-multiple */
    size_t    size;
    struct MmioFreeNode *next;
} MmioFreeNode;

static MmioFreeNode *kernel_mmio_free_head = NULL;
static spinlock_t kernel_mmio_lock = {0};

/* =========================================================================
 * PAT (Page Attribute Table) initialisation
 *
 * x86 IA32_PAT MSR (0x277) holds 8 one-byte memory-type entries (PA0–PA7).
 * Each PTE selects an entry via the three PAT selector bits:
 *   index = (PAT_bit << 2) | (PCD << 1) | PWT
 *
 * Hardware reset defaults (Intel SDM Vol.3 Table 11-10):
 *   PA0=WB(6)  PA1=WT(4)  PA2=UC-(7)  PA3=UC(0)
 *   PA4=WB(6)  PA5=WT(4)  PA6=UC-(7)  PA7=UC(0)
 *
 * We reprogram PA6 from UC-(7) to WC(1).  No other entry changes.
 * vmm_map_framebuffer then selects PA6 via: PCD=1, PAT_bit=1, PWT=0 → index 6.
 * vmm_map_mmio still selects PA3 via:       PCD=1, PWT=1,    PAT_bit=0 → index 3 = UC.
 * ========================================================================= */

#define MSR_IA32_PAT   0x277U

/* CR4.PGE — Intel SDM Vol 3A §2.5. Named here because vmm_pat_init toggles
 * it to satisfy §11.12.4's "including global entries"; the two shootdown
 * sites below spell the same bit as (1ULL << 7) inline. */
#define VMM_CR4_PGE_BIT (1ULL << 7)

#define PAT_TYPE_WB    0x06U
#define PAT_TYPE_WT    0x04U
#define PAT_TYPE_UCM   0x07U   /* UC- (weakly uncacheable) */
#define PAT_TYPE_UC    0x00U
#define PAT_TYPE_WC    0x01U   /* Write Combining            */
#define PAT_TYPE_WP    0x05U   /* Write Protected (reserved) */

/* IA32_PAT MSR value programmed by vmm_pat_init on BSP. Used by:
 *   - vmm_pte_cache_type — decode any PTE's leaf cache type via PAT index
 *   - MemTagVerifyPatMsr — AP-side RDMSR vs BSP value (Intel SDM Vol 3A
 *     §11.12.4: identical PAT required across coherent logical processors)
 * 0 means "PAT not programmed" (e.g. !has_pat). Otherwise it's the 64-bit
 * MSR pattern written via WRMSR — 8 one-byte entries (PA0..PA7). */
uint64_t g_ia32_pat_value = 0;

void vmm_pat_init(void)
{
    /* PAT MSR (IA32_PAT, 0x277) exists only when CPUID.1:EDX[16] = 1.
     * Every long-mode CPU ships with PAT since Pentium III, but a
     * WRMSR to a non-existent MSR raises #GP → triple-fault, so gate
     * defensively. has_pat is detected in cpu_detect_features() and
     * AND-intersected on every AP in cpu_intersect_features_ap(). */
    if (!g_cpu_caps.has_pat) {
        debug_printf("[VMM] PAT not supported by CPU — skipping IA32_PAT program\n");
        return;
    }

    uint64_t pat =
        ((uint64_t)PAT_TYPE_WB  <<  0) |  /* PA0 = WB  (unchanged) */
        ((uint64_t)PAT_TYPE_WT  <<  8) |  /* PA1 = WT  (unchanged) */
        ((uint64_t)PAT_TYPE_UCM << 16) |  /* PA2 = UC- (unchanged) */
        ((uint64_t)PAT_TYPE_UC  << 24) |  /* PA3 = UC  (unchanged, used by vmm_map_mmio) */
        ((uint64_t)PAT_TYPE_WB  << 32) |  /* PA4 = WB  (unchanged) */
        ((uint64_t)PAT_TYPE_WT  << 40) |  /* PA5 = WT  (unchanged) */
        ((uint64_t)PAT_TYPE_WC  << 48) |  /* PA6 = WC  ← was UC-; used by vmm_map_framebuffer */
        ((uint64_t)PAT_TYPE_UC  << 56);   /* PA7 = UC  (unchanged) */

    __asm__ volatile(
        "wrmsr"
        :
        : "c" (MSR_IA32_PAT),
          "a" ((uint32_t)(pat & 0xFFFFFFFFULL)),
          "d" ((uint32_t)(pat >> 32))
        :
    );

    /* Snapshot for AP-side consistency probe (MemTagVerifyPatMsr) and for
     * runtime PTE→cache-type decoding (vmm_pte_cache_type). Atomic store
     * with RELEASE so APs reading after their cpu_intersect_features_ap
     * see the BSP-programmed value. */
    __atomic_store_n(&g_ia32_pat_value, pat, __ATOMIC_RELEASE);

    /* TLB invalidation after the PAT change. Intel SDM Vol 3A §11.12.4
     * ("Programming the PAT") states the duty and then the sequence:
     * "It requires the OS to invalidate all affected TLB entries
     * (including global entries) and all entries in all paging-structure
     * caches. ... 1. If the PCIDE or PGE flag is set in CR4, flush TLBs by
     * clearing one of those flags (then restore the flag via a subsequent
     * CR4 write). Otherwise, flush TLBs by executing a MOV from control
     * register CR3 to another register and then a MOV from that register
     * back to CR3."
     *
     * Which branch this is, is not a choice. CR4.PGE is 1 on every core by
     * the time this runs — the BSP sets it in enable_fpu before vmm_init,
     * each AP in ap_trampoline.asm and again in enable_fpu before
     * per_core_init_ap calls this — and the Pull Map's RAM leaves all carry
     * VMM_FLAG_GLOBAL. A CR3 round trip is the "Otherwise" branch: §4.10.4.1
     * says MOV to CR3 "invalidates all TLB entries associated with PCID
     * 000H except those for global pages", so taking it with PGE set left
     * every global entry — which here is every kernel RAM translation —
     * holding its pre-PAT memory type, and the comment recorded that as
     * sufficient.
     *
     * PGE is the flag toggled and not PCIDE, on purpose: restoring PCIDE is
     * the half that can fail. §4.10.1: "MOV to CR4 causes a general-
     * protection exception (#GP) if it would change CR4.PCIDE from 0 to 1
     * and either IA32_EFER.LMA = 0 or CR3[11:0] ≠ 000H" — so a core
     * carrying a non-zero PCID could clear PCIDE and never get it back. Clearing CR4.PGE has no such restriction and is strictly the
     * bigger hammer: it drops global entries too, which is exactly what
     * §11.12.4 asks for. The instruction pair is the one pcid_alloc_safe and
     * vmm_tlb_shootdown_handler already use.
     *
     * The CR3 round trip stays as the branch the spec names for a core with
     * neither flag set. There, no entry is global (a page is global only
     * while CR4.PGE is 1) and PCID 000H is the only tagged set in use, so
     * the reload reaches everything this core holds.
     *
     * Step 2 of the sequence — CLFLUSH/WBINVD — is not performed and does
     * not need to be: it is scoped to "changes to memory-type mappings for
     * which cache self-snooping behavior would be problematic given the
     * existing mappings (e.g. changing a cache line's memory type from WB to
     * UC...)". This call changes PA6 alone, from UC- to WC, and nothing has
     * yet selected PA6: vmm_map_framebuffer and the EFI runtime mapper are
     * its only users and both run after vmm_init. */
    {
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        if (cr4 & VMM_CR4_PGE_BIT) {
            __asm__ volatile("mov %0, %%cr4"
                             : : "r"(cr4 & ~VMM_CR4_PGE_BIT) : "memory");
            __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
        } else {
            /* Clear the NOFLUSH bit (bit 63) before writing CR3 — see
             * vmm_flush_tlb for the full story. */
            uintptr_t cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            cr3 &= ~(1ULL << 63);
            __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
        }
    }

    debug_printf("[VMM] PAT MSR programmed: PA6=WC (framebuffer Write Combining enabled)\n");
}

/*
 * vmm_wc_pte_flags — the pair of bits that names write-combining in a 4 KiB
 * leaf, and what to say instead when this machine cannot be asked.
 *
 * Bit 7 of a 4 KiB entry is the PAT selector only where CPUID.1:EDX[16]
 * reports PAT. Everywhere else it is a RESERVED bit, and an entry carrying
 * a reserved bit faults on the first access of any kind — read, write or
 * fetch — with RSVD in the #PF error code (Intel SDM Vol 3A §4.5). So the
 * callers that want WC used to be betting on a CPU feature nobody checked:
 * vmm_pat_init returns having done nothing when !has_pat, and the two WC
 * call sites composed bit 7 regardless.
 *
 * The proof asked for here is the MSR value vmm_pat_init publishes. It is
 * non-zero only after the WRMSR that puts write-combining in PA6, so it
 * answers both halves of the question at once: PAT exists, and entry 6
 * means WC. Without it the honest answer is PCD|PWT — PAT index 3, which
 * is strong UC under the programmed PAT and under the x86 reset-default
 * PAT alike. Slower than write-combining, and a mapping the machine can
 * actually use.
 *
 * This is NOT a repair of an ordering problem; there is none. vmm_pat_init
 * runs inside vmm_init, and both callers — the GOP framebuffer mapper and
 * the EFI runtime mapper — run after vmm_init on every boot path. What is
 * left is the machine, or the hypervisor, that masks PAT away entirely.
 */
uint64_t vmm_wc_pte_flags(void)
{
    if (__atomic_load_n(&g_ia32_pat_value, __ATOMIC_ACQUIRE) != 0) {
        return VMM_FLAG_CACHE_DISABLE | VMM_FLAG_PAT_BIT;   /* PA6 = WC */
    }

    /* kprintf, not debug_printf: a build that ships compiles debug_printf
     * into nothing, and a silent demotion from write-combining to strong
     * uncacheable is exactly the kind of thing an operator reports as "the
     * screen got slow" with no way to learn why. Said once — the EFI
     * runtime mapper asks per descriptor. */
    static bool said_no_pat = false;
    if (!__atomic_exchange_n(&said_no_pat, true, __ATOMIC_RELAXED)) {
        kprintf("[VMM] this machine reports no PAT — write-combining mappings "
                "fall back to strong uncacheable (PCD|PWT = PAT entry 3). "
                "Bit 7 of a 4 KiB entry is a reserved bit without PAT, and an "
                "entry carrying it faults on first touch.\n");
    }
    return VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH;  /* PA3 = UC */
}

/*
 * vmm_pte_pat_index — extract the 3-bit PAT selector from a leaf PTE.
 *
 * 4 KiB leaf (level 1): selectors are PWT (bit 3), PCD (bit 4), PAT (bit 7).
 * 2 MiB / 1 GiB leaf  : PWT (bit 3), PCD (bit 4), PAT (bit 12) — because
 *                       bit 7 is PS (Page Size) on the parent PDE/PDPT.
 * Intel SDM Vol 3A §11.12.3 + Tables 4-19/4-21.
 *
 * Returns 0..7; the caller decodes via IA32_PAT MSR (g_ia32_pat_value).
 */
uint8_t vmm_pte_pat_index(uint64_t pte_val, bool is_huge_leaf)
{
    uint8_t idx = 0;
    if (pte_val & VMM_FLAG_WRITE_THROUGH) idx |= 1u;
    if (pte_val & VMM_FLAG_CACHE_DISABLE) idx |= 2u;
    if (is_huge_leaf) {
        if (pte_val & (1ULL << 12))       idx |= 4u;
    } else {
        if (pte_val & VMM_FLAG_PAT_BIT)   idx |= 4u;
    }
    return idx;
}

/*
 * vmm_pat_type_at — decode the memory type stored at PAT entry `pat_idx`
 * inside the live IA32_PAT MSR snapshot (g_ia32_pat_value).
 *
 * Returns one of:
 *   0x00 PAT_TYPE_UC   — Strong Uncached
 *   0x01 PAT_TYPE_WC   — Write Combining
 *   0x04 PAT_TYPE_WT   — Write Through
 *   0x05 PAT_TYPE_WP   — Write Protected
 *   0x06 PAT_TYPE_WB   — Write Back
 *   0x07 PAT_TYPE_UCM  — Uncached, weakly ordered
 *   0xFF                — PAT not yet initialized (vmm_pat_init not run)
 *
 * Other values 0x02/0x03 are RESERVED per Intel SDM Vol 3A Table 11-10
 * and SHOULD NOT appear; returned as-is if seen so callers can detect.
 */
uint8_t vmm_pat_type_at(uint8_t pat_idx)
{
    uint64_t pat = __atomic_load_n(&g_ia32_pat_value, __ATOMIC_ACQUIRE);
    if (pat == 0) return 0xFFu;
    return (uint8_t)((pat >> (pat_idx * 8u)) & 0x07u);
}

/*
 * vmm_pte_cache_type_str — convenience: PTE → "cache:wb"/etc string.
 *
 * Composes vmm_pte_pat_index + vmm_pat_type_at into a stable string tag
 * suitable for MemTagApply. Used by debug tools / userspace introspection.
 * Returns "cache:unknown" when PAT isn't initialized.
 */
const char *vmm_pte_cache_type_str(uint64_t pte_val, bool is_huge_leaf)
{
    uint8_t pat_idx = vmm_pte_pat_index(pte_val, is_huge_leaf);
    uint8_t type    = vmm_pat_type_at(pat_idx);
    switch (type) {
        case PAT_TYPE_UC:  return "cache:uc";
        case PAT_TYPE_WC:  return "cache:wc";
        case PAT_TYPE_WT:  return "cache:wt";
        case PAT_TYPE_WP:  return "cache:wp";
        case PAT_TYPE_WB:  return "cache:wb";
        case PAT_TYPE_UCM: return "cache:uc-";
        case 0xFFu:        return "cache:unknown";
        default:           return "cache:reserved";
    }
}

/*
 * vmm_get_pat_msr_value — read the BSP-programmed IA32_PAT snapshot.
 * 0 means PAT not initialized. AP-side consistency probe compares its
 * own RDMSR to this value.
 */
uint64_t vmm_get_pat_msr_value(void)
{
    return __atomic_load_n(&g_ia32_pat_value, __ATOMIC_ACQUIRE);
}

/* ─── Phase 2E — PAT consistency + MTRR audit (CPU probes) ────────────
 *
 * These were originally in memtag.c with a MemTag* prefix, but they
 * have ZERO MemTag-specific state — they're pure CPU MSR probes that
 * happen to inform MemTag's cache:* tag decisions. Moved here in the
 * post-Phase-2K audit pass so they sit alongside vmm_pat_init / the
 * other VMM MSR helpers. */

#define VMM_MSR_IA32_MTRRCAP        0xFEU
#define VMM_MSR_MTRR_DEF_TYPE       0x2FFU
#define VMM_MSR_MTRR_PHYSBASE0      0x200U
#define VMM_MSR_MTRR_PHYSMASK0      0x201U

static inline uint64_t vmm_rdmsr_local(uint32_t msr_id) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr_id));
    return ((uint64_t)hi << 32) | lo;
}

bool vmm_verify_pat_msr(void) {
    if (!g_cpu_caps.has_pat) {
        debug_printf("[VMM] PAT MSR verify SKIP: CPU lacks PAT\n");
        return true;
    }
    uint64_t bsp = vmm_get_pat_msr_value();
    if (bsp == 0) {
        debug_printf("[VMM] PAT MSR verify SKIP: BSP snapshot not captured\n");
        return true;
    }
    uint64_t local = vmm_rdmsr_local(MSR_IA32_PAT);
    if (local != bsp) {
        debug_printf("[VMM] PAT MSR ABORT: local=0x%016lx bsp=0x%016lx "
                     "— Intel SDM Vol 3A §11.12.4 violation (heterogeneous "
                     "PAT across coherent CPUs)\n",
                     (unsigned long)local, (unsigned long)bsp);
        return false;
    }
    debug_printf("[VMM] PAT MSR verify OK: 0x%016lx (matches BSP)\n",
                 (unsigned long)local);
    return true;
}

static const char *vmm_mtrr_type_str(uint8_t t) {
    switch (t) {
        case 0x00: return "UC";
        case 0x01: return "WC";
        case 0x04: return "WT";
        case 0x05: return "WP";
        case 0x06: return "WB";
        default:   return NULL;
    }
}

/* Does THIS logical processor implement MTRRs at all?
 *
 * Intel SDM Vol 3A §11.11.1 "MTRR Feature Identification": "The
 * availability of the MTRR feature is model-specific. Software can
 * determine if MTRRs are supported on a processor by executing the CPUID
 * instruction and reading the state of the MTRR flag (bit 12) in the
 * feature information register (EDX). If the MTRR flag is set (indicating
 * that the processor implements MTRRs), additional information about MTRRs
 * can be obtained from the 64-bit IA32_MTRRCAP MSR."
 *
 * Bit 12, therefore — not bit 16. The gate that stood here was
 * g_cpu_caps.has_pat, i.e. CPUID.01H:EDX[16], which answers whether the
 * page-attribute table exists and says nothing about IA32_MTRRCAP,
 * IA32_MTRR_DEF_TYPE or the PHYSBASE/PHYSMASK pairs this function reads.
 * Every other MSR probe in this kernel is gated on its own CPUID bit —
 * 0xCF on EDX[30], 0xE1 on WAITPKG, 0x981/0x982 on ECX[13], 0x87 on the
 * CPUID.21H string — and this was the one outlier.
 *
 * Asked of the running core with CPUID rather than of g_cpu_caps, because
 * cpu_capabilities_t carries no MTRR field and the MSRs below are this
 * core's own; a hybrid part answers for the core that is reading them. */
static bool vmm_cpu_has_mtrr(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(CPUID_LEAF_FEATURES, &eax, &ebx, &ecx, &edx);
    return ((edx >> 12) & 1u) != 0;
}

void vmm_dump_mtrr_layout(void) {
    if (!vmm_cpu_has_mtrr()) {
        /* kprintf, like the two verdicts below: the alternative used to be
         * reading MSRs this CPU does not implement and then telling the
         * operator "MTRRs are DISABLED" on a part that has none to disable.
         * A refusal said out loud is the honest replacement for a verdict
         * this kernel has no way to reach. */
        kprintf("[VMM] MTRR audit SKIP: CPUID.01H:EDX[12] is clear — this "
                "processor implements no MTRRs (Intel SDM §11.11.1), so "
                "nothing here can say what effective memory type firmware "
                "gave any span.\n");
        return;
    }
    uint64_t cap = vmm_rdmsr_local(VMM_MSR_IA32_MTRRCAP);
    uint64_t def = vmm_rdmsr_local(VMM_MSR_MTRR_DEF_TYPE);

    uint8_t  vcnt    = (uint8_t)(cap & 0xFFu);
    bool     fix_sup = (cap >> 8) & 1u;
    bool     wc_sup  = (cap >> 10) & 1u;
    bool     smrr    = (cap >> 11) & 1u;
    uint8_t  def_typ = (uint8_t)(def & 0xFFu);
    bool     fix_en  = (def >> 10) & 1u;
    bool     mtrr_en = (def >> 11) & 1u;
    const char *def_str = vmm_mtrr_type_str(def_typ);

    debug_printf("[VMM] MTRR audit: cap=0x%016lx def=0x%016lx VCNT=%u "
                 "FIX_sup=%d WC_sup=%d SMRR=%d FIX_en=%d MTRR_en=%d "
                 "DEF_TYPE=%s(0x%02x)\n",
                 (unsigned long)cap, (unsigned long)def,
                 (unsigned)vcnt, (int)fix_sup, (int)wc_sup, (int)smrr,
                 (int)fix_en, (int)mtrr_en,
                 def_str ? def_str : "RESERVED", (unsigned)def_typ);

    /* The layout above is routine and stays in the debug channel. The two
     * verdicts below are not routine: they are this kernel saying that the
     * ground it stands on is not the ground it assumed.
     *
     * BoxOS does not program MTRRs — firmware does, and the Pull Map now
     * marks its own leaves honestly so no device range depends on firmware
     * to be uncacheable. But the effective type of a page is the MTRR type
     * combined with the PAT type, and the rule for that is Table 11-7 in
     * Intel SDM Vol 3A §11.5.2.2 "Selecting Memory Types for Pentium III and
     * More Recent Processor Families". (§11.12.5, cited here before, is "PAT
     * Compatibility with Earlier IA-32 Processors" — a different subject
     * entirely.)
     *
     * What Table 11-7 gives, said the way it actually reads rather than as
     * "combining only ever moves DOWN":
     *   - for a PAT-WB page, which is every RAM leaf of the Pull Map (PAT
     *     entry 0), the effective type IS the MTRR type, in every row. So a
     *     disabled or non-WB MTRR default does demote all of them, and that
     *     is the whole of what the two verdicts below are about;
     *   - for a PAT-UC page, which is every device leaf here (PCD|PWT, PAT
     *     entry 3), the effective type is UC under every MTRR type — the
     *     uncacheability this kernel asks for cannot be taken away;
     *   - for a PAT-WC page the effective type is WC under EVERY MTRR type,
     *     UC included. That is the one column where the combination is
     *     weaker than the MTRR type, and it is the column the GOP
     *     framebuffer stands in: firmware that leaves the aperture MTRR-UC
     *     does NOT cost vmm_map_framebuffer its write-combining.
     * The old sentence said the opposite of that last row, in a comment and
     * in an operator-visible line, and would have sent someone hunting an
     * MTRR cause for a slow console that has none.
     *
     * kprintf, because debug_printf compiles to nothing in the builds that
     * ship. */
    if (!mtrr_en) {
        kprintf("[VMM] MTRRs are DISABLED (MTRR_DEF_TYPE.E=0) — \"all MTRRs "
                "are disabled ... and the UC memory type is applied to all of "
                "physical memory\" (Intel SDM §11.11.2.1). Every write-back "
                "leaf this kernel asked for is uncacheable; only a PAT-WC "
                "page keeps its type, which Table 11-7 gives WC even under "
                "an MTRR type of UC. Correct, and as slow as the machine "
                "gets.\n");
    }
    if (def_typ != 0x06u && mtrr_en) {
        kprintf("[VMM] MTRR default type is %s, not write-back — a span no "
                "variable MTRR names takes that type combined with its PAT "
                "entry (Intel SDM §11.5.2.2, Table 11-7), and for a "
                "write-back page the combination IS the MTRR type, so every "
                "such leaf is %s. A PAT-WC page is the exception Table 11-7 "
                "makes: WC under every MTRR type, so the framebuffer keeps "
                "write-combining.\n",
                def_str ? def_str : "RESERVED",
                def_str ? def_str : "RESERVED");
    }

    uint32_t walk_n = vcnt > 8 ? 8 : vcnt;
    for (uint32_t i = 0; i < walk_n; i++) {
        uint64_t base = vmm_rdmsr_local(VMM_MSR_MTRR_PHYSBASE0 + i * 2);
        uint64_t mask = vmm_rdmsr_local(VMM_MSR_MTRR_PHYSMASK0 + i * 2);
        if (!((mask >> 11) & 1u)) continue;
        uint8_t  ty   = (uint8_t)(base & 0xFFu);
        uint64_t phys = base & ~0xFFFULL;
        uint64_t mphys = mask & ~0xFFFULL;
        const char *ts = vmm_mtrr_type_str(ty);
        debug_printf("[VMM]   MTRR var[%u]: base=0x%016lx mask=0x%016lx "
                     "type=%s(0x%02x)\n",
                     i, (unsigned long)phys, (unsigned long)mphys,
                     ts ? ts : "RESERVED", (unsigned)ty);
    }
}

/* ─── PTE bits 52-58 metadata-bit availability probe ──────────────── */

bool vmm_verify_pte_metadata_bits_52_58(void) {
    if (vmm_maxphyaddr > 52) {
        debug_printf("[VMM] PTE-metadata-52-58 ABORT: MAXPHYADDR=%u > 52 — "
                     "bits 52-58 are phys, NOT ignored\n",
                     (unsigned)vmm_maxphyaddr);
        return false;
    }

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    bool cr4_pke = ((cr4 >> 22) & 1u) != 0;
    bool cr4_pks = ((cr4 >> 24) & 1u) != 0;
    bool cr4_cet = ((cr4 >> 23) & 1u) != 0;

    /* PKU (bits 62:59) lives OUTSIDE bits 52-58, and CET takes no leaf
     * bit at all — a shadow stack is R/W=0 + Dirty=1, bits 1 and 6.
     * Logged for telemetry — never causes ABORT on current silicon. */
    debug_printf("[VMM] PTE-metadata-52-58 probe: MAXPHYADDR=%u "
                 "CR4.PKE=%d CR4.PKS=%d CR4.CET=%d → bits 52-58 SAFE\n",
                 (unsigned)vmm_maxphyaddr,
                 (int)cr4_pke, (int)cr4_pks, (int)cr4_cet);
    return true;
}

/* ─── Phase 2H — PKU / PKS bring-up ───────────────────────────────── */

#define VMM_CR4_PKE_BIT   (1ULL << 22)   /* CR4.PKE — Intel SDM Vol 3A §2.5 */
#define VMM_CR4_PKS_BIT   (1ULL << 24)   /* CR4.PKS */
#define VMM_XCR0_PKRU_BIT (1ULL << 9)    /* XSAVE component 9 */
#define VMM_MSR_PKRU      0x6E0U
#define VMM_MSR_PKRS      0x6E1U

/* Pre-resolved Touch handle for the #PF.PK publish path. Resolved in
 * vmm_pku_init (outside IRQ context). #PF runs with IF=0 + may hold
 * locks from the faulting code path; calling TouchPublish directly
 * would risk the same deadlock class documented in touch.h §IRQ-
 * Publishers and mitigated in mce.c via TouchPublishIrqPair. */
static TouchTag g_vmm_tag_pku_fault = TOUCH_TAG_INVALID;

static inline void vmm_pku_program_cr4(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    uint64_t want = cr4;
    if (g_cpu_caps.has_pku) want |= VMM_CR4_PKE_BIT;
    if (g_cpu_caps.has_pks) want |= VMM_CR4_PKS_BIT;
    if (want != cr4) {
        __asm__ volatile("mov %0, %%cr4" : : "r"(want) : "memory");
    }
}

static inline void vmm_pku_program_xcr0(void) {
    if (!g_cpu_caps.has_xsave || !g_cpu_caps.has_pku) return;
    /* SDM Vol 1 §13.3: XCR0 bit 9 = PKRU state. fpu_xsave_register_
     * extension does the RMW on XCR0 *and* updates g_xsave_mask +
     * g_xsave_area_size so the existing FPU context-switch path
     * automatically saves/restores PKRU per-thread. */
    (void)fpu_xsave_register_extension(VMM_XCR0_PKRU_BIT, "PKRU");
}

void vmm_pku_init(void) {
    if (!g_cpu_caps.has_pku && !g_cpu_caps.has_pks) {
        debug_printf("[VMM] PKU/PKS not supported — Phase 2H dormant\n");
        return;
    }
    vmm_pku_program_cr4();
    vmm_pku_program_xcr0();
    /* Cache Touch handle for IRQ-safe publish from PF.PK path. */
    g_vmm_tag_pku_fault = TouchLogbookIntern("pku:fault:denied");
    debug_printf("[VMM] PKU/PKS BSP init: PKU=%d PKS=%d XCR0.PKRU=%d "
                 "fault_tag=0x%x (default PKRU=0 → all keys allowed; "
                 "per-process PKRU lifecycle = XSAVE/XRSTOR via "
                 "XCR0.PKRU bit 9)\n",
                 (int)g_cpu_caps.has_pku,
                 (int)g_cpu_caps.has_pks,
                 (int)((g_cpu_caps.has_pku && g_cpu_caps.has_xsave) ? 1 : 0),
                 (unsigned)g_vmm_tag_pku_fault);
}

void vmm_pku_ap_init(void) {
    if (!g_cpu_caps.has_pku && !g_cpu_caps.has_pks) return;
    vmm_pku_program_cr4();
    vmm_pku_program_xcr0();
}

/*
 * PKRU is NOT an MSR — Intel SDM Vol 1 §18.2 defines RDPKRU (opcode
 * 0F 01 EE) and WRPKRU (0F 01 EF) as dedicated instructions. Using
 * RDMSR/WRMSR on 0x6E0 #GPs. We emit raw opcodes so we work with
 * assemblers that don't yet know the mnemonics. Both require CR4.PKE=1
 * (we set it in vmm_pku_init) and ECX=0; WRPKRU additionally needs
 * EDX=0.
 *
 * PKS (supervisor variant), in contrast, IS a real MSR (IA32_PKRS,
 * 0x6E1) — but Phase 2H doesn't yet drive it; reserved here for
 * symmetry with the userspace helpers.
 */
uint32_t vmm_read_pkru(void) {
    if (!g_cpu_caps.has_pku) return 0;
    uint32_t pkru;
    __asm__ volatile(".byte 0x0f, 0x01, 0xee"
                     : "=a"(pkru)
                     : "c"(0u)
                     : "edx");
    return pkru;
}

void vmm_write_pkru(uint32_t value) {
    if (!g_cpu_caps.has_pku) return;
    __asm__ volatile(".byte 0x0f, 0x01, 0xef"
                     :
                     : "a"(value), "c"(0u), "d"(0u));
}

/* ─── Phase 2I — LAM probe ─────────────────────────────────────────── */

static void vmm_lam_probe_inner(const char *who) {
    if (!g_cpu_caps.has_lam) {
        debug_printf("[VMM/%s] LAM not supported — Phase 2I dormant\n", who);
        return;
    }
    uint64_t cr3, cr4;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    bool lam_u48 = (cr3 & VMM_CR3_LAM_U48) != 0;
    bool lam_u57 = (cr3 & VMM_CR3_LAM_U57) != 0;
    bool lam_sup = (cr4 & VMM_CR4_LAM_SUP) != 0;
    debug_printf("[VMM/%s] LAM probe: CR3.LAM_U48=%d CR3.LAM_U57=%d "
                 "CR4.LAM_SUP=%d → user-side dormant by default "
                 "(per-process opt-in pending)\n",
                 who, (int)lam_u48, (int)lam_u57, (int)lam_sup);
}

void vmm_lam_probe(void)    { vmm_lam_probe_inner("BSP"); }
void vmm_lam_ap_probe(void) { vmm_lam_probe_inner("AP"); }

/* ─── Phase 2J — TME / TME-MK probe ────────────────────────────────── */

static void vmm_tme_probe_inner(const char *who) {
    if (!g_cpu_caps.has_tme) {
        debug_printf("[VMM/%s] TME not supported — Phase 2J dormant\n", who);
        return;
    }
    /* MSRs gated by CPUID.07H.0:ECX[13]. Reading without that bit
     * would #GP; the early-return above protects every call site. */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi)
                     : "c"(VMM_MSR_IA32_TME_CAPABILITY));
    uint64_t cap = ((uint64_t)hi << 32) | lo;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi)
                     : "c"(VMM_MSR_IA32_TME_ACTIVATE));
    uint64_t act = ((uint64_t)hi << 32) | lo;

    bool   locked       = (act & VMM_TME_ACT_LOCK)        != 0;
    bool   tme_enabled  = (act & VMM_TME_ACT_TME_EN)      != 0;
    /* TME-MK is active iff LOCK + TME_EN + MK_TME_KEYID_BITS != 0 — the
     * conditions PCONFIG itself enforces. There is no enable bit. */
    bool   mk_enabled   = vmm_tme_act_mk_active(act);
    uint8_t num_keyid_bits = (uint8_t)((act & VMM_TME_ACT_KEYID_BITS_MASK)
                                       >> VMM_TME_ACT_KEYID_BITS_SHIFT);
    uint16_t crypto_algs = (uint16_t)((act & VMM_TME_ACT_ALGS_MASK)
                                      >> VMM_TME_ACT_ALGS_SHIFT);

    debug_printf("[VMM/%s] TME probe: cap=0x%016lx act=0x%016lx "
                 "lock=%d TME_EN=%d MK_active=%d num_keyid_bits=%u "
                 "crypto_algs=0x%04x "
                 "MAXPHYADDR=%u → KeyID window bits %u:%u\n",
                 who,
                 (unsigned long)cap, (unsigned long)act,
                 (int)locked, (int)tme_enabled, (int)mk_enabled,
                 (unsigned)num_keyid_bits,
                 (unsigned)crypto_algs,
                 (unsigned)vmm_maxphyaddr,
                 num_keyid_bits > 0
                     ? (unsigned)(vmm_maxphyaddr - 1)
                     : 0u,
                 num_keyid_bits > 0
                     ? (unsigned)(vmm_maxphyaddr - num_keyid_bits)
                     : 0u);
}

void vmm_tme_probe(void)    { vmm_tme_probe_inner("BSP"); }
void vmm_tme_ap_probe(void) { vmm_tme_probe_inner("AP"); }

/* ─── Phase 2K — CET probe ─────────────────────────────────────────── */

/* Pre-resolved Touch tag for #CP (vector 21) handler. Resolved in
 * vmm_cet_probe (BSP, outside IRQ context) so idt.c's #CP handler
 * does NOT have to call TouchLogbookIntern lazily (which takes registry
 * locks — touch.h:230 forbids from IRQ context). Exposed via getter
 * so idt.c stays IRQ-safe. */
static TouchTag g_vmm_tag_cet_cp_fault = TOUCH_TAG_INVALID;
uint16_t vmm_get_cet_cp_tag(void) { return (uint16_t)g_vmm_tag_cet_cp_fault; }

static void vmm_cet_probe_inner(const char *who) {
    if (!g_cpu_caps.has_shstk && !g_cpu_caps.has_ibt) {
        debug_printf("[VMM/%s] CET not supported (no SHSTK/IBT) — Phase 2K dormant\n", who);
        return;
    }
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    bool cr4_cet = (cr4 & VMM_CR4_CET_BIT) != 0;

    /* MSRs only readable when CR4.CET=1. Firmware may not have enabled
     * it — gate the RDMSR. */
    uint64_t s_cet = 0, u_cet = 0;
    if (cr4_cet) {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi)
                         : "c"(VMM_MSR_IA32_S_CET));
        s_cet = ((uint64_t)hi << 32) | lo;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi)
                         : "c"(VMM_MSR_IA32_U_CET));
        u_cet = ((uint64_t)hi << 32) | lo;
    }

    debug_printf("[VMM/%s] CET probe: SHSTK=%d IBT=%d CR4.CET=%d "
                 "IA32_S_CET=0x%016lx IA32_U_CET=0x%016lx → per-process "
                 "lifecycle pending\n",
                 who,
                 (int)g_cpu_caps.has_shstk,
                 (int)g_cpu_caps.has_ibt,
                 (int)cr4_cet,
                 (unsigned long)s_cet,
                 (unsigned long)u_cet);
}

void vmm_cet_probe(void) {
    vmm_cet_probe_inner("BSP");
    /* Pre-resolve #CP Touch tag for IRQ-safe publish from idt.c
     * vector-21 handler. Done here (BSP, after MemTagInit so the
     * cet:* reserved tags are interned) rather than at lazy first
     * #CP fire — registry locks make TouchLogbookIntern non-IRQ-safe. */
    g_vmm_tag_cet_cp_fault = TouchLogbookIntern("cet:fault:cp");
    debug_printf("[VMM] CET Touch handle cached: cet:fault:cp=0x%x\n",
                 (unsigned)g_vmm_tag_cet_cp_fault);
}
void vmm_cet_ap_probe(void) { vmm_cet_probe_inner("AP"); }

/* ─── Shadow-stack pages (Intel SDM Vol 3A §4.6) ──────────────────────
 *
 * The quotation and the rules are in vmm.h next to
 * VMM_FLAGS_SHADOW_STACK_SUPV. These two functions are the only place in
 * the kernel that puts the R/W=0 + Dirty=1 combination onto a leaf.
 */

/* §4.6 condition (2): "the R/W flag is 1 in every other paging-structure
 * entry controlling the translation of the linear address". A read-only
 * parent makes the address an ordinary read-only address no matter what
 * the leaf says, and the first shadow-stack access to it is a #PF with
 * the SS error bit set — a fault with no obvious cause. Every parent
 * this VMM creates is writable (vmm_get_or_create_table and
 * vmm_demote_large_entry both publish VMM_FLAGS_KERNEL_RW | USER, and
 * the Pull Map's own PDPT/PD entries are PRESENT | WRITABLE), so this
 * walk is expected to pass; it exists so that the day it does not, the
 * caller gets a refusal instead of a shadow stack that is not one.
 *
 * Stops at whichever level holds the leaf, because the leaf is the one
 * entry condition (2) does NOT apply to. */
static bool vmm_shadow_stack_parents_writable(vmm_context_t *ctx,
                                              uintptr_t virt_addr)
{
    if (!ctx || !ctx->pml4)
        return false;

    /* Under 5-level paging the PML5 entry is a parent too, and
     * vmm_walk_pml5_to_pml4 hides it — check it before descending. */
    if (g_vmm_paging_levels == 5)
    {
        pte_t pml5e = ctx->pml4->entries[VMM_PML5_INDEX(virt_addr)];
        if (!(pml5e & VMM_FLAG_PRESENT) || !(pml5e & VMM_FLAG_WRITABLE))
            return false;
    }

    page_table_t *pml4 = vmm_walk_pml5_to_pml4(ctx, virt_addr, false);
    if (!pml4)
        return false;

    pte_t pml4e = pml4->entries[VMM_PML4_INDEX(virt_addr)];
    if (!(pml4e & VMM_FLAG_PRESENT) || !(pml4e & VMM_FLAG_WRITABLE))
        return false;

    page_table_t *pdpt =
        (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pml4e));
    pte_t pdpte = pdpt->entries[VMM_PDPT_INDEX(virt_addr)];
    if (!(pdpte & VMM_FLAG_PRESENT))
        return false;
    if (pdpte & VMM_FLAG_LARGE_PAGE)
        return true;                     /* 1 GiB leaf — no parent left */
    if (!(pdpte & VMM_FLAG_WRITABLE))
        return false;

    page_table_t *pd =
        (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pdpte));
    pte_t pde = pd->entries[VMM_PD_INDEX(virt_addr)];
    if (!(pde & VMM_FLAG_PRESENT))
        return false;
    if (pde & VMM_FLAG_LARGE_PAGE)
        return true;                     /* 2 MiB leaf — no parent left */
    return (pde & VMM_FLAG_WRITABLE) != 0;
}

bool vmm_shadow_stack_seal(vmm_context_t *ctx, uintptr_t virt_addr)
{
    if (!ctx || !vmm_is_page_aligned(virt_addr))
        return false;

    bool changed = false;

    /* ctx->lock across the walk and the leaf write, released before the
     * shootdown — the same discipline, in the same order, as
     * vmm_map_page. The walk is not a read: it can allocate a page table
     * and it can demote a huge leaf, so two cores sealing two pages of
     * the same 1 GiB Pull Map leaf meet inside vmm_get_or_create_table.
     * Holding a lock across an IPI broadcast is what vmm_map_page
     * avoids, and so does this. */
    spin_lock(&ctx->lock);

    /* A 4 KiB leaf, always. The encoding is per page and the Pull Map
     * hands out 1 GiB and 2 MiB leaves for kernel windows; going through
     * vmm_get_or_create_pte splits whichever huge leaf covers this
     * address (vmm_demote_large_entry) so this one page can carry a
     * permission its neighbours must not.
     *
     * That split is a cost, and it is why the shadow stacks that come
     * and go with processes do not live in the Pull Map at all but in
     * the bunk row below, which has no huge leaf to split. On a bunk
     * address this walk finds the PT entry the map already made. */
    pte_t *pte = vmm_get_or_create_pte(ctx, virt_addr);
    if (!pte)
    {
        spin_unlock(&ctx->lock);
        return false;
    }

    if (!(__atomic_load_n(pte, __ATOMIC_ACQUIRE) & VMM_FLAG_PRESENT))
    {
        spin_unlock(&ctx->lock);
        return false;
    }

    /* §4.6 condition (2) is checked BEFORE the leaf is touched, not
     * after. A refusal has to leave the page exactly as it was found:
     * every caller hands the frame straight back to the PMM, and a frame
     * that goes back with R/W = 0 in the direct map faults the next
     * pmm_alloc_zero inside the allocator's own memset. Checking first
     * means no failure remains after the CAS, so there is no rollback
     * that can be got wrong. The check runs after the walk because the
     * walk is what demotes the huge leaf, and the demotion is what
     * publishes the PD/PT parents this condition is about. */
    if (!vmm_shadow_stack_parents_writable(ctx, virt_addr))
    {
        spin_unlock(&ctx->lock);
        return false;
    }

    pte_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    for (;;)
    {
        pte_t sealed = (old | VMM_FLAG_DIRTY) & ~VMM_FLAG_WRITABLE;
        if (sealed == old)
            break;                       /* already sealed */
        if (__atomic_compare_exchange_n(pte, &old, sealed, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            changed = true;
            break;
        }
        if (!(old & VMM_FLAG_PRESENT))   /* entry vanished under us */
        {
            spin_unlock(&ctx->lock);
            return false;
        }
        /* old refreshed by the failed CAS — retry. */
    }

    spin_unlock(&ctx->lock);

    /* Cross-core, because this is a permission DOWNGRADE on a mapping
     * other cores share. Supervisor shadow stacks live in the Pull Map,
     * whose leaves are PRESENT | WRITABLE | GLOBAL and whose PML4 entry
     * is copied by value into every cabin context; a GLOBAL entry
     * survives a CR3 load, so a sibling keeps its writable translation
     * until something invalidates it. A core translating through that
     * stale entry does not see a shadow-stack address at all — §4.6
     * condition (1) is read from the entry, and the cached copy still
     * says R/W = 1 — so its shadow-stack access to the page faults and
     * an ordinary kernel store to it succeeds. The caller that needs it
     * is any future one that seals a Pull Map page after the machine is
     * multicore; the sealers in the tree today are the per-core
     * supervisor stacks, which seal during their own core's bring-up,
     * and two verification calls that change nothing (see below).
     *
     * The INVLPG of this 4 KiB address also drops the stale 1 GiB /
     * 2 MiB entry covering it, which is the other half of what the
     * demotion in the walk above needs: vmm_demote_large_entry
     * deliberately leaves the cross-core flush of the changed leaf to
     * the caller that changed it, and this is that caller.
     *
     * On the AP bring-up path this call sends no IPI and cannot block:
     * g_amp.multicore_active is set only at the very end of
     * amp_boot_aps, and vmm_shootdown_pages answers that with a plain
     * local invlpg. Cores already running then do keep a stale writable
     * entry for the pages sealed there — and that is harmless for
     * exactly those pages, because a core's own PL0 / IST stacks are
     * named by that core's MSRs alone, so no sibling ever makes a
     * shadow-stack access to them and no kernel pointer into them
     * exists for an ordinary write.
     *
     * The two callers that DO run after multicore_active is up —
     * cet_process_create for a cabin's user shadow stack and
     * cet_process_create_kernel_ssp for a process's bunk — map their
     * pages already carrying the encoding, so they reach here with
     * nothing to change and leave no stale writable entry to chase.
     * They call in for the parent walk above, not for the CAS.
     *
     * Nothing changed → nothing to invalidate: a page that was already
     * sealed left no stale writable entry anywhere. */
    if (changed)
        vmm_shootdown_page(ctx, virt_addr);
    return true;
}

bool vmm_shadow_stack_unseal(vmm_context_t *ctx, uintptr_t virt_addr)
{
    if (!ctx || !vmm_is_page_aligned(virt_addr))
        return false;

    bool changed = false;

    spin_lock(&ctx->lock);

    /* No demotion here: a sealed page necessarily already has a 4 KiB
     * leaf, because vmm_shadow_stack_seal made one. */
    pte_t *pte = vmm_get_pte(ctx, virt_addr);
    if (!pte)
    {
        spin_unlock(&ctx->lock);
        return false;
    }

    pte_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    for (;;)
    {
        if (!(old & VMM_FLAG_PRESENT))
        {
            spin_unlock(&ctx->lock);
            return false;
        }
        pte_t plain = old | VMM_FLAG_WRITABLE;
        if (plain == old)
            break;
        if (__atomic_compare_exchange_n(pte, &old, plain, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        {
            changed = true;
            break;
        }
    }

    spin_unlock(&ctx->lock);

    /* Broadcast, the mirror of the seal's reason: the core that sealed
     * this page is not necessarily the core freeing it, and every core
     * that translated it while it was sealed holds a read-only entry
     * that would fault the next writer of the recycled frame. On the one
     * path that runs before the machine is multicore — the out-of-memory
     * rollback in cet_lifecycle.c during AP bring-up — this is the same
     * local invlpg the seal did there, by the same test inside
     * vmm_shootdown_pages: no IPI is sent, no core is waited on.
     *
     * Dirty stays set — it is what it always meant, "this page was
     * written", and leaving it costs nothing. */
    if (changed)
        vmm_shootdown_page(ctx, virt_addr);
    return true;
}

/* ─── The bunk row — where a process's supervisor shadow stack lives ──
 *
 * Why it exists
 * -------------
 * A supervisor shadow stack is one 4 KiB page carrying an encoding no
 * neighbour may share (R/W = 0, Dirty = 1). Sealed in place in the Pull
 * Map, every such page costs the demotion of whichever 1 GiB or 2 MiB
 * leaf covers it — vmm_get_or_create_pte splits it — and nothing puts
 * that leaf back: vmm_shadow_stack_unseal restores R/W and stops there,
 * because a demoted leaf can only be recombined when all 512 of its
 * children agree again, and proving that costs a sweep of the table on
 * every free. A per-CORE shadow stack pays that once at bring-up and the
 * bill is closed. A per-PROCESS one pays it per spawn, at addresses the
 * buddy allocator scatters across all of RAM, so a machine that has been
 * up long enough ends with a Pull Map that is 4 KiB-granular everywhere:
 * the page tables grow toward 2 MiB per gigabyte of RAM and every huge-
 * page TLB entry that used to cover a gigabyte is gone with them.
 *
 * The row removes the split at its root rather than paying for it more
 * cleverly. It is a stretch of kernel VA that has no huge leaf to begin
 * with — nothing maps 1 GiB or 2 MiB pages here, so a page mapped here
 * is born on a 4 KiB leaf, and a shadow stack costs the page table its
 * own PD/PT would have cost anyway. There is nothing left to demote, and
 * vmm_shadow_stack_seal on a bunk address therefore walks to a PT entry
 * that already exists and changes nothing: it is kept in the path purely
 * as the check of SDM Vol 3A §4.6 condition (2) — R/W = 1 in every entry
 * ABOVE the leaf — which no leaf flag can express.
 *
 * How wide it is, and why that is not a number
 * --------------------------------------------
 * One bunk per pid. Not "enough bunks" — exactly the pids the PID
 * allocator can hold at once, PID_MAX_COUNT == MAX_PROCESSES, which is
 * the same ceiling process_create refuses at. A pid is issued as
 * index + 1 and released by pid_free, and a process's bunk is given
 * back (cet_process_destroy_kernel_ssp) strictly before its pid is
 * (process_cleanup), so two live processes can never name one bunk. The
 * row therefore cannot be exhausted by anything except a pid that does
 * not exist, and that case is refused by name rather than wrapped.
 *
 * That is also why there is no allocator here. The bunk IS the pid:
 * base + (pid - 1) pages. The PID allocator already did the only
 * search anyone has to do.
 *
 * Fully populated the row costs one PD, plus one PT for each 2 MiB of
 * it — 2 MiB being what one page table spans, because "a page table
 * comprises 512 64-bit entries (PTEs)" (Intel SDM Vol 3A §4.5.5,
 * 4-Level Paging) and each maps 4 KiB — and those tables are the row's
 * for good. The figure stops growing once every bunk has been used
 * once. What it replaces had no bound at all: a spawn demoted a 1 GiB
 * leaf into a PD and a 2 MiB leaf into a PT, anywhere in RAM the buddy
 * happened to land, and neither was ever recombined.
 *
 * ‼ WHAT THIS COSTS IN PROTECTION, AND WHY IT IS THE OWNER'S CALL
 * ---------------------------------------------------------------
 * Sealed in place in the Pull Map, a per-process supervisor shadow
 * stack had exactly ONE name, and that name was not writable: no kernel
 * pointer could touch it by an ordinary store. In the row it has two —
 * the bunk (R/W = 0) and the Pull Map alias (writable, as it always
 * was). The frame is reachable at PULL_MAP_BASE + phys whatever else
 * names it, and taking THAT mapping away for one page, or sealing it,
 * is the very demotion the row exists to avoid. So the two properties
 * cannot both be had this way round: keeping the Pull Map leaf whole
 * and keeping the frame single-named are the same action with opposite
 * signs.
 *
 * The aliasing itself is architecturally sound — access rights belong
 * to the translation an access actually uses (Intel SDM Vol 3A §5.6.1)
 * — and it is already the state of every USER shadow stack here:
 * cet_process_create seals the cabin's mapping and leaves the Pull Map
 * alias of the same frames writable. But that is a ring-0 alias of a
 * ring-3 asset, where ring 0 is trusted anyway; this is a ring-0 asset
 * whose whole point is to survive a ring-0 bug, so the two are not the
 * same trade and this one is not settled by precedent.
 *
 * It costs nothing today: no core ever loads proc->context.pl0_ssp, so
 * the processor never treats this page as a shadow stack at all. It
 * would start costing something the day the frame path gains a
 * per-process IA32_PL0_SSP swap, and that is the day the question has
 * to be answered rather than noted. The three answers that exist, so
 * whoever gets there does not have to find them again:
 *   (a) leave it as it is — the row, with the writable alias;
 *   (b) go back to sealing in the Pull Map and pay the demotion per
 *       spawn, which is what this change removed;
 *   (c) carve the shadow frames out of a SMALL reserved run of physical
 *       memory whose Pull Map leaf is demoted once, for good, and seal
 *       them in place inside it — one name, no writable alias, and a
 *       demotion bounded like the per-core one. That needs a frame
 *       source of its own in the PMM, which is why it is not what is
 *       here.
 *
 * The per-core PL0 / IST stacks — the only supervisor shadow stacks the
 * processor loads today — deliberately do NOT live in the row for the
 * same reason: sealed in the Pull Map their single mapping is writable
 * by nobody, and their demotion is paid once per core at bring-up and
 * never again. Moving them here would trade that away for nothing.
 */
/* The row's base, width and end are expressions in vmm.h, not addresses
 * anyone picked, so overlapping the MMIO window ahead of it is not a
 * thing a future edit can do. What an expression cannot say is where the
 * row lands in the page tables, and both of those facts matter: */

/* Same PML4 slot as the rest of the kernel window. vmm_create_context
 * copies PML4[256..511] BY VALUE (see vmm_create_context), so the PDPT
 * page a bunk's tables hang off is shared with every context, including
 * ones created before the first bunk existed. A row in a slot of its own
 * would be invisible in those. */
_Static_assert(VMM_PML4_INDEX(VMM_SHADOW_BUNK_BASE) ==
               VMM_PML4_INDEX(VMM_KERNEL_BASE) &&
               VMM_PML4_INDEX(VMM_SHADOW_BUNK_END - 1) ==
               VMM_PML4_INDEX(VMM_KERNEL_BASE),
               "shadow bunk row must share the kernel window's PML4 slot, "
               "which vmm_create_context copies into every context");

/* One PDPT slot from end to end, so the row needs a single PD and no
 * part of it can reach into a 1 GiB slot another window already owns.
 * Combined with the PML4 assert above this is exact: an equal PDPT index
 * 512 GiB away is excluded by the PML4 index already being pinned. */
_Static_assert(VMM_PDPT_INDEX(VMM_SHADOW_BUNK_END - 1) ==
               VMM_PDPT_INDEX(VMM_SHADOW_BUNK_BASE),
               "shadow bunk row must not leave the 1 GiB slot it starts in");

uintptr_t vmm_shadow_bunk_va(uint32_t pid)
{
    if (pid == 0 || (uintptr_t)pid > VMM_SHADOW_BUNK_COUNT)
        return 0;
    return VMM_SHADOW_BUNK_BASE + (((uintptr_t)pid - 1u) * VMM_PAGE_SIZE);
}

error_t vmm_shadow_bunk_map(uint32_t pid, uintptr_t phys_addr,
                            uintptr_t *out_va)
{
    if (!out_va) return ERR_INVALID_ARGUMENT;
    *out_va = 0;

    uintptr_t va = vmm_shadow_bunk_va(pid);
    if (!va)
    {
        kprintf("[VMM] shadow bunk refused: pid %u is outside the %lu bunks "
                "the row is cut to\n",
                pid, (unsigned long)VMM_SHADOW_BUNK_COUNT);
        return ERR_INVALID_PID;
    }

    vmm_context_t *ctx = vmm_get_kernel_context();
    if (!ctx) return ERR_INVALID_ADDRESS;

    /* An occupied bunk is the one way this row can go wrong, so it is
     * read out loud rather than left to be inferred from a mapper error
     * string. It means a pid was handed out again before its previous
     * owner's bunk came back — the ordering the row rests on (the bunk
     * is released in cet_process_destroy_kernel_ssp, the pid later in
     * process_cleanup) has been broken somewhere. Saying so here is what
     * turns that into a named refusal instead of a page quietly staying
     * mapped for the rest of the uptime.
     *
     * This read is the reporter, not the guard: vmm_map_page re-checks
     * the same entry under ctx->lock and refuses a conflicting one, so
     * nothing is decided on a value that could change underneath. */
    uintptr_t occupant = vmm_virt_to_phys(ctx, va);
    if (occupant)
    {
        kprintf("[VMM] shadow bunk %u at 0x%lx is still occupied by frame "
                "0x%lx — a pid was reissued before its bunk was given back; "
                "refusing rather than overwriting\n",
                pid, (unsigned long)va, (unsigned long)occupant);
        return ERR_ALREADY_MAPPED;
    }

    /* Born a shadow stack, not made into one afterwards: the encoding is
     * in the flags of the page's FIRST mapping here, so no CAS turns a
     * writable leaf into a sealed one and no core can be holding a
     * writable translation of this bunk to shoot down. NO_EXECUTE on top
     * of the encoding — a shadow stack holds return addresses and is
     * never fetched from; vmm_make_pte drops the bit by itself on a
     * machine that has not taken NX up. */
    vmm_map_result_t r = vmm_map_page(ctx, va, phys_addr,
                                      VMM_FLAGS_SHADOW_STACK_SUPV |
                                      VMM_FLAG_NO_EXECUTE);
    if (!r.success)
    {
        kprintf("[VMM] shadow bunk for pid %u at 0x%lx refused: %s\n",
                pid, (unsigned long)va,
                r.error_msg ? r.error_msg : "map failed");
        return ERR_MAPPING_FAILED;
    }

    *out_va = va;
    return OK;
}

bool vmm_shadow_bunk_unmap(uintptr_t va_in_bunk)
{
    if (va_in_bunk < VMM_SHADOW_BUNK_BASE || va_in_bunk >= VMM_SHADOW_BUNK_END)
        return false;

    /* The bunk is the page the address is in. Rounding here rather than
     * at the caller means the one place that knows the row's geometry is
     * the row, and a caller that holds the stack's top — an address
     * inside the page, never its base — needs no arithmetic that could
     * drift from it. */
    uintptr_t bunk_va = vmm_page_align_down(va_in_bunk);

    vmm_context_t *ctx = vmm_get_kernel_context();
    if (!ctx)
        return false;

    /* No unseal step, and that is the point: the encoding lived in this
     * mapping only. The Pull Map alias the PMM writes through was never
     * touched, so the frame goes back to the buddy exactly as ordinary
     * writable memory, and the next pmm_alloc_zero of it cannot fault. */
    return vmm_unmap_page(ctx, bunk_va);
}

typedef struct vmalloc_entry
{
    void *virt_base;
    size_t pages;
    struct vmalloc_entry *next;
} vmalloc_entry_t;

static vmalloc_entry_t *vmalloc_list = NULL;
static spinlock_t vmalloc_lock = {0};

void vmm_set_error(const char *error)
{
    if (error)
    {
        strncpy(last_error, error, sizeof(last_error) - 1);
        last_error[sizeof(last_error) - 1] = '\0';
    }
    else
    {
        last_error[0] = '\0';
    }
}

const char *vmm_get_last_error(void)
{
    return last_error;
}

void *vmm_phys_to_virt(uintptr_t phys_addr)
{
    if (g_pull_map_active)
    {
        return (void *)(phys_addr + PULL_MAP_BASE);
    }
    return (void *)phys_addr;
}

bool vmm_pcid_active(void)
{
    return g_pcid_active;
}

static error_t pcid_alloc_safe(uint16_t *out_pcid)
{
    if (!out_pcid)
        return ERR_NULL_POINTER;
    
    if (!g_pcid_active)
    {
        *out_pcid = 0;
        return OK;
    }

    spin_lock(&pcid_lock);
    
    if (pcid_free_count > 0)
    {
        if (pcid_free_count > PCID_MAX)
        {
            spin_unlock(&pcid_lock);
            return ERR_INTERNAL;
        }
        *out_pcid = pcid_free_stack[--pcid_free_count];
        spin_unlock(&pcid_lock);
        return OK;
    }
    
    if (pcid_next <= PCID_MAX)
    {
        *out_pcid = pcid_next++;
        spin_unlock(&pcid_lock);
        return OK;
    }
    
    /* PCID exhausted — Intel SDM Vol 3A §4.10.4.2. Local CR4.PGE toggle
     * flushes this CPU's TLB. The pre-fix vmm_shootdown_pages(kernel,
     * 0, 0) was a no-op (count==0 → zero invlpgs, ACKs never decrement)
     * → stale PCID-tagged TLB on remote cores aliasing the recycled
     * PCID's later PA. vmm_shootdown_all_cores_full() makes every core
     * toggle CR4.PGE (see vmm_tlb_shootdown_handler), flushing all PCID
     * partitions + globals — a CR3 reload alone would only invalidate the
     * reloaded PCID (SDM §4.10.4.1), not the recycled one.
     *
     * pcid_lock is held across the broadcast so no sibling pcid_alloc
     * can hand out a recycled PCID before every core's TLB has been
     * invalidated. The broadcast latency is microseconds in practice
     * (x2APIC ICR write) and the timeout cap is 100 ms only for the
     * pathological "AP wedged" case. Rolled-over PCID allocation
     * happens at most once per 4095 context creations, so the lock
     * hold is a non-issue. */
    pcid_next = 2;
    pcid_free_count = 0;
    *out_pcid = 1;

    uintptr_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    asm volatile("mov %0, %%cr4" : : "r"(cr4 & ~(1ULL << 7)) : "memory");
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    if (g_amp.multicore_active && g_amp.total_cores > 1)
    {
        vmm_shootdown_all_cores_full();
    }

    spin_unlock(&pcid_lock);
    return ERR_PCID_EXHAUSTED;
}

static void pcid_release(uint16_t pcid)
{
    if (pcid == PCID_KERNEL || pcid > PCID_MAX || !g_pcid_active)
        return;
    
    spin_lock(&pcid_lock);
    if (pcid_free_count < PCID_MAX)
    {
        pcid_free_stack[pcid_free_count++] = pcid;
    }
    spin_unlock(&pcid_lock);
}

/* Phase 2I — OR per-context LAM bits into CR3. No-op when ctx->lam_mode
 * is VMM_LAM_NONE or the CPU doesn't support LAM (writing LAM bits on
 * non-LAM hardware just stays ignored, but defensive gate avoids
 * confusing operators). */
static inline uint64_t vmm_cr3_lam_bits(vmm_context_t *ctx) {
    if (!g_cpu_caps.has_lam) return 0;
    if (ctx->lam_mode == VMM_LAM_U48) return VMM_CR3_LAM_U48;
    if (ctx->lam_mode == VMM_LAM_U57) return VMM_CR3_LAM_U57;
    return 0;
}

uint64_t vmm_build_cr3(vmm_context_t *ctx)
{
    if (!ctx)
        return 0;
    uint64_t lam = vmm_cr3_lam_bits(ctx);
    if (g_pcid_active)
    {
        return ctx->pml4_phys | (uint64_t)ctx->pcid | lam;
    }
    return ctx->pml4_phys | lam;
}

uint64_t vmm_build_cr3_noflush(vmm_context_t *ctx)
{
    if (!ctx)
        return 0;
    uint64_t lam = vmm_cr3_lam_bits(ctx);
    if (g_pcid_active)
    {
        return ctx->pml4_phys | (uint64_t)ctx->pcid | CR3_NOFLUSH | lam;
    }
    return ctx->pml4_phys | lam;
}

/* Set the per-context LAM mode. Validates mode value, gates on has_lam.
 * Caller is responsible for flushing this CPU's TLB + the next CR3
 * reload picks up the new bits. Returns OK on success, ERR otherwise. */
error_t vmm_set_user_lam(vmm_context_t *ctx, vmm_lam_mode_t mode) {
    if (!ctx) return ERR_NULL_POINTER;
    if (mode != VMM_LAM_NONE && !g_cpu_caps.has_lam) return ERR_UNSUPPORTED;
    if (mode > VMM_LAM_U57) return ERR_INVALID_ARGUMENT;
    /* LAM_U57 requires 5-level paging (Intel SDM Vol 3A §5.6 "Linear
     * Address Masking"). Reject when the kernel booted 4-level. */
    if (mode == VMM_LAM_U57 &&
        !__atomic_load_n(&g_vmm_la57_active, __ATOMIC_ACQUIRE)) {
        return ERR_UNSUPPORTED;
    }
    ctx->lam_mode = (uint8_t)mode;
    return OK;
}

uintptr_t vmm_alloc_page_table(void)
{
    void *page = pmm_alloc_zero(1);
    if (!page)
    {
        vmm_set_error("Failed to allocate page table from PMM");
        return 0;
    }
    atomic_fetch_add_u64((volatile uint64_t *)&global_stats.page_tables_allocated, 1);
    return (uintptr_t)page;
}

void vmm_free_page_table(uintptr_t phys_addr)
{
    if (!phys_addr)
        return;
    pmm_free((void *)phys_addr, 1);
    if (global_stats.page_tables_allocated > 0)
        atomic_fetch_sub_u64((volatile uint64_t *)&global_stats.page_tables_allocated, 1);
}

/* Force a full local TLB flush. Critical detail: when PCID is enabled,
 * CR3 has bit 63 (NOFLUSH) set by vmm_build_cr3_noflush(). A naive
 * read-modify-write of CR3 preserves that bit and the CPU then *skips*
 * the flush — the very thing we asked for. Always clear bit 63 before
 * writing CR3 here. This was the silent root cause behind random
 * "page-fault on write into .text" crashes seen on 2026-04-29: TLB
 * shootdowns appeared to fire but actually left stale entries alive,
 * so a recycled physical page kept being addressable by cores that
 * had switched off the destroyed cabin. */
void vmm_flush_tlb(void)
{
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    cr3 &= ~(1ULL << 63);
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    atomic_fetch_add_u64((volatile uint64_t *)&global_stats.tlb_flushes, 1);
}

void vmm_flush_tlb_page(uintptr_t virt_addr)
{
    asm volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    atomic_fetch_add_u64((volatile uint64_t *)&global_stats.tlb_flushes, 1);
}

void vmm_invalidate_page(uintptr_t virt_addr)
{
    vmm_flush_tlb_page(virt_addr);
}

// ---------------------------------------------------------------------------
// TLB Shootdown — cross-core invalidation via IPI_SHOOTDOWN_VECTOR
// ---------------------------------------------------------------------------
#include "amp.h"
#include "lapic.h"
#include "irqchip.h"
#include "scheduler.h"
#include "cpu_calibrate.h"

// Global shootdown descriptor — single-slot, serialized by g_shootdown_lock.
static struct
{
    volatile uintptr_t addr;        // target address (0 = full flush)
    volatile uint32_t page_count;   // pages to invalidate (0 = full flush)
    volatile uint32_t pending_acks; // atomic countdown
    volatile uint64_t generation;   // monotonic round id (see handler)
    volatile uintptr_t evict_pml4;  // teardown: PML4 phys every core must leave CR3 (0 = none)
    volatile bool active;
} __attribute__((aligned(64))) g_shootdown;

static spinlock_t g_shootdown_lock;

/* Monotonic shootdown round counter — mutated only under g_shootdown_lock. */
static uint64_t g_shootdown_gen;

/* Per-core "requested generation": shootdown_arm stamps each TARGET core's
 * slot with the round's generation; the handler services a round exactly once
 * and only if it is a target of the CURRENT generation. 0 = no request. */
static volatile uint64_t g_core_shootdown_req[MAX_CORES];

/* Arm the single global descriptor for a new round. Caller MUST hold
 * g_shootdown_lock. Stamps a fresh generation, marks each target core, sets
 * pending_acks + active, and fences — so all of it is globally visible before
 * the caller sends the IPIs. Returns the generation (unused by callers today,
 * handy for tracing). */
static uint64_t shootdown_arm(const uint8_t *targets, uint8_t target_count,
                              uintptr_t addr, uint32_t page_count, uintptr_t evict_pml4)
{
    uint64_t gen = ++g_shootdown_gen;           /* never 0 (g_core_*_req zero = none) */
    g_shootdown.addr       = addr;
    g_shootdown.page_count = page_count;
    g_shootdown.evict_pml4 = evict_pml4;
    g_shootdown.generation = gen;
    atomic_store_u32(&g_shootdown.pending_acks, target_count);
    g_shootdown.active = true;
    for (uint8_t i = 0; i < target_count; i++)
        __atomic_store_n(&g_core_shootdown_req[targets[i]], gen, __ATOMIC_RELAXED);
    mfence();
    return gen;
}

void vmm_tlb_shootdown_handler(void)
{
    if (!g_shootdown.active)
        return;

    /* Generation-gated, per-core idempotent ACK. The descriptor is ONE global
     * slot reused by every round; without this gate a late or duplicated
     * IPI_SHOOTDOWN from a PREVIOUS round — landing after a new holder armed
     * the slot — could decrement the CURRENT round's pending_acks even though
     * this core is not a target (or already ACKed). The sender would then
     * return before a real target invalidated its TLB → stale TLB → use-after-
     * unmap. Each round stamps its targets' g_core_shootdown_req[] with a
     * monotonic generation; a core proceeds only if its slot equals the
     * CURRENT generation, and claims it with a CAS so duplicate IPIs ACK at
     * most once. */
    uint8_t  me      = amp_get_core_index();
    uint64_t cur_gen = __atomic_load_n(&g_shootdown.generation, __ATOMIC_ACQUIRE);
    uint64_t my_req  = __atomic_load_n(&g_core_shootdown_req[me], __ATOMIC_RELAXED);
    if (my_req != cur_gen)
        return;   /* not a target of this round (stale IPI / not mine / done) */
    if (!__atomic_compare_exchange_n(&g_core_shootdown_req[me], &my_req, 0,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;   /* a duplicate IPI raced us — service exactly once */

    uintptr_t addr = g_shootdown.addr;
    uint32_t count = g_shootdown.page_count;

    if (addr == 0 || count == 0 || count > 64)
    {
        /* Full TLB flush across ALL PCID partitions via a CR4.PGE toggle.
         *
         * A plain CR3 reload (even with NOFLUSH cleared) only invalidates the
         * PCID named in the new CR3 (Intel SDM §4.10.4.1) — it leaves every
         * OTHER PCID's entries cached. That is the wrong primitive for the
         * caller that matters most here: vmm_shootdown_all_cores_full(), run
         * when a cabin is torn down and its PCID released for reuse. The dying
         * PCID is not any remote core's current CR3, so a CR3 reload there would
         * not drop its entries; when pcid_alloc recycles that PCID to a new
         * cabin, the stale entries alias the new mappings → the wild ".text #PF
         * / RIP=0" corruption that surfaced once full processes began being
         * reaped (and their contexts destroyed) at runtime. Toggling CR4.PGE
         * flushes the entire TLB — all PCIDs and globals — which is exactly what
         * "full shootdown on every core, irrespective of the context it runs"
         * is documented to mean. Same instruction pair pcid_alloc uses locally. */
        uintptr_t cr4;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        asm volatile("mov %0, %%cr4" : : "r"(cr4 & ~(1ULL << 7)) : "memory");
        asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }
    else
    {
        for (uint32_t i = 0; i < count; i++)
        {
            asm volatile("invlpg (%0)" : : "r"(addr + i * VMM_PAGE_SIZE) : "memory");
        }
    }

    /* Teardown eviction. vmm_destroy_context is about to pmm_free the PML4 of
     * the context it is tearing down; if THIS core's CR3 register still points
     * at that PML4 (it was switched out of the dying process but has not yet
     * loaded the incoming CR3 — the scheduler stores current_process=next
     * before context_restore_to_frame reloads CR3), the freed page would back
     * our address translation → the "CR3==CR2==freed PML4, err=0" kernel #PF.
     * The flush above drops cached entries but does NOT move the CR3 register,
     * so do it here: switch to the kernel address space. Because we ACK only
     * after this, shootdown_wait_acks completing guarantees no core is left on
     * the dying PML4 before it is freed. Compare PML4 phys only (mask off PCID
     * + NOFLUSH/LAM). Harmless mid context-switch: context_restore_to_frame
     * reloads the incoming CR3 unconditionally on an address-space change. */
    if (g_shootdown.evict_pml4)
    {
        uintptr_t cur_cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cur_cr3));
        if ((cur_cr3 & 0x000FFFFFFFFFF000ULL) == g_shootdown.evict_pml4)
        {
            asm volatile("mov %0, %%cr3" : : "r"(kernel_context->pml4_phys) : "memory");
        }
    }

    atomic_fetch_sub_u32(&g_shootdown.pending_acks, 1);
}

/* Service a pending cross-core TLB shootdown for THIS core from a NON-IPI
 * context — the spin_lock() wait loop, which runs with IRQs disabled and could
 * otherwise never ACK a shootdown that targets it (registered as klib's
 * spin-wait service hook in vmm_init). Same idempotent, generation-gated
 * handler the IPI vector runs: if the real IPI later fires too, the per-core
 * request slot is already CAS-claimed, so the second pass is a no-op. */
void vmm_tlb_shootdown_poll(void)
{
    vmm_tlb_shootdown_handler();
}

/* Wait for every target of the just-armed shootdown round to ACK, then return.
 *
 * The cure for the M1 real-HW deadlock lives in spin_lock(), not here: a core
 * spinning for an unrelated spinlock services shootdowns inline (klib's
 * wait-service hook → vmm_tlb_shootdown_poll), so a target that cannot take the
 * IPI because it spins with IRQs off still ACKs. A target in any other IRQ-off
 * section has the IPI queued in its LAPIC IRR (not lost) and ACKs the instant it
 * re-enables interrupts. So in practice this loop spins only microseconds.
 *
 * The TSC-scaled ceiling (CONFIG_TLB_SHOOTDOWN_PANIC_MS) is therefore a genuine
 * cross-core DEADLOCK DETECTOR, not a hair-trigger: nothing legitimate keeps a
 * core from ACKing for seconds, so a trip is a real fault worth a panic
 * (silently hanging here would be worse). It replaced a 100 ms budget that
 * false-paniced whenever a target sat in a longer IRQ-off section. The
 * spin-count backstop covers the one case the TSC ceiling cannot — a frozen TSC
 * (then the elapsed test never grows) — so a broken clock still cannot wedge
 * this IRQs-off loop forever. Caller holds g_shootdown_lock; `target_count`
 * only labels the panic. */
static void shootdown_wait_acks(uint8_t target_count, const char *what)
{
    uint64_t tsc_freq_mhz = cpu_get_tsc_freq_mhz();
    if (tsc_freq_mhz < 100) tsc_freq_mhz = CONFIG_TLB_SHOOTDOWN_TSC_FALLBACK_MHZ;

    const uint64_t panic_cycles = tsc_freq_mhz * 1000ULL * CONFIG_TLB_SHOOTDOWN_PANIC_MS;
    uint64_t start_tsc = rdtsc();
    uint64_t spins     = 0;

    while (atomic_load_u32(&g_shootdown.pending_acks) != 0)
    {
        cpu_pause();
        spins++;

        if ((uint64_t)(rdtsc() - start_tsc) > panic_cycles ||
            spins >= CONFIG_TLB_SHOOTDOWN_SPIN_BACKSTOP)
        {
            /* Re-load before declaring — the last ACK may have landed between
             * the while-test and here. */
            uint32_t remaining = atomic_load_u32(&g_shootdown.pending_acks);
            if (remaining == 0) break;
            panic("TLB shootdown timeout (%s): %u/%u cores did not ACK",
                  what, remaining, target_count);
        }
    }
}

void vmm_shootdown_pages(vmm_context_t *ctx, uintptr_t virt_addr, size_t page_count)
{
    // Single-core: local invalidation only
    if (!g_amp.multicore_active || g_amp.total_cores <= 1)
    {
        for (size_t i = 0; i < page_count; i++)
        {
            asm volatile("invlpg (%0)" : : "r"(virt_addr + i * VMM_PAGE_SIZE) : "memory");
        }
        return;
    }

    // Determine which remote cores have this context loaded in CR3.
    // Kernel context (shared PML4 entries) → all cores.
    // User context → only cores whose current_process uses this cabin.
    bool is_kernel = (ctx == kernel_context);
    uint8_t my_core = amp_get_core_index();
    uint8_t targets[MAX_CORES];
    uint8_t target_count = 0;

    for (uint8_t c = 0; c < g_amp.total_cores; c++)
    {
        if (c == my_core)
            continue;
        if (!amp_core_online(&g_amp.cores[c]))
            continue;

        if (is_kernel)
        {
            targets[target_count++] = c;
        }
        else
        {
            scheduler_state_t *rs = scheduler_get_core(c);
            /* ACQUIRE-load the remote core's current strand: it is published
             * under scheduler_lock before that core loads the new CR3, and
             * "an unmap here shoots down every core running this cabin CR3"
             * relies on (a) this ordered read and (b) the dispatch-path CR3
             * load being NON-NOFLUSH (P2 fix).  If a future change ever
             * reintroduces a NOFLUSH load on dispatch, an incoming sibling
             * could miss this shootdown — keep that path flushing. */
            process_t *rp = __atomic_load_n(&rs->current_process, __ATOMIC_ACQUIRE);
            if (rp && rp->cabin && rp->cabin->vmm && rp->cabin->vmm->pml4_phys == ctx->pml4_phys)
            {
                targets[target_count++] = c;
            }
        }
    }

    // Flush self first
    if (page_count <= 64)
    {
        for (size_t i = 0; i < page_count; i++)
        {
            asm volatile("invlpg (%0)" : : "r"(virt_addr + i * VMM_PAGE_SIZE) : "memory");
        }
    }
    else
    {
        /* Self full flush — clear NOFLUSH bit before reload. */
        uintptr_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        cr3 &= ~(1ULL << 63);
        asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    }

    if (target_count == 0)
        return;

    // Arm the shootdown descriptor and broadcast.
    //
    // Acquire g_shootdown_lock with spin_trylock in a loop rather than
    // spin_lock. Deadlock this avoids: plain spin_lock does `cli` then spins,
    // so a core waiting to start ITS OWN shootdown would wait with interrupts
    // OFF — and if the current holder's shootdown targets that core, it can
    // never run the IPI handler to ACK, so the holder times out (KERNEL PANIC
    // "TLB shootdown timeout"). spin_trylock restores the caller's IRQ state
    // on each failed attempt (interrupts ON for the kernel/K-Core callers),
    // so a waiter keeps servicing the holder's shootdown IPI while it spins,
    // then ends up holding the lock with IRQs OFF exactly like spin_lock
    // (matched by the spin_unlock below). Latent historically; the P5b strand
    // reaper made runtime shootdowns frequent enough to surface it. The IPI
    // handler (vmm_shootdown_ipi) takes no lock, so running it mid-spin is safe.
    while (!spin_trylock(&g_shootdown_lock))
    {
        /* Drain our own pending shootdown while spinning for the lock. trylock
         * keeps IRQs at the caller's level; a caller nested in an IRQs-off
         * critical section (vmm_destroy_context holds ctx->lock across its full
         * shootdown) would otherwise never take the IPI, so the current holder —
         * blocked in shootdown_wait_acks waiting for THIS core to ACK — would
         * deadlock into the shootdown-timeout panic. Servicing inline (the same
         * idempotent, generation-gated handler the IPI runs; a later real IPI is
         * a no-op) lets us ACK even with IRQs off. */
        vmm_tlb_shootdown_poll();
        cpu_pause();
    }

    shootdown_arm(targets, target_count,
                  (page_count <= 64) ? virt_addr : 0,
                  (page_count <= 64) ? (uint32_t)page_count : 0,
                  0 /* no CR3 eviction — a live context is only being unmapped */);

    for (uint8_t i = 0; i < target_count; i++)
    {
        lapic_send_ipi(g_amp.cores[targets[i]].lapic_id, IPI_SHOOTDOWN_VECTOR);
    }

    shootdown_wait_acks(target_count, "pages");

    g_shootdown.active = false;
    spin_unlock(&g_shootdown_lock);
}

/* Force a full TLB flush on every online core, irrespective of which
 * context they are currently running. Used by vmm_destroy_context: when
 * a cabin's page tables are torn down, the underlying physical pages
 * are about to be returned to PMM and may be re-allocated to *any*
 * cabin within microseconds. Without flushing every core, AMP cores
 * that recently switched off the destroyed cabin keep stale TLB
 * entries (PCID cached) — those entries then alias whichever new
 * mapping receives the recycled PA, producing the unpredictable
 * ".text page-fault on user write" pattern reported in 2026-04-29
 * crash logs.
 *
 * evict_pml4 (0 = none): when tearing down a context whose PML4 is about to be
 * freed, every remote core still holding it in CR3 must switch to the kernel
 * address space before we free the page (see the handler) — otherwise a core
 * caught mid context-switch (current_process already advanced, CR3 not yet
 * reloaded) translates through the freed PML4. Passing the dying PML4 here
 * makes shootdown_wait_acks a true "no core is on this CR3 anymore" barrier. */
static void vmm_shootdown_full_evict(uintptr_t evict_pml4)
{
    if (!g_amp.multicore_active || g_amp.total_cores <= 1) {
        /* Single-core: just flush ourselves (clear NOFLUSH bit). */
        uintptr_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        cr3 &= ~(1ULL << 63);
        asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.tlb_flushes, 1);
        return;
    }

    uint8_t my_core = amp_get_core_index();
    uint8_t targets[MAX_CORES];
    uint8_t target_count = 0;

    for (uint8_t c = 0; c < g_amp.total_cores; c++) {
        if (c == my_core) continue;
        if (!amp_core_online(&g_amp.cores[c])) continue;
        targets[target_count++] = c;
    }

    /* Local self flush first — clear NOFLUSH bit before reload. */
    {
        uintptr_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        cr3 &= ~(1ULL << 63);
        asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.tlb_flushes, 1);
    }

    if (target_count == 0)
        return;

    /* Interruptible acquire — same deadlock avoidance as vmm_shootdown_pages:
     * plain spin_lock would wait with IRQs off and a waiter could never ACK
     * the current holder's shootdown that targets it. spin_trylock restores
     * the caller's IRQ state between attempts so the waiter keeps servicing
     * shootdown IPIs while spinning. See vmm_shootdown_pages for the full
     * rationale. */
    while (!spin_trylock(&g_shootdown_lock))
    {
        /* Drain our own pending shootdown while spinning — see vmm_shootdown_pages.
         * Critical here: vmm_destroy_context calls this from inside spin_lock(
         * &ctx->lock) (IRQs off), and two K-Cores reaping exited full processes
         * concurrently each hold their own ctx->lock; without inline servicing
         * the lock loser never ACKs the winner → "TLB shootdown timeout (full)". */
        vmm_tlb_shootdown_poll();
        cpu_pause();
    }

    shootdown_arm(targets, target_count, 0, 0, evict_pml4);   /* 0/0 ⇒ handler does full flush */

    for (uint8_t i = 0; i < target_count; i++) {
        lapic_send_ipi(g_amp.cores[targets[i]].lapic_id, IPI_SHOOTDOWN_VECTOR);
    }

    shootdown_wait_acks(target_count, "full");

    g_shootdown.active = false;
    spin_unlock(&g_shootdown_lock);
}

/* Public entry — full flush on every core with NO CR3 eviction. For callers
 * that are not freeing the PML4 a running core may hold (PCID rollover in
 * pcid_alloc_safe, and any external caller). The teardown path in
 * vmm_destroy_context calls vmm_shootdown_full_evict(pml4) directly. */
void vmm_shootdown_all_cores_full(void)
{
    vmm_shootdown_full_evict(0);
}

void vmm_shootdown_page(vmm_context_t *ctx, uintptr_t virt_addr)
{
    vmm_shootdown_pages(ctx, virt_addr, 1);
}

/* Atomically demote a LARGE_PAGE entry to a smaller-granularity table.
 *
 * Walks one level: replaces a 1 GB (level==1) or 2 MB (level==2) leaf
 * entry with a pointer to a freshly-allocated PD/PT that re-creates
 * the same physical mappings at finer granularity. Used to support
 * per-page modification of regions originally mapped with huge pages
 * (specifically the Pull Map/DPM when CPU exposes 1 GB pages, and
 * any 2 MB region we later need to unmap a single 4 KB page from —
 * kernel stack guard pages).
 *
 * Concurrency: every caller of vmm_get_or_create_table is potentially
 * lock-free on the PDPT/PD level (e.g. idle_setup,
 * tss_setup_dynamic_stacks, AP-boot and process_create paths all
 * call vmm_get_or_create_pte → vmm_get_or_create_table directly,
 * without holding any ctx lock). Two cores racing on the same
 * huge-page entry would both allocate a replacement table and the
 * loser's allocation would leak; worse, both writes to *entry could
 * publish their own pointer producing a partial-PD/PT corruption.
 *
 * We therefore commit the replacement with a single CAS on the
 * parent entry. The loser frees its just-allocated page back to PMM
 * and re-reads the entry, which by then is either the winner's
 * replacement (same physical translations) or already further
 * demoted by a third party — either way next walk-level proceeds
 * correctly.
 *
 * Real-HW TLB safety: Intel SDM Vol 3 §4.10.4.4 disallows changing
 * the page size of a resident translation without invalidation. We
 * issue a LOCAL `invlpg virt_addr` after a successful CAS to drop
 * the stale huge-page entry on the writing core — this covers the
 * Intel-cited "atomic re-walk" requirement for the core that
 * actually published the change.
 *
 * Cross-core shootdown is intentionally NOT issued from here. The
 * split is semantically transparent (same physical mapping at finer
 * granularity); stale huge-page entries on other cores still decode
 * to identical addresses until somebody actually rewrites one of
 * the new fine-grained leaves. The leaf-modifying caller
 * (vmm_unmap_page, guard-page clear) is responsible for the
 * cross-core flush of *that specific leaf*. Issuing the cross-core
 * shootdown here would deadlock against any concurrent path holding
 * the same g_shootdown_lock — empirically observed 2026-05-14 as a
 * "Full TLB shootdown timeout: N/N cores did not ACK" panic during
 * AUTOSTART.
 *
 * Returns:
 *   0  — split applied (CAS won) or no longer needed (entry no
 *        longer LARGE_PAGE: another core won the race)
 *   -1 — allocation failure (out of memory) */
static int vmm_demote_large_entry(pte_t *entry, int level, uintptr_t virt_addr)
{
    pte_t old_entry = __atomic_load_n(entry, __ATOMIC_ACQUIRE);
    if (!(old_entry & VMM_FLAG_PRESENT) || !(old_entry & VMM_FLAG_LARGE_PAGE))
    {
        /* Concurrent reader already saw the demoted form (or entry was
         * cleared). Nothing to do. */
        return 0;
    }

    uintptr_t large_base = vmm_pte_to_phys(old_entry);
    uint64_t  inherit_flags = vmm_pte_to_flags(old_entry) & ~VMM_FLAG_LARGE_PAGE;

    uintptr_t new_phys = vmm_alloc_page_table();
    if (!new_phys)
    {
        vmm_set_error("Failed to allocate replacement table for large-page demotion");
        return -1;
    }

    page_table_t *new_tbl = (page_table_t *)vmm_phys_to_virt(new_phys);

    /* Populate the replacement with 512 entries at the next-finer
     * granularity covering exactly the same physical range. At
     * level==1 (1 GB → PD) each child is itself a 2 MB LARGE_PAGE;
     * at level==2 (2 MB → PT) each child is a 4 KB PTE. */
    if (level == 1)
    {
        for (int j = 0; j < 512; j++)
        {
            uintptr_t chunk = large_base + ((uintptr_t)j << 21);  /* 2 MB stride */
            new_tbl->entries[j] = vmm_make_pte(chunk, inherit_flags | VMM_FLAG_LARGE_PAGE);
        }
    }
    else
    {
        for (int j = 0; j < 512; j++)
        {
            uintptr_t chunk = large_base + ((uintptr_t)j * VMM_PAGE_SIZE);
            new_tbl->entries[j] = vmm_make_pte(chunk, inherit_flags);
        }
    }

    /* Publish: CAS the parent entry from <old large> to <new pointer>.
     * Intermediate tables need USER so user-mode walks at finer
     * granularity can still reach an eventually-USER leaf. */
    pte_t new_entry  = vmm_make_pte(new_phys, VMM_FLAGS_KERNEL_RW | VMM_FLAG_USER);
    pte_t expected   = old_entry;
    bool  won        = __atomic_compare_exchange_n(entry, &expected, new_entry,
                                                   false,
                                                   __ATOMIC_RELEASE,
                                                   __ATOMIC_ACQUIRE);
    if (!won)
    {
        /* Lost the race — drop our replacement back into PMM. */
        pmm_free((void *)new_phys, 1);
        return 0;
    }

    /* Local invlpg: required by Intel SDM Vol 3 §4.10.4.4 — the writing
     * core must invalidate its own TLB before it can safely observe
     * translations through the just-demoted entry. Covers every TLB
     * level the linear address touches (4 KB / 2 MB / 1 GB). */
    asm volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    return 0;
}

page_table_t *vmm_get_or_create_table(vmm_context_t *ctx, uintptr_t virt_addr, int level)
{
    if (!ctx || !ctx->pml4)
        return NULL;

    /* Under 5-level paging, descend PML5 first to reach the PML4 for this
     * VA's PML5 index. Under 4-level this is a no-op returning ctx->pml4
     * directly. CAS-publishes a new PML4 if needed. */
    page_table_t *current_table = vmm_walk_pml5_to_pml4(ctx, virt_addr, true);
    if (!current_table) return NULL;
    uint32_t indices[4] = {
        VMM_PML4_INDEX(virt_addr),
        VMM_PDPT_INDEX(virt_addr),
        VMM_PD_INDEX(virt_addr),
        VMM_PT_INDEX(virt_addr)};

    for (int i = 0; i < level; i++)
    {
        pte_t *entry = &current_table->entries[indices[i]];

        if (!(*entry & VMM_FLAG_PRESENT))
        {
            uintptr_t new_table_phys = vmm_alloc_page_table();
            if (!new_table_phys)
            {
                vmm_set_error("Failed to allocate page table");
                return NULL;
            }

            /* Intermediate tables need USER bit so Ring 3 walks at all
             * levels — leaf USER bit alone is not enough.
             *
             * Race: another core may have set the entry first. CAS so
             * we don't leak the page or trample the winning pointer. */
            pte_t new_entry  = vmm_make_pte(new_table_phys, VMM_FLAGS_KERNEL_RW | VMM_FLAG_USER);
            pte_t expected   = 0;
            bool  won        = __atomic_compare_exchange_n(entry, &expected, new_entry,
                                                           false,
                                                           __ATOMIC_RELEASE,
                                                           __ATOMIC_ACQUIRE);
            if (!won)
            {
                pmm_free((void *)new_table_phys, 1);
            }
        }
        else if ((i == 1 || i == 2) && (*entry & VMM_FLAG_LARGE_PAGE))
        {
            /* Demote 1 GB (i==1, PDPT) or 2 MB (i==2, PD) huge page so
             * the walker can descend one more level. The helper is
             * race-safe (CAS-published with local invlpg) and a no-op
             * when the entry has already been demoted by another core
             * since the last *entry read. */
            if (vmm_demote_large_entry(entry, i, virt_addr) < 0)
            {
                return NULL;
            }
        }

        uintptr_t next_table_phys = vmm_pte_to_phys(*entry);
        current_table = (page_table_t *)vmm_phys_to_virt(next_table_phys);
    }

    return current_table;
}

// does not allocate; returns NULL if any intermediate table is missing
static pte_t *vmm_get_pte_noalloc(vmm_context_t *ctx, uintptr_t virt_addr)
{
    if (!ctx || !ctx->pml4)
        return NULL;

    uint32_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    uint32_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    uint32_t pd_idx = VMM_PD_INDEX(virt_addr);
    uint32_t pt_idx = VMM_PT_INDEX(virt_addr);

    /* 5-level paging: descend PML5 → PML4 first. Returns NULL if PML5 slot
     * empty (no PML4 allocated for this VA region). */
    page_table_t *pml4 = vmm_walk_pml5_to_pml4(ctx, virt_addr, false);
    if (!pml4) return NULL;
    pte_t pml4_entry = pml4->entries[pml4_idx];
    if (!(pml4_entry & VMM_FLAG_PRESENT))
        return NULL;

    page_table_t *pdpt = (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pml4_entry));
    pte_t pdpt_entry = pdpt->entries[pdpt_idx];
    if (!(pdpt_entry & VMM_FLAG_PRESENT))
        return NULL;

    if (pdpt_entry & VMM_FLAG_LARGE_PAGE)
    {
        return NULL;
    }

    page_table_t *pd = (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pdpt_entry));
    pte_t pd_entry = pd->entries[pd_idx];
    if (!(pd_entry & VMM_FLAG_PRESENT))
        return NULL;

    if (pd_entry & VMM_FLAG_LARGE_PAGE)
    {
        return NULL;
    }

    page_table_t *pt = (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pd_entry));
    return &pt->entries[pt_idx];
}

pte_t *vmm_get_or_create_pte(vmm_context_t *ctx, uintptr_t virt_addr)
{
    page_table_t *pt = vmm_get_or_create_table(ctx, virt_addr, 3);
    if (!pt)
        return NULL;
    return &pt->entries[VMM_PT_INDEX(virt_addr)];
}

/*
 * vmm_get_leaf_pte — return a pointer to whichever PT/PD/PDPT entry is
 * the LEAF for `virt_addr`. Out-parameter `out_level` reports the level
 * of the leaf:
 *   1 → 4 KiB PT entry  (caller masks PTE bits 12+ for phys)
 *   2 → 2 MiB PD leaf  (caller masks PTE bits 21+ for phys)
 *   3 → 1 GiB PDPT leaf (caller masks PTE bits 30+ for phys)
 *
 * Returns NULL when no mapping covers `virt_addr` at any level. This is
 * the only walker that can find a PRESENT 2 MiB / 1 GiB leaf — the
 * existing `vmm_get_pte` short-circuits at LARGE_PAGE bits and returns
 * NULL.  Phase 2C revoke/grant uses this to toggle PTE.P regardless of
 * page size.
 */
pte_t *vmm_get_leaf_pte(vmm_context_t *ctx, uintptr_t virt_addr,
                        uint8_t *out_level)
{
    if (out_level) *out_level = 0;
    if (!ctx || !ctx->pml4) return NULL;

    uint32_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    uint32_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    uint32_t pd_idx   = VMM_PD_INDEX(virt_addr);
    uint32_t pt_idx   = VMM_PT_INDEX(virt_addr);

    /* 5-level: descend PML5 → PML4 first (no-op under 4-level). */
    page_table_t *pml4_table = vmm_walk_pml5_to_pml4(ctx, virt_addr, false);
    if (!pml4_table) return NULL;
    pte_t pml4_entry = pml4_table->entries[pml4_idx];
    if (!(pml4_entry & VMM_FLAG_PRESENT)) return NULL;

    page_table_t *pdpt =
        (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pml4_entry));
    pte_t pdpt_entry = pdpt->entries[pdpt_idx];
    if (!(pdpt_entry & VMM_FLAG_PRESENT)) return NULL;
    if (pdpt_entry & VMM_FLAG_LARGE_PAGE) {
        if (out_level) *out_level = 3;
        return &pdpt->entries[pdpt_idx];
    }

    page_table_t *pd =
        (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pdpt_entry));
    pte_t pd_entry = pd->entries[pd_idx];
    if (!(pd_entry & VMM_FLAG_PRESENT)) return NULL;
    if (pd_entry & VMM_FLAG_LARGE_PAGE) {
        if (out_level) *out_level = 2;
        return &pd->entries[pd_idx];
    }

    page_table_t *pt =
        (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pd_entry));
    if (out_level) *out_level = 1;
    return &pt->entries[pt_idx];
}

pte_t *vmm_get_pte(vmm_context_t *ctx, uintptr_t virt_addr)
{
    return vmm_get_pte_noalloc(ctx, virt_addr);
}

vmm_context_t *vmm_create_context(void)
{
    vmm_context_t *ctx = kmalloc(sizeof(vmm_context_t));
    if (!ctx)
    {
        vmm_set_error("Failed to allocate VMM context");
        return NULL;
    }

    memset(ctx, 0, sizeof(vmm_context_t));

    uintptr_t pml4_phys = vmm_alloc_page_table();
    if (!pml4_phys)
    {
        kfree(ctx);
        vmm_set_error("Failed to allocate PML4 table");
        return NULL;
    }

    ctx->pml4 = (page_table_t *)vmm_phys_to_virt(pml4_phys);
    ctx->pml4_phys = pml4_phys;
    ctx->heap_start = VMM_USER_HEAP_BASE;
    ctx->heap_end = VMM_USER_HEAP_BASE;
    ctx->stack_top = VMM_USER_STACK_TOP;

    error_t pcid_err = pcid_alloc_safe(&ctx->pcid);
    if (pcid_err == ERR_PCID_EXHAUSTED)
    {
        debug_printf("[VMM] PCID exhausted, full TLB flush occurred\n");
    }

    spinlock_init(&ctx->lock);

    if (kernel_context && kernel_context->pml4)
    {
        for (int i = 256; i < 512; i++)
        {
            ctx->pml4->entries[i] = kernel_context->pml4->entries[i];
        }
    }

    atomic_fetch_add_u64((volatile uint64_t *)&global_stats.total_contexts, 1);

    return ctx;
}

/* Walk one PML4's user entries [0..pml4_entry_end), freeing PDPT/PD/PT
 * structures and marking user data pages in the dedup bitmap for deferred
 * pmm_free (Phase 1 of the two-phase teardown).
 *
 * Caller bounds:
 *   • 4-level paging: invoked once with pml4=ctx->pml4, end=256 (user half;
 *     entries 256..511 are kernel-shared).
 *   • 5-level paging: invoked per non-NULL PML5[0..255] entry with end=512
 *     (entire PML4 is user under 5-level; kernel sits under PML5[511] in a
 *     separate PML4 that this function never touches).
 *
 * `pml4_top_idx_for_virt` is the PML5 (5-level) or 0 (4-level) index that
 * names this PML4 in the linear-address space. Used only to compose the
 * `is_identity_mapped` check for the (now dead in practice) pre-Pull-Map
 * boot identity teardown path. */

/* Frames already returned to the PMM during THIS teardown.
 *
 * The guard exists because one frame can be named by more than one PTE in the
 * same cabin (a CoW snapshot sharing pages with the live mapping): each unique
 * frame must be freed exactly once, or the buddy takes a double free.
 *
 * Keyed by frame number, so the scratch costs what the ADDRESS SPACE costs. It
 * used to be one bit per physical page — 288 KiB on an 8 GiB box, 16 MiB on a
 * 512 GiB one — to track a walk that can touch at most this context's own
 * mapped pages. Slots hold frame+1 so that 0 means empty and no user frame
 * needs a sentinel of its own. */
typedef struct
{
    uint64_t *slots;
    size_t    mask;      /* capacity - 1; capacity is a power of two */
    void     *phys;      /* PMM block backing slots[] */
    size_t    pages;
} VmmFreedSet;

static size_t vmm_pow2_ceil(size_t v)
{
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* True when `frame` was absent and is now recorded — i.e. the caller owns the
 * free. False when it was already there (aliased), or when the set is full,
 * which cannot happen at the load factor chosen in vmm_destroy_context but is
 * answered conservatively rather than assumed away. */
static bool vmm_freed_set_add(VmmFreedSet *set, uint64_t frame)
{
    if (!set || !set->slots)
        return false;

    uint64_t key = frame + 1;
    /* Fibonacci hashing: one multiply, and it spreads the sequential frame
     * numbers a linear walk produces instead of piling them into one run. */
    size_t i = (size_t)((key * 0x9E3779B97F4A7C15ULL) >> 32) & set->mask;

    for (size_t probe = 0; probe <= set->mask; probe++)
    {
        uint64_t cur = set->slots[i];
        if (cur == 0)
        {
            set->slots[i] = key;
            return true;
        }
        if (cur == key)
            return false;
        i = (i + 1) & set->mask;
    }

    kprintf("[VMM] ERROR: dedup set full at frame 0x%lx — leaking the rest of "
            "this context to avoid a double-free\n", (unsigned long)frame);
    return false;
}
static void vmm_walk_free_pml4_user_(page_table_t *pml4, int pml4_entry_end,
                                     VmmFreedSet *freed,
                                     uint64_t pml5_va_term)
{
    for (int p4 = 0; p4 < pml4_entry_end; p4++)
    {
        pte_t pml4_entry = pml4->entries[p4];
        if (!(pml4_entry & VMM_FLAG_PRESENT))
            continue;

        uintptr_t pdpt_phys = vmm_pte_to_phys(pml4_entry);
        page_table_t *pdpt = (page_table_t *)vmm_phys_to_virt(pdpt_phys);

        int skip_pt_free = 0;

        for (int p3 = 0; p3 < 512; p3++)
        {
            pte_t pdpt_entry = pdpt->entries[p3];
            if (!(pdpt_entry & VMM_FLAG_PRESENT))
                continue;

            if (pdpt_entry & VMM_FLAG_LARGE_PAGE)
            {
                continue;
            }

            uintptr_t pd_phys = vmm_pte_to_phys(pdpt_entry);
            page_table_t *pd = (page_table_t *)vmm_phys_to_virt(pd_phys);

            for (int p2 = 0; p2 < 512; p2++)
            {
                pte_t pd_entry = pd->entries[p2];
                if (!(pd_entry & VMM_FLAG_PRESENT))
                    continue;

                if (pd_entry & VMM_FLAG_LARGE_PAGE)
                {
                    /* User-owned 2 MB pages (Bay, user-heap implicit
                     * huge) must be returned to PMM here. Kernel-side
                     * 2 MB pages (identity, Pull Map) lack VMM_FLAG_USER
                     * and we still skip them. Shared phys are skipped
                     * too — they live for the whole kernel session. */
                    if (pd_entry & VMM_FLAG_USER) {
                        uintptr_t phys = vmm_pte_to_phys(pd_entry);
                        if (!vmm_is_shared_phys(phys)) {
                            pmm_free((void *)phys, VMM_LARGE_PAGE_2M_PAGES);
                        }
                        pd->entries[p2] = 0;
                    }
                    continue;
                }

                uintptr_t pt_phys = vmm_pte_to_phys(pd_entry);
                page_table_t *pt = (page_table_t *)vmm_phys_to_virt(pt_phys);

                int freed_pages = 0;
                for (int p1 = 0; p1 < 512; p1++)
                {
                    pte_t pt_entry = pt->entries[p1];
                    if (!(pt_entry & VMM_FLAG_PRESENT))
                        continue;

                    uintptr_t phys = vmm_pte_to_phys(pt_entry);

                    /* Compose the linear-address term the boot-identity
                     * check needs. Under 5-level we add the PML5 slot's
                     * VA contribution (pml5_va_term, 256 TB per slot).
                     * Under 4-level pml5_va_term is 0. */
                    uintptr_t virt = pml5_va_term +
                                     (p4 * 512ULL * 1024 * 1024 * 1024) +
                                     (p3 * 1024 * 1024 * 1024) +
                                     (p2 * 2 * 1024 * 1024) +
                                     (p1 * VMM_PAGE_SIZE);
                    bool is_identity_mapped = (!g_pull_map_active) && (phys == virt);

                    /* Shared kernel pages (cpu_caps, ClockBoard, future
                     * vDSO-style pages) are mapped into many Cabins but
                     * the physical lives for the whole kernel session.
                     * Skip the pmm_free — only zero the PTE so the next
                     * process to be created does NOT inherit the entry.
                     * Registered via vmm_register_shared_phys() at boot. */
                    if (vmm_is_shared_phys(phys))
                    {
                        if (!is_identity_mapped)
                        {
                            pt->entries[p1] = 0;
                        }
                        continue;
                    }

                    /* Free the data page NOW, inline with the walk. The Phase 0
                     * quiesce already evicted every core off this address space
                     * and flushed all TLBs, so the frame can return to PMM
                     * immediately — there is no longer a second pass over all of
                     * physical RAM (the old O(total-RAM) Phase 3 scan is gone;
                     * teardown is now O(mapped-pages)). The dedup set still
                     * guards a frame aliased by more than one PTE in this cabin
                     * (e.g. a CoW snapshot sharing pages with the live mapping):
                     * free each unique frame exactly once. With no set (its own
                     * allocation failed) we must NOT free — an un-deduped
                     * double-free would corrupt the buddy — so the frame leaks,
                     * a rare bounded fallback. */
                    if (!is_identity_mapped)
                    {
                        if (vmm_freed_set_add(freed, phys / VMM_PAGE_SIZE))
                        {
                            pmm_free((void *)phys, 1);
                            freed_pages++;
                        }
                        pt->entries[p1] = 0;
                    }
                }

                if (freed_pages > 0)
                {
                    debug_printf("[VMM]   Freed %d data pages\n", freed_pages);
                }

                if (!skip_pt_free)
                {
                    vmm_free_page_table(pt_phys);
                }
                pd->entries[p2] = 0;
            }

            vmm_free_page_table(pd_phys);
            pdpt->entries[p3] = 0;
        }

        vmm_free_page_table(pdpt_phys);
        pml4->entries[p4] = 0;
    }
}

static void vmm_free_user_space_tables(vmm_context_t *ctx)
{
    if (!ctx || !ctx->pml4)
        return;

    debug_printf("[VMM] Freeing user space tables for context CR3=0x%lx (%d-level)\n",
                 ctx->pml4_phys, g_vmm_paging_levels);

    /* Phase 0 — quiesce, BEFORE the walk frees any paging structure.
     *
     * The walk below frees PT/PD/PDPT pages inline (vmm_free_page_table →
     * pmm_free), returning them to the allocator for immediate reuse. If any
     * core still carries THIS context's PML4 in its CR3 register — e.g. one
     * switched out of the now-dead process but not yet reloaded with the
     * incoming CR3 (the scheduler advances current_process before context_
     * restore_to_frame reloads CR3) — its hardware page-walk would then read
     * freed-and-recycled paging structures, the "RIP=0 / CR3==CR2 #PF" wild
     * fault that only appears once contexts are destroyed at RUNTIME (full
     * processes reaped) rather than at shutdown. vmm_shootdown_full_evict
     * flushes every core's TLB (all PCID partitions) AND moves any core still
     * on this PML4 to the kernel address space, so on return no core references
     * these tables. The dead process is already unschedulable (unlinked from
     * the run list), so none can re-load this CR3 during the walk — making this
     * single up-front barrier sufficient: nothing re-caches a dying context's
     * entries between here and the data-page free, so no post-walk shootdown is
     * needed. Runs under ctx->lock (IRQs off); the shootdown's trylock loop
     * drains inline, so it does not deadlock. */
    vmm_shootdown_full_evict(ctx->pml4_phys);

    /* Dedup scratch, sized by THIS context rather than by installed RAM.
     *
     * Capacity is a power of two at twice the context's mapped-page count, so
     * linear probing runs at a load factor of 0.5 or better and insertion
     * cannot fail through fullness. From the PMM: the kernel heap is a fixed
     * small-object pool, and this is page-scale scratch for a walk that is
     * about to hand pages back to the PMM anyway. */
    VmmFreedSet freed = {0};
    {
        size_t want_slots = vmm_pow2_ceil(((size_t)ctx->mapped_pages + 1) * 2);
        size_t want_bytes = want_slots * sizeof(uint64_t);
        freed.pages = (want_bytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
        freed.phys  = pmm_alloc_zero(freed.pages);
        if (freed.phys)
        {
            freed.slots = (uint64_t *)vmm_phys_to_virt((uintptr_t)freed.phys);
            freed.mask  = want_slots - 1;
        }
        else
        {
            /* kprintf: this is not a soft degradation. With no set the walk
             * cannot tell a doubly-mapped frame from a fresh one, so it frees
             * NO user data page at all and every one of them leaks. Silent
             * here meant the leak compounded until the PMM ran dry and
             * unrelated allocations began failing. */
            kprintf("[VMM] ERROR: dedup set alloc failed (%zu pages) — LEAKING every "
                    "user data page of this context to avoid a double-free\n",
                    freed.pages);
        }
    }
    // When has_dedup is false we still walk the tables to free page-table
    // structures but SKIP freeing data pages (pmm_free) to prevent
    // double-free if two PTEs point to the same physical frame.

    if (g_vmm_paging_levels == 4) {
        /* 4-level: user-half = PML4[0..255]. Kernel-shared entries
         * 256..511 are left alone (they reference the kernel PML4
         * mirror shared across every cabin). */
        vmm_walk_free_pml4_user_(ctx->pml4, 256, &freed, 0ULL);
    } else {
        /* 5-level: user-half = PML5[0..255]; each non-NULL entry points
         * to a PML4 whose ALL 512 entries are user (since kernel lives
         * under PML5[511] in a separate PML4 mirror shared via the
         * create_context copy loop). Walk each subtree, free its
         * PML4 page, and zero the PML5 entry. */
        for (int p5 = 0; p5 < 256; p5++) {
            pte_t pml5_entry = ctx->pml4->entries[p5];
            if (!(pml5_entry & VMM_FLAG_PRESENT)) continue;
            uintptr_t pml4_phys_user = vmm_pte_to_phys(pml5_entry);
            page_table_t *pml4_user =
                (page_table_t *)vmm_phys_to_virt(pml4_phys_user);
            uint64_t pml5_va_term = (uint64_t)p5 << 48;
            vmm_walk_free_pml4_user_(pml4_user, 512, &freed, pml5_va_term);
            vmm_free_page_table(pml4_phys_user);
            ctx->pml4->entries[p5] = 0;
        }
    }

    /* The old Phase 2 (a second post-walk cross-core shootdown) and Phase 3 (a
     * scan over ALL of installed RAM to bulk-free the marked frames) are both
     * gone: the Phase 0 quiesce flushed every core and evicted them off this
     * CR3, and the walk now frees each mapped data page inline. Teardown cost is
     * O(mapped pages), not O(installed RAM). Only the dedup scratch remains. */
    if (freed.phys)
        pmm_free(freed.phys, freed.pages);

    debug_printf("[VMM] User space tables freed\n");
}

void vmm_destroy_context(vmm_context_t *ctx)
{
    if (!ctx || ctx == kernel_context)
        return;

    uintptr_t saved_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(saved_cr3));
    uintptr_t saved_pml4 = saved_cr3 & ~0xFFFULL;

    // switch to kernel CR3 before walking page tables: user contexts have split PD
    // entries that may not identity-map all physical addresses used by other contexts'
    // page table structures; kernel context has pristine 2MB identity mapping
    uintptr_t destroyed_pml4 = ctx->pml4_phys;
    uintptr_t kernel_pml4 = kernel_context->pml4_phys;

    if (saved_pml4 != kernel_pml4)
    {
        uint64_t kernel_cr3 = vmm_build_cr3_noflush(kernel_context);
        asm volatile("mov %0, %%cr3" ::"r"(kernel_cr3) : "memory");
    }

    spin_lock(&ctx->lock);

    vmm_free_user_space_tables(ctx);

    /* After vmm_free_user_space_tables does its TLB shootdown, the shoot-
     * down only flushes the CURRENT CR3's PCID (kernel PCID 0).  With PCID
     * enabled, each PCID has its own TLB partition.  We must explicitly
     * expunge the user process's PCID X partition before releasing the PCID
     * for reuse.  Write the (now-empty) user PML4 with PCID X and NO-FLUSH
     * bit clear — the CPU then invalidates every TLB entry tagged PCID X. */
    if (g_pcid_active && ctx->pcid != 0 && ctx->pml4_phys)
    {
        uint64_t flush_cr3 = ctx->pml4_phys | (uint64_t)ctx->pcid; /* NOFLUSH=0 */
        asm volatile("mov %0, %%cr3" ::"r"(flush_cr3) : "memory");
        /* Immediately switch back to kernel. */
        uint64_t kernel_cr3 = kernel_context->pml4_phys; /* kernel PCID 0, NOFLUSH=0 */
        asm volatile("mov %0, %%cr3" ::"r"(kernel_cr3) : "memory");
    }

    if (ctx->pml4_phys)
    {
        vmm_free_page_table(ctx->pml4_phys);
    }

    spin_unlock(&ctx->lock);

    uint16_t freed_pcid = ctx->pcid;
    kfree(ctx);
    pcid_release(freed_pcid);

    if (saved_pml4 != destroyed_pml4 && saved_pml4 != kernel_pml4)
    {
        uint64_t restore_cr3 = saved_cr3;
        if (g_pcid_active)
            restore_cr3 |= CR3_NOFLUSH;
        asm volatile("mov %0, %%cr3" ::"r"(restore_cr3) : "memory");
    }

    if (global_stats.total_contexts > 0)
        atomic_fetch_sub_u64((volatile uint64_t *)&global_stats.total_contexts, 1);
}

vmm_context_t *vmm_get_kernel_context(void)
{
    return kernel_context;
}

/* ===========================================================================
 * Shared-physical registry — see vmm.h for the rationale. Small fixed-size
 * array because the set of shared kernel pages is bounded and known at
 * design time (cpu_caps, ClockBoard, future vDSO-style pages — never more
 * than a handful).
 * =========================================================================== */
#define VMM_SHARED_PHYS_MAX 8

static uint64_t      g_shared_phys[VMM_SHARED_PHYS_MAX];
static uint8_t       g_shared_phys_count = 0;
static spinlock_t    g_shared_phys_lock;
static uint8_t       g_shared_phys_lock_inited = 0;

static inline void shared_phys_lock_init_once(void)
{
    if (!g_shared_phys_lock_inited) {
        spinlock_init(&g_shared_phys_lock);
        g_shared_phys_lock_inited = 1;
    }
}

bool vmm_register_shared_phys(uint64_t phys)
{
    if (phys == 0) return false;
    shared_phys_lock_init_once();

    spin_lock(&g_shared_phys_lock);
    /* Idempotent — registering the same page twice is a silent success. */
    for (uint8_t i = 0; i < g_shared_phys_count; i++) {
        if (g_shared_phys[i] == phys) {
            spin_unlock(&g_shared_phys_lock);
            return true;
        }
    }
    if (g_shared_phys_count >= VMM_SHARED_PHYS_MAX) {
        spin_unlock(&g_shared_phys_lock);
        debug_printf("[VMM] shared-phys registry full (%u entries)\n",
                     VMM_SHARED_PHYS_MAX);
        return false;
    }
    g_shared_phys[g_shared_phys_count++] = phys;
    spin_unlock(&g_shared_phys_lock);
    return true;
}

bool vmm_is_shared_phys(uint64_t phys)
{
    if (phys == 0) return false;
    if (!g_shared_phys_lock_inited) return false;  /* nothing registered yet */
    /* Hot path — called per-PTE during process_destroy. The lock is held
     * very briefly; alternative would be RCU, but the registry is
     * append-only after boot so this is fine. */
    spin_lock(&g_shared_phys_lock);
    uint8_t  count = g_shared_phys_count;
    bool     hit   = false;
    for (uint8_t i = 0; i < count; i++) {
        if (g_shared_phys[i] == phys) { hit = true; break; }
    }
    spin_unlock(&g_shared_phys_lock);
    return hit;
}

vmm_context_t *vmm_get_current_context(void)
{
    return current_context ? current_context : kernel_context;
}

void vmm_switch_context(vmm_context_t *ctx)
{
    if (!ctx || !ctx->pml4_phys)
        return;

    current_context = ctx;
    if (g_pcid_active)
    {
        // NOFLUSH: preserve TLB entries from other PCIDs
        asm volatile("mov %0, %%cr3" : : "r"(vmm_build_cr3_noflush(ctx)) : "memory");
    }
    else
    {
        asm volatile("mov %0, %%cr3" : : "r"(ctx->pml4_phys) : "memory");
        vmm_flush_tlb();
    }
}

/* TME-MK aware variant — uses vmm_make_pte_with_keyid so the wider
 * vmm_pte_addr_mask_with_keyid is applied, preserving KeyID bits in
 * the PTE.phys field. Behaves identically to vmm_map_page when MK is
 * inactive (the two masks are equal). The check below verifies the
 * passed phys (without KeyID) fits within the raw MAXPHYADDR — if a
 * caller accidentally passes a non-canonical phys, we still catch it
 * via the existing vmm_map_page check. */
vmm_map_result_t vmm_map_page_with_keyid(vmm_context_t *ctx, uintptr_t virt_addr,
                                          uintptr_t phys_addr_with_keyid,
                                          uint64_t flags)
{
    vmm_map_result_t result = {0};

    if (!ctx) {
        result.error_msg = "Invalid context";
        return result;
    }

    if (!vmm_is_page_aligned(virt_addr) ||
        !vmm_is_page_aligned(phys_addr_with_keyid & vmm_pte_addr_mask_with_keyid)) {
        result.error_msg = "Address not page-aligned";
        return result;
    }

    spin_lock(&ctx->lock);

    pte_t *pte = vmm_get_or_create_pte(ctx, virt_addr);
    if (!pte) {
        spin_unlock(&ctx->lock);
        result.error_msg = "Failed to get/create page table entry";
        return result;
    }

    bool was_present = (*pte & VMM_FLAG_PRESENT) != 0;
    if (was_present) {
        /* Disallow remap with KeyID-bearing PTE — TME-MK semantics
         * mean a remap would change the encryption key for already-
         * mapped data, almost certainly a bug. */
        spin_unlock(&ctx->lock);
        result.error_msg = "vmm_map_page_with_keyid: page already mapped (no implicit remap)";
        return result;
    }

    *pte = vmm_make_pte_with_keyid(phys_addr_with_keyid, flags);

    ctx->mapped_pages++;
    if (flags & VMM_FLAG_USER) {
        ctx->user_pages++;
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.user_mapped_pages, 1);
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.total_mapped_pages, 1);
    } else {
        ctx->kernel_pages++;
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.kernel_mapped_pages, 1);
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.total_mapped_pages, 1);
    }

    spin_unlock(&ctx->lock);
    /* No was_present-true path; first-time map only — no TLB shootdown
     * needed (no stale entry can exist on any core). */

    result.success = true;
    result.virt_addr = virt_addr;
    result.phys_addr = phys_addr_with_keyid;  /* includes KeyID for caller info */
    result.pages_mapped = 1;
    return result;
}

vmm_map_result_t vmm_map_page(vmm_context_t *ctx, uintptr_t virt_addr,
                              uintptr_t phys_addr, uint64_t flags)
{
    vmm_map_result_t result = {0};

    if (!ctx)
    {
        result.error_msg = "Invalid context";
        return result;
    }

    if (!vmm_is_page_aligned(virt_addr) || !vmm_is_page_aligned(phys_addr))
    {
        result.error_msg = "Address not page-aligned";
        return result;
    }

    spin_lock(&ctx->lock);

    pte_t *pte = vmm_get_or_create_pte(ctx, virt_addr);
    if (!pte)
    {
        spin_unlock(&ctx->lock);
        result.error_msg = "Failed to get/create page table entry";
        return result;
    }

    /* Track whether this is a REMAP (PTE already present) vs first-time
     * map. Intel SDM Vol 3A §4.10.2.1 — only existing TLB entries need
     * invalidation. First-time maps create no stale TLB entry on any
     * core, so the cross-core shootdown below can be skipped entirely.
     * This keeps the M5 correctness fix (kernel-context maps now flush
     * remote TLBs on remap) without paying an IPI broadcast for every
     * heap demand-page / MMIO mapping / framebuffer page during boot. */
    bool was_present = (*pte & VMM_FLAG_PRESENT) != 0;

    if (was_present)
    {
        uintptr_t existing_phys = vmm_pte_to_phys(*pte);
        uint64_t existing_flags = vmm_pte_to_flags(*pte);

        if (existing_phys == phys_addr && existing_flags == flags)
        {
            spin_unlock(&ctx->lock);
            result.success = true;
            result.virt_addr = virt_addr;
            result.phys_addr = phys_addr;
            result.pages_mapped = 1;
            return result;
        }

        // Allow remapping identity-mapped kernel pages (phys == virt, supervisor-only)
        // Cabin setup overrides these for CabinInfo/PocketRing at 0x1000-0xBFFF
        bool is_identity_map = (existing_phys == virt_addr) &&
                               !(existing_flags & VMM_FLAG_USER);

        if (!is_identity_map)
        {
            spin_unlock(&ctx->lock);
            char error_buf[256];
            ksnprintf(error_buf, sizeof(error_buf),
                      "Page already mapped (virt=0x%p: existing_phys=0x%p, new_phys=0x%p, existing_flags=0x%llx, new_flags=0x%llx)",
                      (void *)virt_addr, (void *)existing_phys, (void *)phys_addr,
                      (unsigned long long)existing_flags, (unsigned long long)flags);
            debug_printf("[VMM] %s\n", error_buf);
            result.error_msg = "Page already mapped with different address/flags";
            return result;
        }
    }

    *pte = vmm_make_pte(phys_addr, flags);

    ctx->mapped_pages++;
    if (flags & VMM_FLAG_USER)
    {
        ctx->user_pages++;
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.user_mapped_pages, 1);
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.total_mapped_pages, 1);
    }
    else
    {
        ctx->kernel_pages++;
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.kernel_mapped_pages, 1);
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.total_mapped_pages, 1);
    }

    spin_unlock(&ctx->lock);

    /* TLB invalidation. Intel SDM Vol 3A §4.10.2.1: only TLB entries that
     * actually existed need to be invalidated. First-time map (was_present
     * == false) → no stale entry on any core → skip the flush entirely.
     *
     * Remap → must flush. For shared kernel VA (kernel_context / GLOBAL),
     * every online core may hold the stale entry → cross-core shootdown.
     * For user VA, only cores currently on this PML4 → vmm_shootdown_page
     * filters by CR3 match. Both collapse to local invlpg on single-core. */
    if (was_present)
    {
        if (ctx == kernel_context || (flags & VMM_FLAG_GLOBAL))
            vmm_shootdown_page(ctx, virt_addr);
        else
            vmm_flush_tlb_page(virt_addr);
    }

    result.success = true;
    result.virt_addr = virt_addr;
    result.phys_addr = phys_addr;
    result.pages_mapped = 1;

    return result;
}

vmm_map_result_t vmm_map_pages(vmm_context_t *ctx, uintptr_t virt_addr,
                               uintptr_t phys_addr, size_t page_count, uint64_t flags)
{
    vmm_map_result_t result = {0};
    result.virt_addr = virt_addr;
    result.phys_addr = phys_addr;

    for (size_t i = 0; i < page_count; i++)
    {
        vmm_map_result_t single_result = vmm_map_page(ctx,
                                                      virt_addr + i * VMM_PAGE_SIZE,
                                                      phys_addr + i * VMM_PAGE_SIZE,
                                                      flags);

        if (!single_result.success)
        {
            for (size_t j = 0; j < i; j++)
            {
                vmm_unmap_page(ctx, virt_addr + j * VMM_PAGE_SIZE);
            }
            result.error_msg = single_result.error_msg;
            return result;
        }

        result.pages_mapped++;
    }

    result.success = true;
    return result;
}

bool vmm_unmap_page(vmm_context_t *ctx, uintptr_t virt_addr)
{
    if (!ctx || !vmm_is_page_aligned(virt_addr))
        return false;

    spin_lock(&ctx->lock);

    pte_t *pte = vmm_get_pte(ctx, virt_addr);
    if (!pte || !(*pte & VMM_FLAG_PRESENT))
    {
        spin_unlock(&ctx->lock);
        return false;
    }

    uint64_t flags = vmm_pte_to_flags(*pte);
    if (flags & VMM_FLAG_USER)
    {
        if (ctx->user_pages > 0)
            ctx->user_pages--;
        if (global_stats.user_mapped_pages > 0)
            atomic_fetch_sub_u64((volatile uint64_t *)&global_stats.user_mapped_pages, 1);
        if (global_stats.total_mapped_pages > 0)
            atomic_fetch_sub_u64((volatile uint64_t *)&global_stats.total_mapped_pages, 1);
    }
    else
    {
        if (ctx->kernel_pages > 0)
            ctx->kernel_pages--;
        if (global_stats.kernel_mapped_pages > 0)
            atomic_fetch_sub_u64((volatile uint64_t *)&global_stats.kernel_mapped_pages, 1);
        if (global_stats.total_mapped_pages > 0)
            atomic_fetch_sub_u64((volatile uint64_t *)&global_stats.total_mapped_pages, 1);
    }

    if (ctx->mapped_pages > 0)
        ctx->mapped_pages--;

    *pte = 0;

    spin_unlock(&ctx->lock);

    // Cross-core TLB shootdown: invalidate on all cores sharing this context.
    vmm_shootdown_page(ctx, virt_addr);

    return true;
}

bool vmm_unmap_pages(vmm_context_t *ctx, uintptr_t virt_addr, size_t page_count)
{
    bool success = true;

    // Unmap locally (each vmm_unmap_page does local invlpg via shootdown).
    // For bulk unmap, a single batched shootdown is more efficient —
    // but vmm_unmap_page already handles cross-core per page. This is
    // acceptable for small page counts. Large bulk unmaps (vmm_destroy_context)
    // don't call this — they reload CR3 entirely.
    for (size_t i = 0; i < page_count; i++)
    {
        if (!vmm_unmap_page(ctx, virt_addr + i * VMM_PAGE_SIZE))
        {
            success = false;
        }
    }

    return success;
}

/* Try to take `size_aligned` bytes from the kernel-MMIO free list.
 *
 * First-fit on the sorted list. If the chosen node is larger than
 * needed, the trailing slice is reinserted as a new node (we keep
 * the head address so reclaimed regions migrate toward the front
 * over time). Returns 0 if no fitting block exists — caller then
 * bumps `kernel_mmio_current`.
 *
 * Caller must hold `kernel_mmio_lock`. */
static uintptr_t mmio_free_list_take_locked(size_t size_aligned)
{
    MmioFreeNode **pp = &kernel_mmio_free_head;
    while (*pp) {
        MmioFreeNode *n = *pp;
        if (n->size >= size_aligned) {
            uintptr_t base = n->base;
            if (n->size == size_aligned) {
                *pp = n->next;
                kfree(n);
            } else {
                n->base += size_aligned;
                n->size -= size_aligned;
            }
            return base;
        }
        pp = &n->next;
    }
    return 0;
}

/* Insert a reclaimed range into the sorted free list and coalesce
 * with any directly adjacent neighbours. Caller must hold the lock.
 *
 * Coalescing keeps the list short and prevents fragmentation: after
 * a few full-cycle allocate/free passes the list shrinks back to one
 * big block instead of growing without bound. */
static void mmio_free_list_insert_locked(uintptr_t base, size_t size_aligned)
{
    /* Special case: the freed region is the immediately preceding
     * top-of-bump, give it back to the cursor instead of fragmenting
     * the free list — keeps the common shutdown-then-restart pattern
     * fully bump-recyclable. */
    if (base + size_aligned == kernel_mmio_current) {
        kernel_mmio_current = base;
        return;
    }

    MmioFreeNode *node = (MmioFreeNode *)kmalloc(sizeof(MmioFreeNode));
    if (!node) {
        /* Loss of the freed range is graceful: nothing crashes, the
         * VA simply stays "in use" forever. Logged so the operator
         * notices systematic leakage if it happens repeatedly. */
        debug_printf("[VMM] WARN: kmalloc MmioFreeNode failed — "
                     "leaking 0x%lx bytes of MMIO VA at 0x%lx\n",
                     (unsigned long)size_aligned, (unsigned long)base);
        return;
    }
    node->base = base;
    node->size = size_aligned;
    node->next = NULL;

    /* Insert sorted by base. */
    MmioFreeNode **pp = &kernel_mmio_free_head;
    while (*pp && (*pp)->base < base) pp = &(*pp)->next;
    node->next = *pp;
    *pp = node;

    /* Coalesce forward. */
    if (node->next && node->base + node->size == node->next->base) {
        MmioFreeNode *succ = node->next;
        node->size += succ->size;
        node->next  = succ->next;
        kfree(succ);
    }

    /* Coalesce backward — walk again from head to find predecessor.
     * O(N) but the list stays short (~10s of nodes max in practice). */
    if (pp != &kernel_mmio_free_head) {
        MmioFreeNode *pred = kernel_mmio_free_head;
        while (pred->next != node) pred = pred->next;
        if (pred->base + pred->size == node->base) {
            pred->size += node->size;
            pred->next  = node->next;
            kfree(node);
        }
    }
}

volatile void *vmm_map_mmio(uintptr_t phys_addr, size_t size, uint64_t flags)
{
    if (size == 0)
    {
        vmm_set_error("vmm_map_mmio: size is zero");
        return NULL;
    }

    // block mapping if range overlaps E820 USABLE RAM to prevent accidental MMIO over managed RAM
    // Exception: legacy BIOS area (<1MB) contains ACPI tables, EBDA, BIOS ROM that need mapping
    bool is_legacy_bios = (phys_addr + size <= 0x100000);
    if (!is_legacy_bios && pmm_is_usable_ram(phys_addr, size))
    {
        vmm_set_error("vmm_map_mmio: physical address overlaps USABLE RAM");
        debug_printf("[VMM] ERROR: vmm_map_mmio phys=0x%llx size=0x%llx overlaps USABLE RAM from E820\n",
                     phys_addr, (uint64_t)size);
        debug_printf("[VMM]        MMIO mappings cannot overlap E820_USABLE regions\n");
        return NULL;
    }

    uintptr_t phys_aligned = vmm_page_align_down(phys_addr);
    size_t offset = phys_addr - phys_aligned;
    size_t size_aligned = vmm_page_align_up(size + offset);
    size_t page_count = size_aligned / VMM_PAGE_SIZE;

    spin_lock(&kernel_mmio_lock);

    /* Try the free list first — recycled VA cuts both fragmentation
     * and total kernel-MMIO usage. Fall through to bump on miss. */
    uintptr_t virt_base = mmio_free_list_take_locked(size_aligned);
    if (virt_base == 0) {
        virt_base = kernel_mmio_current;

        if (virt_base + size_aligned > VMM_KERNEL_MMIO_BASE + VMM_KERNEL_MMIO_SIZE)
        {
            spin_unlock(&kernel_mmio_lock);
            vmm_set_error("vmm_map_mmio: kernel MMIO region exhausted");
            debug_printf("[VMM] ERROR: MMIO region exhausted (need %zu bytes)\n", size_aligned);
            return NULL;
        }

        kernel_mmio_current += size_aligned;
    }
    spin_unlock(&kernel_mmio_lock);

    uint64_t mmio_flags = flags | VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH;

    vmm_context_t *ctx = vmm_get_kernel_context();
    vmm_map_result_t result = vmm_map_pages(ctx, virt_base, phys_aligned,
                                            page_count, mmio_flags);

    if (!result.success)
    {
        debug_printf("[VMM] ERROR: vmm_map_mmio: failed to map pages: %s\n", result.error_msg);
        /* Return the just-allocated VA range to the recycler so the
         * failure doesn't leak it. */
        spin_lock(&kernel_mmio_lock);
        mmio_free_list_insert_locked(virt_base, size_aligned);
        spin_unlock(&kernel_mmio_lock);
        return NULL;
    }

    /* Register a MemTag region so drivers can query MMIO mappings via tag
     * algebra (`MemTagAnd("cache:uc", "purpose:mmio")` finds every
     * uncacheable MMIO mapping). Phys may live outside id_by_page (device
     * registers above mem_end) — registry tracks it regardless; only the
     * O(1) phys lookup degrades to "not in id_by_page". */
    uint32_t rid = MemRegionCreate(phys_aligned, virt_base, NULL, page_count,
                                    MEMTAG_REGION_FLAG_KERNEL |
                                    MEMTAG_REGION_FLAG_PHYSICAL |
                                    MEMTAG_REGION_FLAG_VIRTUAL);
    if (rid != MEMTAG_INVALID_REGION_ID) {
        MemTagApply(rid, "purpose:mmio");
        MemTagApply(rid, "cache:uc");
    }

    return (volatile void *)(virt_base + offset);
}

/*
 * vmm_map_framebuffer — map a GOP linear framebuffer with Write Combining (WC).
 *
 * WC lets the CPU coalesce sequential writes into cache-line bursts before
 * flushing to the bus — 10–50× faster than UC for framebuffer blits.
 *
 * PAT selection: PCD=1, PAT_bit=1, PWT=0 → PAT index 6.
 * vmm_pat_init() (called from vmm_init) must have set PA6 = WC (type 1).
 *
 * Virtual address is bump-allocated from the same MMIO region used by
 * vmm_map_mmio, so the two functions never overlap.
 */
volatile void *vmm_map_framebuffer(uintptr_t phys_addr, size_t size)
{
    if (size == 0) {
        vmm_set_error("vmm_map_framebuffer: size is zero");
        return NULL;
    }

    bool is_legacy_bios = (phys_addr + size <= 0x100000);
    if (!is_legacy_bios && pmm_is_usable_ram(phys_addr, size)) {
        vmm_set_error("vmm_map_framebuffer: physical address overlaps USABLE RAM");
        debug_printf("[VMM] ERROR: vmm_map_framebuffer phys=0x%lx size=0x%lx overlaps USABLE RAM\n",
                     (unsigned long)phys_addr, (unsigned long)size);
        return NULL;
    }

    uintptr_t phys_aligned = vmm_page_align_down(phys_addr);
    size_t    offset       = phys_addr - phys_aligned;
    size_t    size_aligned = vmm_page_align_up(size + offset);
    size_t    page_count   = size_aligned / VMM_PAGE_SIZE;

    spin_lock(&kernel_mmio_lock);
    uintptr_t virt_base = kernel_mmio_current;
    if (virt_base + size_aligned > VMM_KERNEL_MMIO_BASE + VMM_KERNEL_MMIO_SIZE) {
        spin_unlock(&kernel_mmio_lock);
        vmm_set_error("vmm_map_framebuffer: kernel MMIO region exhausted");
        debug_printf("[VMM] ERROR: vmm_map_framebuffer: MMIO region exhausted\n");
        return NULL;
    }
    kernel_mmio_current += size_aligned;
    spin_unlock(&kernel_mmio_lock);

    /*
     * WC flag combination: PCD=1 (CACHE_DISABLE), PAT_bit=1, PWT=0 (no WRITE_THROUGH)
     * → PAT index = (PAT_bit<<2)|(PCD<<1)|PWT = 4|2|0 = 6 = WC (programmed by
     * vmm_pat_init). vmm_wc_pte_flags hands back that pair only where the PAT
     * was actually programmed; on a CPU without PAT, bit 7 is reserved and the
     * pair it returns instead is strong UC.
     */
    uint64_t wc_flags = VMM_FLAGS_KERNEL_RW | vmm_wc_pte_flags();

    vmm_context_t    *ctx    = vmm_get_kernel_context();
    vmm_map_result_t  result = vmm_map_pages(ctx, virt_base, phys_aligned,
                                              page_count, wc_flags);
    if (!result.success) {
        debug_printf("[VMM] ERROR: vmm_map_framebuffer: vmm_map_pages failed: %s\n",
                     result.error_msg);
        spin_lock(&kernel_mmio_lock);
        if (kernel_mmio_current == virt_base + size_aligned)
            kernel_mmio_current = virt_base;
        spin_unlock(&kernel_mmio_lock);
        return NULL;
    }

    debug_printf("[VMM] vmm_map_framebuffer: phys=0x%lx size=0x%lx → virt=0x%lx (WC)\n",
                 (unsigned long)phys_addr, (unsigned long)size,
                 (unsigned long)(virt_base + offset));

    uint32_t rid = MemRegionCreate(phys_aligned, virt_base, NULL, page_count,
                                    MEMTAG_REGION_FLAG_KERNEL |
                                    MEMTAG_REGION_FLAG_PHYSICAL |
                                    MEMTAG_REGION_FLAG_VIRTUAL);
    if (rid != MEMTAG_INVALID_REGION_ID) {
        MemTagApply(rid, "purpose:framebuffer");
        MemTagApply(rid, "cache:wc");
    }

    return (volatile void *)(virt_base + offset);
}

void vmm_unmap_mmio(volatile void *virt_addr, size_t size)
{
    if (!virt_addr || size == 0)
        return;

    void *virt = (void *)virt_addr;
    /* Align the original allocation: vmm_map_mmio aligned the size
     * INCLUDING the sub-page offset of phys_addr. We don't have the
     * original phys here, but the page-aligned virt + page-up size
     * recovers the same range as long as the caller passed back the
     * exact pointer vmm_map_mmio returned. */
    uintptr_t virt_base = vmm_page_align_down((uintptr_t)virt);
    size_t size_aligned = vmm_page_align_up(size + ((uintptr_t)virt - virt_base));
    size_t page_count = size_aligned / VMM_PAGE_SIZE;

    debug_printf("[VMM] vmm_unmap_mmio: virt=%p size=0x%llx pages=%zu\n",
                 virt, size, page_count);

    vmm_context_t *ctx = vmm_get_kernel_context();
    vmm_unmap_pages(ctx, virt_base, page_count);

    /* Reclaim the virtual address range. mmio_free_list_insert_locked
     * coalesces adjacent ranges and folds top-of-bump back into the
     * cursor, so steady-state allocate/free workloads never leak. */
    spin_lock(&kernel_mmio_lock);
    mmio_free_list_insert_locked(virt_base, size_aligned);
    spin_unlock(&kernel_mmio_lock);
}

void *vmm_alloc_pages(vmm_context_t *ctx, size_t page_count, uint64_t flags)
{
    if (!ctx || page_count == 0)
    {
        debug_printf("[VMM] vmm_alloc_pages: invalid parameters (ctx=%p, count=%zu)\n", ctx, page_count);
        return NULL;
    }

    debug_printf("[VMM] vmm_alloc_pages: requesting %zu pages with flags 0x%llx\n", page_count, (unsigned long long)flags);

    void *phys_pages = pmm_alloc(page_count);
    if (!phys_pages)
    {
        vmm_set_error("Failed to allocate physical pages");
        debug_printf("[VMM] PMM allocation failed for %zu pages\n", page_count);
        return NULL;
    }

    debug_printf("[VMM] PMM allocated %zu pages at physical 0x%p\n", page_count, phys_pages);

    uintptr_t phys_base = (uintptr_t)phys_pages;
    uintptr_t virt_base;

    if (flags & VMM_FLAG_USER)
    {
        virt_base = vmm_find_free_region(ctx, vmm_pages_to_size(page_count),
                                         VMM_USER_BASE, VMM_USER_STACK_TOP);
        if (!virt_base)
        {
            pmm_free(phys_pages, page_count);
            vmm_set_error("Failed to find user virtual address space");
            debug_printf("[VMM] Failed to find user virtual space for %zu pages\n", page_count);
            return NULL;
        }
        debug_printf("[VMM] Found user virtual space at 0x%p\n", (void *)virt_base);
    }
    else
    {
        spin_lock(&kernel_heap_lock);
        virt_base = kernel_heap_current;

        debug_printf("[VMM] Current kernel heap pointer: 0x%p\n", (void *)kernel_heap_current);
        debug_printf("[VMM] Kernel heap base: 0x%p\n", (void *)VMM_KERNEL_HEAP_BASE);
        debug_printf("[VMM] Kernel heap size: 0x%llx\n", (unsigned long long)VMM_KERNEL_HEAP_SIZE);

        if (virt_base + vmm_pages_to_size(page_count) > VMM_KERNEL_HEAP_BASE + VMM_KERNEL_HEAP_SIZE)
        {
            spin_unlock(&kernel_heap_lock);
            pmm_free(phys_pages, page_count);
            vmm_set_error("Kernel heap exhausted");
            debug_printf("[VMM] ERROR: Kernel heap exhausted! Current: 0x%p, need: 0x%llx, limit: 0x%p\n",
                         (void *)virt_base, (unsigned long long)vmm_pages_to_size(page_count),
                         (void *)(VMM_KERNEL_HEAP_BASE + VMM_KERNEL_HEAP_SIZE));
            return NULL;
        }

        kernel_heap_current += vmm_pages_to_size(page_count);
        spin_unlock(&kernel_heap_lock);

        debug_printf("[VMM] Kernel allocation: virt=0x%p, phys=0x%p, pages=%zu\n",
                     (void *)virt_base, (void *)phys_base, page_count);
    }

    for (size_t i = 0; i < page_count; i++)
    {
        uintptr_t virt_addr = virt_base + i * VMM_PAGE_SIZE;
        uintptr_t phys_addr = phys_base + i * VMM_PAGE_SIZE;

        debug_printf("[VMM] Mapping page %zu/%zu: virt=0x%p -> phys=0x%p\n",
                     i + 1, page_count, (void *)virt_addr, (void *)phys_addr);

        vmm_map_result_t result = vmm_map_page(ctx, virt_addr, phys_addr, flags);

        if (!result.success)
        {
            debug_printf("[VMM] ERROR: Failed to map page %zu/%zu (virt=0x%p, phys=0x%p): %s\n",
                         i + 1, page_count, (void *)virt_addr, (void *)phys_addr,
                         result.error_msg ? result.error_msg : "unknown error");

            for (size_t j = 0; j < i; j++)
            {
                vmm_unmap_page(ctx, virt_base + j * VMM_PAGE_SIZE);
            }

            pmm_free(phys_pages, page_count);
            vmm_set_error(result.error_msg);
            return NULL;
        }
    }

    debug_printf("[VMM] SUCCESS: Allocated %zu pages at virtual 0x%p\n", page_count, (void *)virt_base);
    return (void *)virt_base;
}

void vmm_free_pages(vmm_context_t *ctx, void *virt_addr, size_t page_count)
{
    if (!ctx || !virt_addr || page_count == 0)
        return;

    uintptr_t virt_base = (uintptr_t)virt_addr;

    uintptr_t *phys_addrs = kmalloc(page_count * sizeof(uintptr_t));
    if (phys_addrs)
    {
        for (size_t i = 0; i < page_count; i++)
        {
            phys_addrs[i] = vmm_virt_to_phys(ctx, virt_base + i * VMM_PAGE_SIZE);
        }
    }

    vmm_unmap_pages(ctx, virt_base, page_count);

    if (phys_addrs)
    {
        for (size_t i = 0; i < page_count; i++)
        {
            if (phys_addrs[i])
            {
                pmm_free((void *)phys_addrs[i], 1);
            }
        }
        kfree(phys_addrs);
    }
}

void *vmalloc(size_t size)
{
    if (size == 0)
    {
        debug_printf("[VMM] vmalloc: size is 0\n");
        return NULL;
    }

    if (!vmm_initialized)
    {
        debug_printf("[VMM] vmalloc: VMM not initialized\n");
        return NULL;
    }

    size_t page_count = vmm_size_to_pages(size);
    debug_printf("[VMM] vmalloc: requested %zu bytes (%zu pages)\n", size, page_count);

    vmm_context_t *ctx = vmm_get_current_context();
    if (!ctx)
    {
        debug_printf("[VMM] vmalloc: no current context\n");
        return NULL;
    }

    void *virt = vmm_alloc_pages(ctx, page_count, VMM_FLAGS_KERNEL_RW);
    if (!virt)
    {
        debug_printf("[VMM] vmalloc FAILED: %s\n", vmm_get_last_error());
        return NULL;
    }

    uintptr_t phys_first = vmm_virt_to_phys(ctx, (uintptr_t)virt);
    debug_printf("[VMM] vmalloc: allocated virt=%p phys=%p pages=%zu\n",
                 virt, (void *)phys_first, page_count);

    vmalloc_entry_t *ent = kmalloc(sizeof(vmalloc_entry_t));
    if (ent)
    {
        ent->virt_base = virt;
        ent->pages = page_count;
        spin_lock(&vmalloc_lock);
        ent->next = vmalloc_list;
        vmalloc_list = ent;
        spin_unlock(&vmalloc_lock);
        debug_printf("[VMM] vmalloc: recorded allocation (%p, %zu pages)\n", virt, page_count);
    }
    else
    {
        debug_printf("[VMM] vmalloc: WARNING: could not record allocation for vfree()\n");
    }

    debug_printf("[VMM] vmalloc SUCCESS: %p (%zu pages)\n", virt, page_count);
    return virt;
}

void *vzalloc(size_t size)
{
    void *addr = vmalloc(size);
    if (addr)
    {
        memset(addr, 0, size);
    }
    return addr;
}

void vfree(void *addr)
{
    if (!addr)
    {
        debug_printf("[VMM] vfree: null pointer, nothing to free\n");
        return;
    }

    spin_lock(&vmalloc_lock);
    vmalloc_entry_t *prev = NULL;
    vmalloc_entry_t *cur = vmalloc_list;

    while (cur)
    {
        if (cur->virt_base == addr)
        {
            if (prev)
                prev->next = cur->next;
            else
                vmalloc_list = cur->next;
            spin_unlock(&vmalloc_lock);

            debug_printf("[VMM] vfree: freeing allocation at %p (%zu pages)\n",
                         addr, cur->pages);

            vmm_free_pages(vmm_get_current_context(), addr, cur->pages);
            kfree(cur);

            debug_printf("[VMM] vfree: successfully freed %p\n", addr);
            return;
        }
        prev = cur;
        cur = cur->next;
    }

    spin_unlock(&vmalloc_lock);

    debug_printf("[VMM] vfree: allocation not found in list, fallback free at %p\n", addr);

    /* The fallback returns ONE 4 KiB frame, so it must see a 4 KiB leaf and
     * nothing else. vmm_virt_to_phys answers for a 2 MiB / 1 GiB leaf too
     * (see its note in vmm.h), and that answer names a byte inside an
     * order-9 or order-18 PMM block: giving it to pmm_free with a count of 1
     * would hand the buddy allocator a fragment of a block it still owns
     * whole. Walk the leaf here so the page size is part of the decision. */
    vmm_context_t *free_ctx = vmm_get_current_context();
    uint8_t leaf_level = 0;
    pte_t *leaf = vmm_get_leaf_pte(free_ctx, (uintptr_t)addr, &leaf_level);
    if (leaf && leaf_level == 1 && (*leaf & VMM_FLAG_PRESENT))
    {
        uintptr_t phys = vmm_pte_to_phys(*leaf) & VMM_PAGE_MASK;
        debug_printf("[VMM] vfree: unmapped single page virt=%p phys=%p\n", addr, (void *)phys);
        vmm_unmap_page(free_ctx, (uintptr_t)addr);
        pmm_free((void *)phys, 1);
    }
    else if (leaf && leaf_level > 1 && (*leaf & VMM_FLAG_PRESENT))
    {
        kprintf("[VMM] vfree: %p is covered by a %s leaf, not a vmalloc page "
                "— refusing to free\n",
                addr, leaf_level == 2 ? "2 MiB" : "1 GiB");
    }
    else
    {
        debug_printf("[VMM] vfree: WARNING: could not resolve physical address for %p\n", addr);
    }
}

/*
 * The three queries below answer about whichever leaf covers the address:
 * a 4 KiB PT entry, a 2 MiB PD leaf or a 1 GiB PDPT leaf.
 *
 * They used to walk vmm_get_pte, which stops at a LARGE_PAGE bit and hands
 * back NULL, so every one of them answered "nothing is mapped here" about a
 * live huge mapping. That answer was not merely incomplete, it was acted on:
 * vmm_find_free_region handed out VAs inside a live 2 MiB Bay leaf,
 * vmm_is_user_accessible called a cabin's own Bay page inaccessible, and the
 * demand-paging arms of vmm_handle_page_fault took "unmapped" as licence to
 * pmm_alloc and vmm_map_page over it — which demotes the huge leaf and
 * shadows one of its 4 KiB slices with a fresh frame.
 *
 * Reading the leaf's physical base needs the leaf's own mask, not the 4 KiB
 * one: in a PDE with PS=1 bit 12 is PAT and bits 20:13 are reserved, and in a
 * PDPTE with PS=1 bit 12 is PAT and bits 29:13 are reserved (Intel SDM Vol 3A
 * Tables 4-17 and 4-18). Masking with the page size clears the PAT bit along
 * with the reserved ones and leaves the address.
 */
static inline uintptr_t vmm_leaf_size_for_level(uint8_t level)
{
    if (level == 3) return 512ULL * VMM_LARGE_PAGE_2M_SIZE;   /* 1 GiB */
    if (level == 2) return VMM_LARGE_PAGE_2M_SIZE;            /* 2 MiB */
    return VMM_PAGE_SIZE;                                     /* 4 KiB */
}

uintptr_t vmm_virt_to_phys(vmm_context_t *ctx, uintptr_t virt_addr)
{
    if (!ctx)
        return 0;

    spin_lock(&ctx->lock);

    uint8_t level = 0;
    pte_t *leaf = vmm_get_leaf_pte(ctx, virt_addr, &level);
    if (!leaf || level == 0 || !(*leaf & VMM_FLAG_PRESENT))
    {
        spin_unlock(&ctx->lock);
        return 0;
    }

    uintptr_t leaf_size = vmm_leaf_size_for_level(level);
    uintptr_t phys_base = vmm_pte_to_phys(*leaf) & ~(leaf_size - 1);
    uintptr_t offset = virt_addr & (leaf_size - 1);

    spin_unlock(&ctx->lock);

    return phys_base + offset;
}

bool vmm_is_mapped(vmm_context_t *ctx, uintptr_t virt_addr)
{
    return vmm_virt_to_phys(ctx, virt_addr) != 0;
}

/*
 * vmm_user_page_state — what already lives at `page_addr` in a cabin, judged
 * against the terms `vmm_ensure_user_page` was asked for.
 *
 *    1  a present leaf maps it, user-accessible, and writable if that was asked
 *    0  nothing maps it — the caller may map it
 *   -1  something maps it, but NOT on those terms
 *
 * The walk is vmm_get_leaf_pte, not vmm_get_pte, so a 2 MiB or 1 GiB leaf
 * counts as the mapping it is instead of being reported absent — reporting it
 * absent is what would send vmm_get_or_create_pte into vmm_demote_large_entry
 * and split a live huge mapping on a path meant only to fill a hole.
 *
 * The -1 case is the one that matters for safety. `writable` is not decoration:
 * the caller's next move is to write into that page through the Pull Map alias,
 * which is supervisor-writable and carries no U/S, so the hardware checks
 * neither the cabin's R/W bit nor CR0.WP nor CR4.SMAP on that store (Intel SDM
 * Vol 3A §5.6/§5.6.1 — access rights belong to the translation the store
 * actually uses). Answering "already there" for a page the cabin holds
 * read-only therefore hands the kernel's own pen to whoever chose the address.
 * Both callers choose it from a ring header the cabin can write: the slot VA is
 * hdr.slots_base + (idx % hdr.slot_count_max) * hdr.slot_size, and all three of
 * those fields live in the header page that vmm_map_result_ring /
 * vmm_map_touch_ring map PRESENT|WRITABLE|USER. A cabin that points slots_base
 * at its ClockBoard page (PRESENT|USER, deliberately not WRITABLE, one physical
 * frame shared by every cabin and written by the timer) would otherwise have
 * the kernel publish a 32-byte slot straight into it.
 */
static int vmm_user_page_state(vmm_context_t *ctx, uintptr_t page_addr, bool writable)
{
    uint8_t level = 0;
    pte_t *leaf = vmm_get_leaf_pte(ctx, page_addr, &level);
    if (!leaf || level == 0) return 0;

    pte_t entry = __atomic_load_n(leaf, __ATOMIC_ACQUIRE);
    if (!(entry & VMM_FLAG_PRESENT)) return 0;
    if (!(entry & VMM_FLAG_USER)) return -1;
    if (writable && !(entry & VMM_FLAG_WRITABLE)) return -1;
    return 1;
}

int vmm_ensure_user_page(vmm_context_t *ctx, uintptr_t user_vaddr, bool writable)
{
    if (!ctx) return -1;

    uintptr_t page_addr = user_vaddr & ~(VMM_PAGE_SIZE - 1);
    int state = vmm_user_page_state(ctx, page_addr, writable);
    if (state == 1) return 0;
    if (state < 0) {
        /* Mapped, but not on the terms asked for. Refuse rather than map over
         * it or write through it: see vmm_user_page_state. Quiet on release —
         * an unprivileged cabin can provoke this at will by pointing its ring
         * header at a page it holds read-only, and the caller says the refusal
         * out loud in its own words (krp_refused / g_ktr_map_fail). */
        debug_printf("[VMM] ensure-user-page refuse: cabin page 0x%lx is mapped but not %s\n",
                     (unsigned long)page_addr,
                     writable ? "user-writable" : "user-accessible");
        return -1;
    }

    void *phys = pmm_alloc_zero(1);
    if (!phys) return -1;

    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE;
    if (writable) flags |= VMM_FLAG_WRITABLE;

    vmm_map_result_t r = vmm_map_page(ctx, page_addr, (uintptr_t)phys, flags);
    if (!r.success) {
        /* Race tolerance: another producer mapped the same page between our
         * probe and vmm_map_page. vmm_map_page rejects the duplicate; free our
         * spare phys page and report success only if what the winner installed
         * carries the same terms. Without this, MPSC producers landing on a
         * fresh slot page will see spurious failures. */
        pmm_free(phys, 1);
        return vmm_user_page_state(ctx, page_addr, writable) == 1 ? 0 : -1;
    }
    return 0;
}

/*
 * VmmFirstTouch — the page a cabin's first touch of `addr` brings in, brought
 * in: the one place that decides what that is. The #PF handler asks it about
 * a fault the cabin took, and the kernel's copies into and out of cabin memory
 * (VmmUserCopyWindow) about a page the cabin has not touched yet, so a buffer
 * reads and writes the same whether its owner has been through it or not.
 *
 * Of what a program owns, only its heap grows on first touch — the window from
 * ctx->heap_start that CabinInfo hands it as heap_max_size. Stacks, Bay, Brook
 * and the hammock are mapped whole when they are made. The ring slot windows
 * grow on first touch as well, but they are the kernel's rings, not a buffer:
 * their producers bring them in (vmm_ensure_user_page), their own arm of the
 * fault handler covers a cabin that looks first, and a copy aimed at a slot
 * page nobody has brought in yet is refused.
 *
 *    1  the page is there: brought in now, or already there — a sibling
 *       strand's fault or a copy on another core may get there first, and
 *       vmm_map_page refuses the second map under ctx->lock
 *    0  `addr` is not in the heap window
 *   -1  it is, and the page could not be had
 *
 * A page that was not present has no TLB entry on any core, so bringing it in
 * owes no shootdown: vmm_map_page skips one for a first-time map (SDM Vol 3A
 * §4.10.2.1), and a copy on another core goes through the Pull Map alias, not
 * through the cabin's translation.
 */
static int VmmFirstTouch(vmm_context_t *ctx, uintptr_t addr)
{
    if (addr < ctx->heap_start || addr >= ctx->heap_start + CONFIG_USER_HEAP_MAX_SIZE)
        return 0;

    uintptr_t page_addr = addr & ~(VMM_PAGE_SIZE - 1);
    if (vmm_is_mapped(ctx, page_addr))
        return 1;

    void *phys = pmm_alloc_zero(1);
    if (!phys)
    {
        debug_printf("[VMM] ERROR: Failed to allocate heap page for user process\n");
        return -1;
    }

    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE;
    vmm_map_result_t result = vmm_map_page(ctx, page_addr, (uintptr_t)phys, flags);
    if (!result.success)
    {
        /* Race tolerance: two first touches of the SAME heap page — sibling
         * strands on different cores, or the cabin and a kernel copy — both
         * pass the unlocked vmm_is_mapped probe, both pmm_alloc, both
         * vmm_map_page. The loser's map is rejected (the winner installed a
         * different phys); free our spare and report the page there if it now
         * genuinely is. Without this the loser returned -1, which kills a
         * faulting strand and leaks its page. Mirrors vmm_ensure_user_page. */
        pmm_free(phys, 1);
        if (vmm_is_mapped(ctx, page_addr))
            return 1;
        debug_printf("[VMM] ERROR: Failed to map user heap page at 0x%lx\n", page_addr);
        return -1;
    }

    /* A high-water mark raised from more than one core: a CAS, so a lower
     * page mapped late cannot lower it. */
    uintptr_t end  = page_addr + VMM_PAGE_SIZE;
    uintptr_t seen = __atomic_load_n(&ctx->heap_end, __ATOMIC_RELAXED);
    while (seen < end &&
           !__atomic_compare_exchange_n(&ctx->heap_end, &seen, end, true,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;

    debug_printf("[VMM] Demand paging: mapped user heap page at 0x%lx\n", page_addr);
    return 1;
}

uint64_t vmm_get_page_flags(vmm_context_t *ctx, uintptr_t virt_addr)
{
    if (!ctx)
        return 0;

    spin_lock(&ctx->lock);

    uint8_t level = 0;
    pte_t *leaf = vmm_get_leaf_pte(ctx, virt_addr, &level);
    if (!leaf || level == 0 || !(*leaf & VMM_FLAG_PRESENT))
    {
        spin_unlock(&ctx->lock);
        return 0;
    }

    /* VMM_FLAG_LARGE_PAGE comes back set on a 2 MiB / 1 GiB leaf, which is
     * how a caller tells the permission bits apart from a 4 KiB entry's:
     * bit 7 is PS on a huge leaf and PAT on a 4 KiB one. Everything else
     * VMM_PTE_FLAGS_MASK keeps — P, R/W, U/S, PWT, PCD, A, D, G, NX — means
     * the same at every level. */
    uint64_t flags = vmm_pte_to_flags(*leaf);
    spin_unlock(&ctx->lock);

    return flags;
}

uintptr_t vmm_find_free_region(vmm_context_t *ctx, size_t size, uintptr_t start, uintptr_t end)
{
    if (!ctx || size == 0 || start >= end)
        return 0;

    size_t pages_needed = vmm_size_to_pages(size);
    uintptr_t current = vmm_page_align_up(start);

    while (current + vmm_pages_to_size(pages_needed) <= end)
    {
        bool region_free = true;

        for (size_t i = 0; i < pages_needed; i++)
        {
            if (vmm_is_mapped(ctx, current + i * VMM_PAGE_SIZE))
            {
                region_free = false;
                uintptr_t next = current + (i + 1) * VMM_PAGE_SIZE;

                if (next <= current || next >= end)
                {
                    return 0;
                }

                current = vmm_page_align_up(next);
                break;
            }
        }

        if (region_free)
        {
            return current;
        }
    }

    return 0;
}

bool vmm_is_kernel_addr(uintptr_t addr)
{
    return addr >= VMM_KERNEL_BASE;
}

bool vmm_is_user_accessible(uintptr_t virt_addr)
{
    vmm_context_t *ctx = vmm_get_current_context();
    if (!ctx)
        return false;

    uint64_t flags = vmm_get_page_flags(ctx, virt_addr);
    return (flags & VMM_FLAG_USER) != 0;
}

bool vmm_protect(vmm_context_t *ctx, uintptr_t virt_addr, size_t size, uint64_t new_flags)
{
    if (!ctx || size == 0)
        return false;

    size_t page_count = vmm_size_to_pages(size);
    uintptr_t base_addr = vmm_page_align_down(virt_addr);
    uintptr_t current_addr = base_addr;

    spin_lock(&ctx->lock);

    for (size_t i = 0; i < page_count; i++)
    {
        pte_t *pte = vmm_get_pte(ctx, current_addr);
        if (!pte || !(*pte & VMM_FLAG_PRESENT))
        {
            spin_unlock(&ctx->lock);
            return false;
        }

        uintptr_t phys_addr = vmm_pte_to_phys(*pte);
        uint64_t flags_to_set = (new_flags & VMM_PTE_FLAGS_MASK);
        if (!(flags_to_set & VMM_FLAG_PRESENT))
            flags_to_set |= VMM_FLAG_PRESENT;
        *pte = vmm_make_pte(phys_addr, flags_to_set);

        current_addr += VMM_PAGE_SIZE;
    }

    spin_unlock(&ctx->lock);

    // Batched cross-core shootdown for all modified pages
    vmm_shootdown_pages(ctx, base_addr, page_count);

    return true;
}

bool vmm_reserve_region(vmm_context_t *ctx, uintptr_t start, size_t size, uint64_t flags)
{
    if (!ctx || size == 0)
        return false;

    size_t page_count = vmm_size_to_pages(size);
    uintptr_t virt_base = vmm_page_align_down(start);

    void *phys_pages = pmm_alloc(page_count);
    if (!phys_pages)
        return false;

    vmm_map_result_t result = vmm_map_pages(ctx, virt_base, (uintptr_t)phys_pages,
                                            page_count, flags);

    if (!result.success)
    {
        pmm_free(phys_pages, page_count);
        return false;
    }

    return true;
}

static void vmm_init_maxphyaddr(void)
{
    vmm_maxphyaddr = cpuid_get_maxphyaddr();

    if (vmm_maxphyaddr >= 52)
    {
        vmm_pte_addr_mask = 0x000FFFFFFFFFF000ULL;
    }
    else
    {
        uint64_t max_addr = (1ULL << vmm_maxphyaddr);
        vmm_pte_addr_mask = (max_addr - 1) & 0xFFFFFFFFFFFFF000ULL;
    }

    debug_printf("[VMM] MAXPHYADDR detection:\n");
    debug_printf("[VMM]   Physical address bits: %u\n", vmm_maxphyaddr);
    debug_printf("[VMM]   Max physical address: 0x%llx (%llu MB)\n",
                 (1ULL << vmm_maxphyaddr) - 1,
                 (1ULL << vmm_maxphyaddr) / (1024 * 1024));
    debug_printf("[VMM]   PTE address mask: 0x%016llx\n", vmm_pte_addr_mask);

    uint8_t virt_bits = cpuid_get_maxvirtaddr();
    debug_printf("[VMM]   Virtual address bits: %u\n", virt_bits);
    debug_printf("[VMM]   Max virtual address: 0x%llx\n",
                 (1ULL << virt_bits) - 1);
}

/* =========================================================================
 * Pull Map construction — the map is built FROM the memory map
 *
 * The Pull Map used to be laid out from a single number: the top of the
 * highest usable E820 entry, rounded up to a gigabyte, covered end to end
 * with write-back leaves. That is one flat run of identical entries across
 * memory that is not identical at all. On any board with RAM above 4 GiB it
 * put a write-back window over the PCI hole at 3–4 GiB and over the first
 * megabyte, which means:
 *
 *   • the kernel's own text console writes VGA memory at 0xB8000 through a
 *     cacheable entry (hw_vga reaches it via vmm_phys_to_virt),
 *   • every vmm_map_mmio consumer — HPET, xHCI, AHCI, the ACPI operation
 *     regions, PCI BARs — holds a strong-UC window onto a physical page
 *     this map simultaneously calls write-back, which Intel SDM Vol 3A
 *     §11.12.4 names as the one thing a program must not do,
 *   • and the round-up mapped past the end of what the machine decodes.
 *
 * It has not bitten because firmware's MTRRs make the hole uncacheable, and
 * effective type is MTRR combined with PAT — so the kernel was standing on
 * a configuration it never programmed and never checked. vmm_dump_mtrr_
 * layout now says so out loud when the bet fails; this builds the map so
 * the bet is not needed.
 *
 * The rule is one sentence: a span every byte of which is E820 USABLE is
 * write-back, and everything else is uncacheable. Uncacheable, not absent —
 * dropping the non-usable leaves would take 0xB8000 out of the address
 * space and kill the text console on every BIOS boot, and take the AP
 * trampoline page and the firmware tables with it. A leaf that cannot say
 * one thing about the whole span it covers is split until it can.
 *
 * UC is spelled PCD|PWT, which is PAT entry 3. That entry is strong UC in
 * the table vmm_pat_init programs AND in the x86 reset-default PAT, which
 * matters because the Pull Map is installed and first used before
 * vmm_pat_init runs. The write-back leaves carry no cache bits at all —
 * PAT entry 0, write-back in both tables.
 * ========================================================================= */

typedef enum {
    PULL_SPAN_RAM,      /* every byte of the span is E820 USABLE */
    PULL_SPAN_DEVICE,   /* no byte of it is */
    PULL_SPAN_MIXED     /* both, so this leaf is too coarse to tell the truth */
} PullSpanKind;

/* What the boot line reports: how the map came out, in leaves and tables. */
typedef struct {
    size_t gib_leaves;
    size_t mib_leaves;
    size_t kib_leaves;
    size_t device_leaves;
    size_t tables;
} PullMapTally;

#define PULL_LEAF_RAM     (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_GLOBAL)
#define PULL_LEAF_DEVICE  (PULL_LEAF_RAM | VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH)

/* Classify [start, end) against the memory map.
 *
 * Two questions, because overlap is not coverage. First, which kinds of
 * entry the span meets at all — a span that meets no usable entry is device
 * memory whatever else it meets, including nothing (an address range the
 * map never names is a hole, and a hole is not RAM). Second, whether the
 * usable entries cover the span with no gap: the cursor walks forward, each
 * step jumping to the far end of whichever usable run contains it, and a
 * step that cannot move is a cursor standing in a gap.
 *
 * A span that meets a non-usable entry is never RAM even if usable entries
 * appear to cover it. Firmware that publishes a reserved range brushing or
 * inside a usable one is real — pmm_init carves the same overlaps out of
 * the buddy — and where the two descriptions disagree the cautious reading
 * is the one that does not put a cacheable window on a device.
 *
 * Terminates: the cursor's next value is always the end of some usable
 * entry and always strictly greater than the current one, so there are at
 * most `count` steps before it passes `end` or fails to move. */
static PullSpanKind pull_span_kind(const e820_entry_t *entries, size_t count,
                                   uint64_t start, uint64_t end)
{
    bool meets_usable = false;
    bool meets_other  = false;

    for (size_t i = 0; i < count; i++) {
        uint64_t base = entries[i].base;
        uint64_t stop = base + entries[i].length;
        if (entries[i].length == 0 || stop <= base) continue;   /* empty or wrapped */
        if (start >= stop || base >= end) continue;             /* no overlap */
        if (entries[i].type == E820_USABLE) meets_usable = true;
        else                                meets_other  = true;
    }

    if (!meets_usable) return PULL_SPAN_DEVICE;
    if (meets_other)   return PULL_SPAN_MIXED;

    uint64_t cursor = start;
    for (;;) {
        uint64_t reach = cursor;
        for (size_t i = 0; i < count; i++) {
            if (entries[i].type != E820_USABLE || entries[i].length == 0) continue;
            uint64_t base = entries[i].base;
            uint64_t stop = base + entries[i].length;
            if (stop <= base) continue;
            if (base <= cursor && stop > reach) reach = stop;
        }
        if (reach >= end)    return PULL_SPAN_RAM;
        if (reach == cursor) return PULL_SPAN_MIXED;
        cursor = reach;
    }
}

/* The last physical address the memory map describes, rounded up to a page.
 * The Pull Map stops here and not a byte later.
 *
 * The top of the highest USABLE entry was the wrong bound in both
 * directions. Rounded up to a gigabyte it named physical addresses nothing
 * decodes; taken literally it would leave every device range ABOVE the top
 * of RAM with no window at all, and a driver that hands one of those to
 * vmm_phys_to_virt gets a fault instead of registers. The top of the WHOLE
 * map is the honest bound: everything the firmware named, and nothing else.
 *
 * pmm_get_total_memory() is folded in as a floor rather than trusted as the
 * answer — the buddy zone is derived from the same usable entries, so it
 * can only ever be lower, but a map this kernel reads differently from the
 * way the PMM read it must not end up smaller than the memory the PMM is
 * prepared to hand out. */
static uint64_t pull_map_top(const e820_entry_t *entries, size_t count)
{
    uint64_t cpu_limit = (vmm_maxphyaddr >= 64)
                       ? ~0ULL
                       : (1ULL << vmm_maxphyaddr);

    uint64_t top = 0;
    for (size_t i = 0; i < count; i++) {
        uint64_t base = entries[i].base;
        uint64_t stop = base + entries[i].length;
        if (entries[i].length == 0 || stop <= base) continue;
        if (stop > top) top = stop;
    }

    uint64_t ram_top = pmm_get_total_memory();
    if (ram_top > top) top = ram_top;

    /* Clamp before rounding, never after: both clamps land on page-aligned
     * values, so the round-up that follows cannot push past either of them
     * and cannot wrap. */
    if (top > cpu_limit) top = cpu_limit;

    if (top > PULL_MAP_SPAN) {
        /* One PML4 slot is the whole reach, and physical memory past it has
         * no Pull Map address. Said out loud because a machine this large
         * would otherwise lose the top of itself in silence. */
        kprintf("[VMM] the memory map reaches 0x%lx, past the Pull Map's one "
                "PML4 slot (512 GiB) — physical space above that has no "
                "kernel window\n", (unsigned long)top);
        top = PULL_MAP_SPAN;
    }

    return (top + VMM_PAGE_SIZE - 1) & ~((uint64_t)VMM_PAGE_SIZE - 1);
}

/* Build the Pull Map's PDPT and everything under it. Returns the PDPT's
 * physical address; the caller installs it at PULL_MAP_PML4_INDEX.
 *
 * Three fixed levels, largest leaf first: a 1 GiB leaf where the CPU has
 * PDPE1GB and the whole gigabyte says one thing, a 2 MiB leaf where the
 * megabyte-pair does, and 4 KiB leaves for the rest. A span that runs past
 * `top` is always split, which is what keeps the last leaf from reaching
 * addresses nothing decodes. At 4 KiB there is nowhere finer to go, so a
 * page that is part RAM and part something else is treated as device
 * memory — the reading that cannot corrupt anything.
 *
 * The tables come from the PMM, which is up: vmm_init runs after pmm_init,
 * and every page the buddy can hand out at this point is below 4 GiB
 * (pmm_init defers the high regions until pmm_activate_pull_map, which runs
 * later). That matters because g_pull_map_active is still false here, so
 * vmm_phys_to_virt returns the identity address and the boot identity map
 * only covers the first 4 GiB.
 *
 * Cost: one table per gigabyte and per megabyte-pair that straddles a type
 * boundary, so it is bounded by the number of boundaries in the map — at
 * most a few dozen on a real board, and at most a couple of hundred for a
 * map at the 128-entry ceiling. The 2 MiB fallback already allocated up to
 * 512 tables unconditionally. */
static uintptr_t pull_map_build(const e820_entry_t *entries, size_t count,
                                uint64_t top, bool use_1gib_pages,
                                PullMapTally *tally)
{
    uintptr_t pdpt_phys = vmm_alloc_page_table();
    if (!pdpt_phys) {
        panic("Failed to allocate Pull Map PDPT");
    }
    page_table_t *pdpt = (page_table_t *)vmm_phys_to_virt(pdpt_phys);
    tally->tables++;

    for (uint32_t gib = 0; gib < 512; gib++) {
        uint64_t gib_base = (uint64_t)gib << 30;
        if (gib_base >= top) break;
        uint64_t gib_end = gib_base + (1ULL << 30);

        if (use_1gib_pages && gib_end <= top) {
            PullSpanKind kind = pull_span_kind(entries, count, gib_base, gib_end);
            if (kind != PULL_SPAN_MIXED) {
                uint64_t flags = (kind == PULL_SPAN_RAM)
                               ? PULL_LEAF_RAM : PULL_LEAF_DEVICE;
                pdpt->entries[gib] = vmm_make_pte(gib_base,
                                                  flags | VMM_FLAG_LARGE_PAGE);
                tally->gib_leaves++;
                if (kind == PULL_SPAN_DEVICE) tally->device_leaves++;
                continue;
            }
        }

        uintptr_t pd_phys = vmm_alloc_page_table();
        if (!pd_phys) {
            panic("Failed to allocate Pull Map PD for phys 0x%lx",
                  (unsigned long)gib_base);
        }
        page_table_t *pd = (page_table_t *)vmm_phys_to_virt(pd_phys);
        tally->tables++;

        for (uint32_t mib = 0; mib < 512; mib++) {
            uint64_t mib_base = gib_base + ((uint64_t)mib << 21);
            if (mib_base >= top) break;
            uint64_t mib_end = mib_base + (1ULL << 21);

            if (mib_end <= top) {
                PullSpanKind kind = pull_span_kind(entries, count,
                                                   mib_base, mib_end);
                if (kind != PULL_SPAN_MIXED) {
                    uint64_t flags = (kind == PULL_SPAN_RAM)
                                   ? PULL_LEAF_RAM : PULL_LEAF_DEVICE;
                    pd->entries[mib] = vmm_make_pte(mib_base,
                                                    flags | VMM_FLAG_LARGE_PAGE);
                    tally->mib_leaves++;
                    if (kind == PULL_SPAN_DEVICE) tally->device_leaves++;
                    continue;
                }
            }

            uintptr_t pt_phys = vmm_alloc_page_table();
            if (!pt_phys) {
                panic("Failed to allocate Pull Map PT for phys 0x%lx",
                      (unsigned long)mib_base);
            }
            page_table_t *pt = (page_table_t *)vmm_phys_to_virt(pt_phys);
            tally->tables++;

            for (uint32_t kib = 0; kib < 512; kib++) {
                uint64_t page = mib_base + ((uint64_t)kib << 12);
                if (page >= top) break;
                PullSpanKind kind = pull_span_kind(entries, count,
                                                   page, page + VMM_PAGE_SIZE);
                uint64_t flags = (kind == PULL_SPAN_RAM)
                               ? PULL_LEAF_RAM : PULL_LEAF_DEVICE;
                pt->entries[kib] = vmm_make_pte(page, flags);
                tally->kib_leaves++;
                if (kind != PULL_SPAN_RAM) tally->device_leaves++;
            }

            pd->entries[mib] = vmm_make_pte(pt_phys,
                                            VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
        }

        pdpt->entries[gib] = vmm_make_pte(pd_phys,
                                          VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
    }

    return pdpt_phys;
}

void vmm_init(void)
{
    if (vmm_initialized)
    {
        debug_printf("[VMM] Already initialized!\n");
        return;
    }

    debug_printf("[VMM] Initializing Virtual Memory Manager...\n");

    // detect MAXPHYADDR before creating any page tables to build the correct PTE mask
    vmm_init_maxphyaddr();

    /* 5-level paging decision. Intel SDM Vol 3A §4.5 / AMD APM Vol 2 §5.3.5.
     *
     * Three paths:
     *   (1) Firmware ALREADY set CR4.LA57=1 (UEFI on 5-level-capable
     *       platforms sometimes does this when the OS option requests it):
     *       adopt natively, no runtime transition.
     *   (2) Bare-metal HW with CPUID LA57 + CR4.LA57=0: run the runtime
     *       dance to enable CR4.LA57 + switch to a PML5 root. Safe per
     *       Intel SDM on real silicon.
     *   (3) Hypervisor environment (KVM, TCG, Hyper-V, ...) with CPUID LA57:
     *       STAY 4-level. The runtime dance involves CR0.PG=0 + compat-mode
     *       trampoline + far-jmp through a temp PML5; some hypervisors
     *       (notably QEMU TCG) don't reliably emulate this sequence and
     *       triple-fault. Real Intel/AMD silicon honours the dance per SDM
     *       — production hardware will take path (2). */
    {
        uint64_t cr4_init;
        asm volatile("mov %%cr4, %0" : "=r"(cr4_init));
        bool firmware_la57 = (cr4_init & (1ULL << 12)) != 0;
        if (firmware_la57) {
            g_vmm_paging_levels = 5;
            __atomic_store_n(&g_vmm_la57_active, true, __ATOMIC_RELEASE);
            debug_printf("[VMM] LA57 already enabled by firmware — adopting "
                         "5-level paging without runtime transition\n");
        } else if (g_cpu_caps.has_la57 && !hv_present()) {
            g_vmm_paging_levels = 5;
            debug_printf("[VMM] CPU supports LA57 + bare-metal — will enable "
                         "5-level paging via runtime CR0.PG dance\n");
        } else if (g_cpu_caps.has_la57) {
            /* Hypervisor advertises LA57 but the runtime dance triple-faults
             * under QEMU TCG specifically on the compat→64-bit far-jmp after
             * CR4.LA57=1 + CR3=PML5 + CR0.PG=1. Investigation (2026-06-08):
             *
             *   - Dance reaches PG=1 in 5-level mode successfully (slot read
             *     via [ebp], DBG marker after `mov cr0, eax`).
             *   - Crash is on the immediately-following `jmp far` regardless
             *     of: m16:m32 vs m16:m64 operand form (REX.W is silently
             *     ignored in compat mode per Intel SDM Vol 2A §2.2.1.2 and
             *     AMD APM Vol 3 §1.7.1.6), low-PA vs high-VA target (so it
             *     isn't sign-extension), or selector cache state.
             *   - The dance is per Intel SDM Vol 3A §4.1.1 / §4.5 / §9.8.5.
             *     Linux's head_64.S enables LA57 in 32-bit *protected* mode
             *     (BEFORE entering IA-32e), not via a compat-submode dance
             *     inside IA-32e — so this code path is rarely exercised on
             *     real hardware or emulators.
             *   - vmm_la57.asm is correct per spec; we still gate on bare-
             *     metal to avoid TCG triple-fault. On real Sapphire Rapids /
             *     Zen 4 silicon the dance should work (TCG-specific quirk).
             *
             * Note: hv_present()==true also covers KVM. KVM may pass the
             * dance through to silicon and work, but without a real LA57
             * KVM host to test, we stay conservative.
             */
            g_vmm_paging_levels = 4;
            debug_printf("[VMM] CPU supports LA57 but running under %s — "
                         "staying 4-level (runtime dance disabled under "
                         "hypervisor; see vmm.c LA57-decision comment)\n",
                         hv_vendor_name());
        } else {
            g_vmm_paging_levels = 4;
            debug_printf("[VMM] LA57 not supported — using 4-level paging\n");
        }
    }

    spinlock_init(&kernel_heap_lock);
    spinlock_init(&vmalloc_lock);
    spinlock_init(&kernel_mmio_lock);
    spinlock_init(&g_shootdown_lock);

    /* From here on, a core spinning in spin_lock() with IRQs disabled drains
     * cross-core TLB shootdowns that target it (instead of stalling until it
     * acquires the lock — the M1 real-HW deadlock). Safe to arm this early: the
     * poll is a no-op until a shootdown is actually in flight, and the
     * descriptor lock above is now initialized. */
    spin_set_wait_service(vmm_tlb_shootdown_poll);

    kernel_context = vmm_create_context();
    if (!kernel_context)
    {
        panic("Failed to create kernel VMM context");
    }

    debug_printf("[VMM] Kernel context created at %p\n", kernel_context);
    debug_printf("[VMM] PML4 physical address: 0x%p\n", (void *)kernel_context->pml4_phys);

    // Higher-half kernel mapping: map kernel code+data+stack at PML4[511] PDPT[510]
    // using 2MB large pages. No identity mapping — kernel runs at 0xFFFFFFFF80100000+

#define LARGE_PAGE_SIZE (2 * 1024 * 1024)
#define HIGHER_HALF_BASE 0xFFFFFFFF80000000ULL
// Map first 64MB at higher-half (covers kernel + boot infrastructure)
#define HIGHER_HALF_MAP_SIZE (64ULL * 1024 * 1024)
    size_t large_pages_mapped = 0;

    debug_printf("[VMM] Setting up higher-half kernel mapping at 0x%p (%llu MB)...\n",
                 (void *)HIGHER_HALF_BASE, HIGHER_HALF_MAP_SIZE / (1024 * 1024));

    /* ‼ A KNOWN REMAINING ALIAS, left standing deliberately.
     *
     * This is a flat write-back run over the first 64 MiB, and the first
     * megabyte of that is not RAM: 0xA0000-0xFFFFF is the legacy VGA
     * aperture, the option ROMs and the BIOS area. The Pull Map built below
     * gives those same physical pages the uncacheable type the memory map
     * says they deserve, which leaves this run as a SECOND, write-back name
     * for them — the alias Intel SDM Vol 3A §11.12.4 names.
     *
     * Nothing in the kernel reaches VGA or the ROMs through a higher-half
     * address; hw_vga goes through vmm_phys_to_virt, which is the Pull Map.
     * So the alias is unused rather than absent, and the question is open
     * rather than closed. It stays because this run is what the kernel's own
     * code, data and boot stack execute from before anything else exists,
     * and carving holes in it is a change with its own ways of going wrong.
     * Named here so the next reader does not mistake silence for an answer. */
    for (uintptr_t phys = 0; phys < HIGHER_HALF_MAP_SIZE; phys += LARGE_PAGE_SIZE)
    {
        uintptr_t virt = HIGHER_HALF_BASE + phys;
        page_table_t *pd = vmm_get_or_create_table(kernel_context, virt, 2);
        if (!pd)
        {
            panic("Failed to create page directory for higher-half mapping");
        }

        uint32_t pd_idx = VMM_PD_INDEX(virt);
        pte_t *pd_entry = &pd->entries[pd_idx];

        *pd_entry = vmm_make_pte(phys, VMM_FLAGS_KERNEL_RW | VMM_FLAG_LARGE_PAGE);
        large_pages_mapped++;
    }

    debug_printf("[VMM] Higher-half mapped %zu large pages (%zu MB) at PML4[511]\n",
                 large_pages_mapped, large_pages_mapped * 2);

    // Pull Map: every physical address the memory map names, mapped once at
    // PULL_MAP_BASE with the type that map gives it — write-back for RAM,
    // uncacheable for everything else. 1 GiB leaves collapse a whole
    // gigabyte to a single PDPT entry (no per-2 MiB PD walk, fewer TLB
    // entries) wherever the CPU advertises PDPE1GB (CPUID.80000001H:EDX[26])
    // AND the gigabyte is of one kind throughout. See the block comment
    // above pull_span_kind for why the type matters and what it costs.
    //
    // g_pull_map_active is false here, so vmm_phys_to_virt returns identity
    // — the tables below are reached at their physical addresses, which the
    // boot identity map still covers.
    bool use_1gb_pages = g_cpu_caps.has_1gb_pages;

    e820_entry_t *pull_entries     = memory_map_get_entries();
    size_t        pull_entry_count = memory_map_get_entry_count();
    if (!pull_entries || pull_entry_count == 0)
    {
        panic("Pull Map: no memory map — nothing can tell RAM from device memory");
    }

    uint64_t     pull_top = pull_map_top(pull_entries, pull_entry_count);
    PullMapTally tally    = {0};

    uintptr_t pull_pdpt_phys = pull_map_build(pull_entries, pull_entry_count,
                                              pull_top, use_1gb_pages, &tally);

    debug_printf("[VMM] Pull Map: covers phys 0 - 0x%llx from %zu memory-map "
                 "entries; leaves 1G=%zu 2M=%zu 4K=%zu of which %zu uncached; "
                 "%zu page tables\n",
                 (unsigned long long)pull_top, pull_entry_count,
                 tally.gib_leaves, tally.mib_leaves, tally.kib_leaves,
                 tally.device_leaves, tally.tables);

    /* Pull Map root install. Under 5-level paging, ctx->pml4 is actually
     * the PML5 root — we must descend through PML5[511] (PA of the kernel
     * PML4 mirror, allocated implicitly by the prior vmm_get_or_create_table
     * pass for higher-half) and write to PML4[272]. Under 4-level we write
     * directly to PML4[272]. vmm_kernel_pml4_of returns the right page. */
    {
        page_table_t *kpml4 = vmm_kernel_pml4_of(kernel_context);
        if (!kpml4) {
            panic("vmm_init: kernel PML4 not yet allocated (no higher-half "
                  "mapping created before Pull Map install?)");
        }
        kpml4->entries[PULL_MAP_PML4_INDEX] =
            vmm_make_pte(pull_pdpt_phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
    }

    debug_printf("[VMM] Pull Map installed at PML4[%d] = 0x%lx (%d-level)\n",
                 PULL_MAP_PML4_INDEX, pull_pdpt_phys, g_vmm_paging_levels);

    debug_printf("[VMM] Kernel heap will be mapped on demand starting at 0x%p\n",
                 (void *)VMM_KERNEL_HEAP_BASE);

    debug_printf("[VMM] Kernel MMIO region: %p - %p (on-demand)\n",
                 (void *)VMM_KERNEL_MMIO_BASE,
                 (void *)(VMM_KERNEL_MMIO_BASE + VMM_KERNEL_MMIO_SIZE));

    /* The shadow bunk row is declared here beside the heap and the MMIO
     * window because that is what makes a kernel VA window exist in this
     * kernel: a BASE/SIZE pair in vmm.h and a line here saying which
     * stretch is spoken for. It maps nothing at boot — one bunk appears
     * when a process is given a supervisor shadow stack and goes when
     * the process does. */
    debug_printf("[VMM] Shadow bunk row: %p - %p (%lu bunks, one per pid, "
                 "on-demand)\n",
                 (void *)VMM_SHADOW_BUNK_BASE, (void *)VMM_SHADOW_BUNK_END,
                 (unsigned long)VMM_SHADOW_BUNK_COUNT);

    // Save values that will be inaccessible after identity mapping is removed.
    // kernel_context was kmalloc'd at identity address — will be a dangling pointer after switch.
    uintptr_t saved_pml4_phys = kernel_context->pml4_phys;
    uintptr_t saved_ctx_phys = (uintptr_t)kernel_context;

    /* LA57 runtime transition. Done BEFORE vmm_switch_context so the boot
     * identity mapping (PA=VA for 0..4 GB stage2/tagboot tables) is still
     * alive — the asm trampoline needs it to execute its low-PA aliased
     * post-PG=0 code and to jump back to the high-VA kernel after PG=1.
     *
     * Skipped when firmware already enabled CR4.LA57 (g_vmm_la57_active set
     * earlier in vmm_init) — no runtime flip needed in that case.
     *
     * The dance loads CR3 with a temporary PML5 that wraps the boot PML4
     * via PML5[0] AND PML5[511] (so both identity and higher-half stay
     * reachable). Immediately after the dance returns we mov-cr3 to the
     * final kernel_context PML5 so subsequent code runs on the proper
     * 5-level kernel tables.
     *
     * Phys-address constraints from the asm trampoline:
     *   • temp PML5 phys < 4 GB (32-bit `mov cr3, ebx` in dance asm).
     *   • final kernel PML5 phys < 4 GB (same constraint applies to the
     *     AP trampoline, which loads it in 32-bit pre-paging code). */
    if (g_vmm_paging_levels == 5 &&
        !__atomic_load_n(&g_vmm_la57_active, __ATOMIC_ACQUIRE))
    {
        uint64_t boot_cr3;
        asm volatile("mov %%cr3, %0" : "=r"(boot_cr3));
        uint64_t boot_pml4_phys = boot_cr3 & ~0xFFFULL;

        if (kernel_context->pml4_phys >= (1ULL << 32)) {
            panic("LA57: kernel PML5 phys 0x%lx >= 4 GB — AP trampoline 32-bit "
                  "CR3 load would truncate. PMM must serve <4 GB pages for the "
                  "kernel context's top-level table.",
                  (unsigned long)kernel_context->pml4_phys);
        }

        void *temp_pml5_p = pmm_alloc_zero(1);
        if (!temp_pml5_p) panic("LA57: cannot allocate temp PML5 page");
        uintptr_t temp_pml5_phys = (uintptr_t)temp_pml5_p;
        if (temp_pml5_phys >= (1ULL << 32)) {
            pmm_free(temp_pml5_p, 1);
            panic("LA57: temp PML5 phys 0x%lx >= 4 GB — dance asm 32-bit "
                  "CR3 load would truncate.",
                  (unsigned long)temp_pml5_phys);
        }
        page_table_t *temp_pml5 = (page_table_t *)vmm_phys_to_virt(temp_pml5_phys);
        /* Wrap the boot PML4 at both PML5[0] (so identity 0..4 GB stays
         * reachable through PML5[0]→boot_PML4[0]→PDPT_identity) and
         * PML5[511] (so higher-half kernel stays reachable through
         * PML5[511]→boot_PML4[511]→PDPT_high). */
        temp_pml5->entries[0]   = boot_pml4_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        temp_pml5->entries[511] = boot_pml4_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;

        debug_printf("[VMM] LA57 transition: boot_PML4=0x%lx temp_PML5=0x%lx "
                     "kernel_PML5=0x%lx\n",
                     (unsigned long)boot_pml4_phys,
                     (unsigned long)temp_pml5_phys,
                     (unsigned long)kernel_context->pml4_phys);

        /* Execute the dance: CR0.PG=0 → CR4.LA57=1 → CR3=temp_PML5 → CR0.PG=1
         * with a temporary 32-bit CS to bridge the compat-mode window. */
        vmm_la57_runtime_enable(temp_pml5_phys);

        /* Publish LA57 active state for AP trampoline + other consumers
         * (per-AP CR4.LA57 propagation). Release-store synchronises with
         * any subsequent acquire-load on APs. */
        __atomic_store_n(&g_vmm_la57_active, true, __ATOMIC_RELEASE);

        /* Switch CR3 from temp_PML5 to the final kernel PML5. PCID is not
         * yet enabled so a plain CR3 write is fine. */
        asm volatile("mov %0, %%cr3" : : "r"(kernel_context->pml4_phys) : "memory");

        /* Release the temp PML5 page — it served its single-use purpose. */
        pmm_free(temp_pml5_p, 1);

        debug_printf("[VMM] LA57 active: CR4.LA57=1, kernel on 5-level paging\n");
    }

    // Switch to new kernel page tables.
    // After this: identity mapping is GONE. Only higher-half + Pull Map exist.
    // RSP is at higher-half address (converted in kernel_entry.asm), mapped by PML4[511].
    current_context = kernel_context;
    vmm_switch_context(kernel_context);

    // Immediately activate Pull Map — identity addresses are dead, Pull Map is alive
    asm volatile("" ::: "memory");
    g_pull_map_active = true;

    // Rebase kernel_context from identity to Pull Map address (was kmalloc'd at phys addr)
    kernel_context = (vmm_context_t *)vmm_phys_to_virt(saved_ctx_phys);
    current_context = kernel_context;
    kernel_context->pml4 = (page_table_t *)vmm_phys_to_virt(saved_pml4_phys);

    // VGA MUST be rebased FIRST — all subsequent prints go through VGA
    VideoActivatePullMap();

    // Rebase kmalloc heap pool and free list from identity to Pull Map addresses
    mem_activate_pull_map();

    // Rebase PMM bitmap to Pull Map address
    pmm_activate_pull_map();

    // Rebase e820 entries to Pull Map address
    e820_activate_pull_map();

    // Verify Pull Map access
    volatile uint32_t *pull_test = (volatile uint32_t *)((uintptr_t)0x200000 + PULL_MAP_BASE);
    uint32_t pull_old = *pull_test;
    *pull_test = 0x5055BA5E;
    if (*pull_test != 0x5055BA5E)
    {
        panic("Pull Map verification failed");
    }
    *pull_test = pull_old;
    debug_printf("[VMM] Pull Map verification: PASSED\n");
    debug_printf("[VMM] Identity mapping removed — higher-half kernel active\n");

    /* Program IA32_PAT so that entry 6 = WC (Write Combining).
     * This must be done while paging is active (after vmm_switch_context)
     * so the new PAT takes effect for all subsequent page mappings.
     * vmm_map_framebuffer uses PAT index 6 (PCD=1, PAT_bit=1, PWT=0). */
    vmm_pat_init();

    // Enable PCID if CPU supports it — zero-flush context switches
    // CR4.PCIDE requires CR3[11:0] = 0 when enabling (kernel PML4 is page-aligned)
    if (g_cpu_caps.has_pcid)
    {
        uint64_t cr4;
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 17); // CR4.PCIDE
        asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

        spinlock_init(&pcid_lock);
        kernel_context->pcid = PCID_KERNEL;
        g_pcid_active = true;

        debug_printf("[VMM] %[S]PCID enabled — zero-flush context switches active%[D]\n");
    }
    else
    {
        debug_printf("[VMM] PCID not supported — using full TLB flush on context switch\n");
    }

    vmm_initialized = true;

    debug_printf("[VMM] Virtual memory layout:\n");
    debug_printf("[VMM]   Kernel code:      0x%p (higher-half)\n", (void *)HIGHER_HALF_BASE);
    debug_printf("[VMM]   Kernel heap:      0x%p - 0x%p (on-demand)\n",
                 (void *)VMM_KERNEL_HEAP_BASE,
                 (void *)(VMM_KERNEL_HEAP_BASE + VMM_KERNEL_HEAP_SIZE));
    debug_printf("[VMM]   Pull Map:        0x%p - 0x%p (%s leaves where the "
                 "span allows)\n",
                 (void *)PULL_MAP_BASE,
                 (void *)(PULL_MAP_BASE + pull_top),
                 use_1gb_pages ? "1GB" : "2MB");
    debug_printf("[VMM]   User base:        0x%p\n", (void *)VMM_USER_BASE);
    debug_printf("[VMM]   User heap:        0x%p\n", (void *)VMM_USER_HEAP_BASE);
    debug_printf("[VMM]   User stack top:   0x%p\n", (void *)VMM_USER_STACK_TOP);
    debug_printf("[VMM]   PCID:            %s\n", g_pcid_active ? "active (zero-flush)" : "off");
    debug_printf("[VMM]   Identity map:     NONE (removed)\n");

    debug_printf("[VMM] %[S]Virtual Memory Manager initialized successfully!%[D]\n");
}

void vmm_dump_page_tables(vmm_context_t *ctx, uintptr_t virt_addr)
{
    if (!ctx)
        return;

    debug_printf("[VMM] Page table dump for virtual address 0x%p (%d-level):\n",
                 (void *)virt_addr, g_vmm_paging_levels);
    if (g_vmm_paging_levels == 5) {
        debug_printf("[VMM]   PML5 index: %d\n", VMM_PML5_INDEX(virt_addr));
    }
    debug_printf("[VMM]   PML4 index: %d\n", VMM_PML4_INDEX(virt_addr));
    debug_printf("[VMM]   PDPT index: %d\n", VMM_PDPT_INDEX(virt_addr));
    debug_printf("[VMM]   PD index:   %d\n", VMM_PD_INDEX(virt_addr));
    debug_printf("[VMM]   PT index:   %d\n", VMM_PT_INDEX(virt_addr));

    spin_lock(&ctx->lock);

    if (!ctx->pml4)
    {
        debug_printf("[VMM]   top table (PML%d): NULL\n", g_vmm_paging_levels);
        spin_unlock(&ctx->lock);
        return;
    }

    /* 5-level: report PML5 entry + descend; under 4-level go straight to PML4. */
    if (g_vmm_paging_levels == 5) {
        pte_t pml5_entry = ctx->pml4->entries[VMM_PML5_INDEX(virt_addr)];
        debug_printf("[VMM]   PML5 entry: 0x%016llx (present: %s)\n",
                     (unsigned long long)pml5_entry,
                     (pml5_entry & VMM_FLAG_PRESENT) ? "yes" : "no");
        if (!(pml5_entry & VMM_FLAG_PRESENT)) {
            spin_unlock(&ctx->lock);
            return;
        }
    }
    page_table_t *pml4 = vmm_walk_pml5_to_pml4(ctx, virt_addr, false);
    if (!pml4) {
        spin_unlock(&ctx->lock);
        return;
    }
    pte_t pml4_entry = pml4->entries[VMM_PML4_INDEX(virt_addr)];
    debug_printf("[VMM]   PML4 entry: 0x%016llx (present: %s)\n",
                 (unsigned long long)pml4_entry, (pml4_entry & VMM_FLAG_PRESENT) ? "yes" : "no");

    if (!(pml4_entry & VMM_FLAG_PRESENT))
    {
        spin_unlock(&ctx->lock);
        return;
    }

    page_table_t *pdpt = (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pml4_entry));
    pte_t pdpt_entry = pdpt->entries[VMM_PDPT_INDEX(virt_addr)];
    debug_printf("[VMM]   PDPT entry: 0x%016llx (present: %s)\n",
                 (unsigned long long)pdpt_entry, (pdpt_entry & VMM_FLAG_PRESENT) ? "yes" : "no");

    if (!(pdpt_entry & VMM_FLAG_PRESENT))
    {
        spin_unlock(&ctx->lock);
        return;
    }

    page_table_t *pd = (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pdpt_entry));
    pte_t pd_entry = pd->entries[VMM_PD_INDEX(virt_addr)];
    debug_printf("[VMM]   PD entry:   0x%016llx (present: %s)\n",
                 (unsigned long long)pd_entry, (pd_entry & VMM_FLAG_PRESENT) ? "yes" : "no");

    if (!(pd_entry & VMM_FLAG_PRESENT))
    {
        spin_unlock(&ctx->lock);
        return;
    }

    if (pd_entry & VMM_FLAG_LARGE_PAGE)
    {
        debug_printf("[VMM]   PD entry is a large page (2MB). Physical: 0x%p\n", (void *)vmm_pte_to_phys(pd_entry));
        spin_unlock(&ctx->lock);
        return;
    }

    page_table_t *pt = (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pd_entry));
    pte_t pt_entry = pt->entries[VMM_PT_INDEX(virt_addr)];
    debug_printf("[VMM]   PT entry:   0x%016llx (present: %s)\n",
                 (unsigned long long)pt_entry, (pt_entry & VMM_FLAG_PRESENT) ? "yes" : "no");

    if (pt_entry & VMM_FLAG_PRESENT)
    {
        uintptr_t phys_addr = vmm_pte_to_phys(pt_entry);
        debug_printf("[VMM]   -> Physical: 0x%p\n", (void *)phys_addr);
        debug_printf("[VMM]   -> Flags: %s%s%s%s\n",
                     (pt_entry & VMM_FLAG_WRITABLE) ? "W" : "R",
                     (pt_entry & VMM_FLAG_USER) ? "U" : "K",
                     (pt_entry & VMM_FLAG_NO_EXECUTE) ? "NX" : "X",
                     (pt_entry & VMM_FLAG_GLOBAL) ? "G" : "");
    }

    spin_unlock(&ctx->lock);
}

void vmm_dump_context_stats(vmm_context_t *ctx)
{
    if (!ctx)
        return;

    spin_lock(&ctx->lock);

    debug_printf("[VMM] Context statistics:\n");
    debug_printf("[VMM]   PML4 physical:   0x%p\n", (void *)ctx->pml4_phys);
    debug_printf("[VMM]   Total pages:     %zu\n", ctx->mapped_pages);
    debug_printf("[VMM]   Kernel pages:    %zu\n", ctx->kernel_pages);
    debug_printf("[VMM]   User pages:      %zu\n", ctx->user_pages);
    debug_printf("[VMM]   Heap start:      0x%p\n", (void *)ctx->heap_start);
    debug_printf("[VMM]   Heap end:        0x%p\n", (void *)ctx->heap_end);
    debug_printf("[VMM]   Stack top:       0x%p\n", (void *)ctx->stack_top);

    spin_unlock(&ctx->lock);
}

void vmm_get_global_stats(vmm_stats_t *stats)
{
    if (!stats)
        return;
    stats->total_contexts = atomic_load_u64((volatile uint64_t *)&global_stats.total_contexts);
    stats->total_mapped_pages = atomic_load_u64((volatile uint64_t *)&global_stats.total_mapped_pages);
    stats->kernel_mapped_pages = atomic_load_u64((volatile uint64_t *)&global_stats.kernel_mapped_pages);
    stats->user_mapped_pages = atomic_load_u64((volatile uint64_t *)&global_stats.user_mapped_pages);
    stats->page_tables_allocated = atomic_load_u64((volatile uint64_t *)&global_stats.page_tables_allocated);
    stats->page_faults_handled = atomic_load_u64((volatile uint64_t *)&global_stats.page_faults_handled);
    stats->tlb_flushes = atomic_load_u64((volatile uint64_t *)&global_stats.tlb_flushes);
}

void vmm_print_stats(void)
{
    vmm_stats_t stats;
    vmm_get_global_stats(&stats);

    debug_printf("[VMM] Global statistics:\n");
    debug_printf("[VMM]   Total contexts:        %zu\n", stats.total_contexts);
    debug_printf("[VMM]   Total mapped pages:    %zu (%zu MB)\n",
                 stats.total_mapped_pages,
                 (stats.total_mapped_pages * VMM_PAGE_SIZE) / (1024 * 1024));
    debug_printf("[VMM]   Kernel mapped pages:   %zu (%zu MB)\n",
                 stats.kernel_mapped_pages,
                 (stats.kernel_mapped_pages * VMM_PAGE_SIZE) / (1024 * 1024));
    debug_printf("[VMM]   User mapped pages:     %zu (%zu MB)\n",
                 stats.user_mapped_pages,
                 (stats.user_mapped_pages * VMM_PAGE_SIZE) / (1024 * 1024));
    debug_printf("[VMM]   Page tables allocated: %zu (%zu KB)\n",
                 stats.page_tables_allocated,
                 (stats.page_tables_allocated * VMM_PAGE_SIZE) / 1024);
    debug_printf("[VMM]   Page faults handled:   %zu\n", stats.page_faults_handled);
    debug_printf("[VMM]   TLB flushes:           %zu\n", stats.tlb_flushes);
}

void vmm_test_basic(void)
{
    debug_printf("[VMM] %[H]Running basic VMM tests...%[D]\n");

    debug_printf("[VMM] Test 1: Context creation/destruction...\n");
    vmm_context_t *test_ctx = vmm_create_context();
    if (!test_ctx)
    {
        debug_printf("[VMM] %[E]FAILED: Could not create context%[D]\n");
        return;
    }
    debug_printf("[VMM] %[S]PASSED: Context created successfully%[D]\n");

    debug_printf("[VMM] Test 2: Page mapping...\n");
    void *phys_page = pmm_alloc(1);
    if (!phys_page)
    {
        debug_printf("[VMM] %[E]FAILED: Could not allocate physical page%[D]\n");
        vmm_destroy_context(test_ctx);
        return;
    }

    uintptr_t test_virt = 0x1000000;
    vmm_map_result_t result = vmm_map_page(test_ctx, test_virt, (uintptr_t)phys_page,
                                           VMM_FLAGS_KERNEL_RW);
    if (!result.success)
    {
        debug_printf("[VMM] %[E]FAILED: Could not map page: %s%[D]\n", result.error_msg);
        pmm_free(phys_page, 1);
        vmm_destroy_context(test_ctx);
        return;
    }
    debug_printf("[VMM] %[S]PASSED: Page mapped successfully%[D]\n");

    debug_printf("[VMM] Test 3: Address translation...\n");
    uintptr_t translated = vmm_virt_to_phys(test_ctx, test_virt);
    if (translated != (uintptr_t)phys_page)
    {
        debug_printf("[VMM] %[E]FAILED: Translation mismatch (got 0x%p, expected 0x%p)%[D]\n",
                     (void *)translated, phys_page);
        vmm_unmap_page(test_ctx, test_virt);
        pmm_free(phys_page, 1);
        vmm_destroy_context(test_ctx);
        return;
    }
    debug_printf("[VMM] %[S]PASSED: Address translation correct%[D]\n");

    debug_printf("[VMM] Test 4: Page unmapping...\n");
    if (!vmm_unmap_page(test_ctx, test_virt))
    {
        debug_printf("[VMM] %[E]FAILED: Could not unmap page%[D]\n");
        pmm_free(phys_page, 1);
        vmm_destroy_context(test_ctx);
        return;
    }

    if (vmm_is_mapped(test_ctx, test_virt))
    {
        debug_printf("[VMM] %[E]FAILED: Page still mapped after unmap%[D]\n");
        pmm_free(phys_page, 1);
        vmm_destroy_context(test_ctx);
        return;
    }
    debug_printf("[VMM] %[S]PASSED: Page unmapped successfully%[D]\n");

    pmm_free(phys_page, 1);
    vmm_destroy_context(test_ctx);

    debug_printf("[VMM] Test 5: Kernel heap allocation...\n");

    void *heap_ptr = vmalloc(8192);

    if (!heap_ptr)
    {
        debug_printf("[VMM] %[E]FAILED: vmalloc failed%[D]\n");
        return;
    }

    memset(heap_ptr, 0xAA, 8192);
    if (((uint8_t *)heap_ptr)[0] != 0xAA || ((uint8_t *)heap_ptr)[8191] != 0xAA)
    {
        debug_printf("[VMM] %[E]FAILED: Could not write to allocated memory%[D]\n");
        vfree(heap_ptr);
        return;
    }

    debug_printf("[VMM] %[S]PASSED: Kernel heap allocation works%[D]\n");

    vfree(heap_ptr);

    debug_printf("[VMM] %[S]All basic tests PASSED!%[D]\n");

    vmm_print_stats();
    vmm_dump_context_stats(kernel_context);
}

/* NUMA-aware single-page allocator for cabin metadata pages (CabinInfo,
 * PocketRing header, ResultRing header, TouchRing header).
 *
 * Real-HW rationale: every push/pop on the per-cabin IPC rings reads
 * the header. On a 2-socket Xeon / EPYC, a header page that landed on
 * the remote socket pays 3–5× cross-socket coherence cost per access.
 * Brook (NUMA-audit 2026-06-03) co-locates its stream pages with the
 * caller; Pocket/Result/Touch rings should match for consistency.
 *
 * Heuristic: prefer the NUMA domain of the CPU running this call —
 * usually the process spawner — because the spawned cabin tends to be
 * scheduled on the same socket via round-robin App-Core affinity. UMA
 * machines see no difference (acpi_get_numa returns !present →
 * UNKNOWN → fall through to plain pmm_alloc_zero). SRAT-absent BIOS
 * paths also fall through. Intel® 64 Optimization Reference Manual
 * §11.x (multi-socket memory hierarchy) — non-uniform access latency. */
static void *vmm_cabin_alloc_zero_numa(size_t pages)
{
    uint32_t domain = ACPI_NUMA_DOMAIN_UNKNOWN;
    const acpi_numa_info_t *n = acpi_get_numa();
    if (n && n->present) {
        uint8_t core = amp_get_core_index();
        if (core < MAX_CORES) {
            uint32_t lapic_id = g_amp.cores[core].lapic_id;
            for (uint16_t i = 0; i < n->cpu_count; i++) {
                if (!n->cpus[i].enabled) continue;
                if (n->cpus[i].apic_id == lapic_id) {
                    domain = n->cpus[i].domain;
                    break;
                }
            }
        }
    }

    if (domain == ACPI_NUMA_DOMAIN_UNKNOWN) {
        return pmm_alloc_zero(pages);
    }

    void *p = pmm_alloc_in_domain(pages, domain);
    if (!p) {
        /* Domain-local pool exhausted; fall back to any-zone zero alloc
         * so the cabin still comes up (correctness > NUMA optimality). */
        return pmm_alloc_zero(pages);
    }
    /* pmm_alloc_in_domain returns raw uncleared pages — mirror brook's
     * symmetric zero-fill so callers can rely on a freshly-cleared
     * header regardless of which path served the allocation. */
    memset((void *)vmm_phys_to_virt((uintptr_t)p), 0, pages * PMM_PAGE_SIZE);
    return p;
}

vmm_context_t *vmm_create_cabin(uint64_t *cabin_info_phys,
                                uint64_t *pocket_ring_phys,
                                uint64_t *result_ring_phys,
                                uint64_t *touch_ring_phys)
{
    if (!cabin_info_phys || !pocket_ring_phys || !result_ring_phys || !touch_ring_phys)
    {
        vmm_set_error("Invalid output parameters for Cabin creation");
        return NULL;
    }

    vmm_context_t *cabin_ctx = vmm_create_context();
    if (!cabin_ctx)
    {
        vmm_set_error("Failed to create Cabin context");
        return NULL;
    }

    // Allocate physical pages: CabinInfo (1), PocketRing (1), ResultRing (1), TouchRing (1).
    // Slot regions are lazily mapped on demand and are NOT pre-allocated here.
    // NUMA-aware allocation (see vmm_cabin_alloc_zero_numa rationale) co-
    // locates each header with the spawner's socket; cleared on return.
    void *info_phys = vmm_cabin_alloc_zero_numa(CABIN_INFO_PAGES);
    if (!info_phys)
    {
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to allocate CabinInfo page");
        return NULL;
    }

    void *pocket_phys = vmm_cabin_alloc_zero_numa(CABIN_POCKET_RING_PAGES);
    if (!pocket_phys)
    {
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to allocate PocketRing page");
        return NULL;
    }

    void *result_phys = vmm_cabin_alloc_zero_numa(CABIN_RESULT_RING_PAGES);
    if (!result_phys)
    {
        pmm_free(pocket_phys, CABIN_POCKET_RING_PAGES);
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to allocate ResultRing pages");
        return NULL;
    }

    void *touch_phys = vmm_cabin_alloc_zero_numa(CABIN_TOUCH_RING_PAGES);
    if (!touch_phys)
    {
        pmm_free(result_phys, CABIN_RESULT_RING_PAGES);
        pmm_free(pocket_phys, CABIN_POCKET_RING_PAGES);
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to allocate TouchRing page");
        return NULL;
    }
    /* Pages already zero-cleared by vmm_cabin_alloc_zero_numa — no
     * redundant memset needed here. */

    if (vmm_setup_null_trap(cabin_ctx) != 0)
    {
        pmm_free(touch_phys, CABIN_TOUCH_RING_PAGES);
        pmm_free(result_phys, CABIN_RESULT_RING_PAGES);
        pmm_free(pocket_phys, CABIN_POCKET_RING_PAGES);
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to setup NULL trap");
        return NULL;
    }

    if (vmm_map_cabin_info(cabin_ctx, (uintptr_t)info_phys) != 0)
    {
        pmm_free(touch_phys, CABIN_TOUCH_RING_PAGES);
        pmm_free(result_phys, CABIN_RESULT_RING_PAGES);
        pmm_free(pocket_phys, CABIN_POCKET_RING_PAGES);
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to map CabinInfo");
        return NULL;
    }

    if (vmm_map_pocket_ring(cabin_ctx, (uintptr_t)pocket_phys) != 0)
    {
        pmm_free(touch_phys, CABIN_TOUCH_RING_PAGES);
        pmm_free(result_phys, CABIN_RESULT_RING_PAGES);
        pmm_free(pocket_phys, CABIN_POCKET_RING_PAGES);
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to map PocketRing");
        return NULL;
    }

    if (vmm_map_result_ring(cabin_ctx, (uintptr_t)result_phys) != 0)
    {
        pmm_free(touch_phys, CABIN_TOUCH_RING_PAGES);
        pmm_free(result_phys, CABIN_RESULT_RING_PAGES);
        pmm_free(pocket_phys, CABIN_POCKET_RING_PAGES);
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to map ResultRing");
        return NULL;
    }

    if (vmm_map_touch_ring(cabin_ctx, (uintptr_t)touch_phys) != 0)
    {
        pmm_free(touch_phys, CABIN_TOUCH_RING_PAGES);
        pmm_free(result_phys, CABIN_RESULT_RING_PAGES);
        pmm_free(pocket_phys, CABIN_POCKET_RING_PAGES);
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to map TouchRing");
        return NULL;
    }

    if (g_cpu_caps_page_phys != 0)
    {
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        vmm_map_result_t map_result = vmm_map_page(cabin_ctx, CABIN_CPU_CAPS_ADDR, g_cpu_caps_page_phys, flags);
        if (!map_result.success)
        {
            debug_printf("[VMM] WARNING: Failed to map CPU caps page at 0x%lx: %s\n",
                         CABIN_CPU_CAPS_ADDR, map_result.error_msg);
        }
    }

    /* ClockBoard — read-only kernel-published clock page. ONE physical
     * page allocated at boot (clockboard_init) and mapped at the same VA
     * into every Cabin. No WRITABLE flag — userspace can read uptime in a
     * single load, kernel writes via the shared kernel direct-map. */
    {
        uint64_t cb_phys = clockboard_phys();
        if (cb_phys != 0)
        {
            uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;  /* R/O for user */
            vmm_map_result_t map_result = vmm_map_page(cabin_ctx,
                                                        CABIN_CLOCKBOARD_ADDR,
                                                        cb_phys, flags);
            if (!map_result.success)
            {
                debug_printf("[VMM] WARNING: Failed to map ClockBoard at 0x%lx: %s\n",
                             (unsigned long)CABIN_CLOCKBOARD_ADDR, map_result.error_msg);
            }
        }
    }

    *cabin_info_phys  = (uint64_t)info_phys;
    *pocket_ring_phys = (uint64_t)pocket_phys;
    *result_ring_phys = (uint64_t)result_phys;
    *touch_ring_phys  = (uint64_t)touch_phys;

    return cabin_ctx;
}

int vmm_map_cabin_info(vmm_context_t *ctx, uintptr_t phys_page)
{
    if (!ctx)
        return -1;

    // CabinInfo is read-only for userspace
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;

    vmm_map_result_t result = vmm_map_pages(ctx, VMM_CABIN_INFO, phys_page,
                                            CABIN_INFO_PAGES, flags);
    if (!result.success)
    {
        debug_printf("[VMM] Failed to map CabinInfo: %s\n", result.error_msg);
        return -1;
    }

    return 0;
}

int vmm_map_pocket_ring(vmm_context_t *ctx, uintptr_t phys_page)
{
    if (!ctx)
        return -1;

    // PocketRing is RW for userspace (producer writes Pockets, advances tail)
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;

    vmm_map_result_t result = vmm_map_pages(ctx, VMM_CABIN_POCKET_RING, phys_page,
                                            CABIN_POCKET_RING_PAGES, flags);
    if (!result.success)
    {
        debug_printf("[VMM] Failed to map PocketRing: %s\n", result.error_msg);
        return -1;
    }

    return 0;
}

int vmm_map_result_ring(vmm_context_t *ctx, uintptr_t phys_page)
{
    if (!ctx)
        return -1;

    // ResultRing is RW for userspace (consumer reads Results, advances head)
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;

    vmm_map_result_t result = vmm_map_pages(ctx, VMM_CABIN_RESULT_RING, phys_page,
                                            CABIN_RESULT_RING_PAGES, flags);
    if (!result.success)
    {
        debug_printf("[VMM] Failed to map ResultRing: %s\n", result.error_msg);
        return -1;
    }

    return 0;
}

int vmm_map_touch_ring(vmm_context_t *ctx, uintptr_t phys_page)
{
    if (!ctx)
        return -1;

    // TouchRing header is RW for userspace (consumer advances head, releases
    // per-slot seq). Slot region (CABIN_TOUCH_SLOTS_BASE..) is mapped lazily
    // on demand by KTouchPush via vmm_ensure_user_page.
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;

    vmm_map_result_t result = vmm_map_pages(ctx, VMM_CABIN_TOUCH_RING, phys_page,
                                            CABIN_TOUCH_RING_PAGES, flags);
    if (!result.success)
    {
        debug_printf("[VMM] Failed to map TouchRing: %s\n", result.error_msg);
        return -1;
    }

    return 0;
}

/*
 * vmm_user_window — the one walk behind every kernel access to cabin
 * memory. Returns the Pull Map pointer for `user_vaddr` and, in
 * *out_span, how many bytes of the user's range that pointer is good
 * for: the distance from the address to the end of the leaf that maps
 * it. NULL means the address is not user memory the caller may touch.
 *
 * Two hardware facts decide what this function must do.
 *
 * 1. PAGE SIZE. A PDE with PS=1 (bit 7) is a LEAF mapping a 2 MiB page,
 *    and a PDPTE with PS=1 is a leaf mapping a 1 GiB page — Intel SDM
 *    Vol 3A §5.5, Tables 5-18 and 5-16. Such an entry is a present
 *    translation for every linear address in its range, not an absent
 *    mapping. The kernel installs exactly those leaves for a cabin
 *    itself (SysHeapPrefault for malloc >= 2 MiB, Bay claims, Brook
 *    streams), so a walker that stops at the PS bit — vmm_get_pte — says
 *    "not mapped" about memory this kernel mapped user-RW one syscall
 *    earlier. vmm_get_leaf_pte finds the leaf at whatever level it lives
 *    and reports that level. The frame is then the leaf's OWN aligned
 *    field: bit 12 of a 2 MiB or 1 GiB leaf is PAT, not an address bit
 *    (the same split Linux draws with _PAGE_BIT_PAT_LARGE=12 and
 *    PHYSICAL_PMD_PAGE_MASK), so the phys base is masked to the leaf's
 *    own size, never to 4 KiB.
 *
 * 2. WRITE PERMISSION. The pointer handed back is the Pull Map alias —
 *    PULL_LEAF_RAM: PRESENT|WRITABLE|GLOBAL and no U/S. Access rights
 *    are a property of the translation the store actually uses (SDM
 *    Vol 3A §5.6): with CR0.WP=1 a supervisor write is permitted only
 *    "with a translation for which the R/W flag (bit 1) is 1 in every
 *    paging-structure entry controlling the translation" (§5.6.1), and
 *    SMAP is scoped to user-mode addresses, which this supervisor alias
 *    is not. So a memcpy through this pointer consults neither the
 *    cabin's R/W bit, nor CR0.WP, nor CR4.SMAP: this check is the only
 *    thing standing between a user-supplied address and a kernel store.
 *    `for_write` therefore demands VMM_FLAG_WRITABLE on the cabin's own
 *    leaf. Without it a Crate aimed at the machine-wide ClockBoard, the
 *    cpu_caps page, the read-only CabinInfo or the cabin's own
 *    non-writable ELF text has the kernel overwrite it on the app's
 *    behalf — a W^X and shared-page bypass needing no privilege.
 */
static void *vmm_user_window(vmm_context_t *ctx, uintptr_t user_vaddr,
                             bool for_write, size_t *out_span)
{
    if (out_span) *out_span = 0;
    if (!ctx) return NULL;

    uint8_t level = 0;
    pte_t *leaf = vmm_get_leaf_pte(ctx, user_vaddr, &level);
    if (!leaf || level == 0) return NULL;

    /* One read of the entry: a concurrent unmap on another core must not
     * split the permission checks from the frame this call returns. */
    pte_t entry = __atomic_load_n(leaf, __ATOMIC_ACQUIRE);
    if (!(entry & VMM_FLAG_PRESENT) || !(entry & VMM_FLAG_USER))
        return NULL;
    if (for_write && !(entry & VMM_FLAG_WRITABLE))
        return NULL;

    uintptr_t page_size = (level == 3) ? (1ULL << 30)   /* 1 GiB PDPT leaf */
                        : (level == 2) ? (1ULL << 21)   /* 2 MiB PD leaf   */
                                       : (1ULL << 12);  /* 4 KiB PT leaf   */
    uintptr_t page_off_mask = page_size - 1;

    uintptr_t frame  = vmm_pte_to_phys(entry) & ~page_off_mask;
    uintptr_t offset = user_vaddr & page_off_mask;

    if (out_span) *out_span = (size_t)(page_size - offset);
    return vmm_phys_to_virt(frame + offset);
}

void *vmm_translate_user_addr(vmm_context_t *ctx, uintptr_t user_vaddr, size_t size)
{
    if (size == 0)
        return NULL;

    size_t span = 0;
    void *kaddr = vmm_user_window(ctx, user_vaddr, false, &span);
    if (!kaddr)
        return NULL;

    if (size > span)
    {
        /* Fail-closed backstop: refuse a range that leaves the leaf instead of
         * the old silent truncation that corrupted a foreign frame. The limit is
         * the leaf's own size, so a 2 MiB-backed buffer is served whole rather
         * than chopped at an imaginary 4 KiB edge. Page-walking callers
         * (crate_read/write, vmm_user_buf_*) chunk per leaf, and the ring slot
         * accessors are straddle-safe by geometry (slot size divides the page,
         * slot base is page-aligned), so no production caller reaches here.
         * Coverage is proven by the per-subsystem audits plus CrateIoSelfTest
         * (which intentionally trips this path to verify the backstop returns
         * NULL). Canary is debug-gated: quiet on release, and NOT a production
         * tripwire precisely because the selftest deliberately exercises it. */
        debug_printf("[VMM] straddle-reject: vaddr=0x%lx size=%zu leaves its mapping after %zu bytes; use crate_read/crate_write or vmm_user_buf_* (page-walked)\n",
                     (unsigned long)user_vaddr, size, span);
        return NULL;
    }

    return kaddr;
}

int vmm_setup_null_trap(vmm_context_t *ctx)
{
    if (!ctx)
        return -1;
    // 0x0000-0x0FFF is intentionally left unmapped; any access raises a page fault
    return 0;
}

/* ====================================================================
 * 2 MB huge-page helpers — single PDE leaf with VMM_FLAG_LARGE_PAGE.
 *
 * Used by Bay (cross-cabin shared memory) and the user-heap pre-fault
 * path. Pure leaf-PDE write, no intermediate PT allocated. PMM buddy
 * order-9 (pmm_alloc(512)) naturally returns 2 MB-aligned phys blocks
 * so callers don't need to align separately.
 * ==================================================================== */

/* TME-MK aware huge-page (2 MiB) map. Same algorithm as
 * vmm_map_huge_2m but builds the PDE via vmm_make_pte_with_keyid so
 * KeyID bits in the upper phys field survive masking. Used by
 * encrypted Bay (BAY_CREATE | BAY_ENCRYPTED) when the requested size
 * crosses the 2 MiB threshold.
 *
 * Per Intel SDM Vol 3D §16.3: 2 MiB PDE format under TME-MK is the
 * same as 4 KiB PTE — phys field at bits [51:21] for 2 MiB pages,
 * with KeyID embedded in the upper num_keyid_bits of that field.
 *
 * Behaves identically to vmm_map_huge_2m when MK is inactive
 * (vmm_pte_addr_mask_with_keyid == vmm_pte_addr_mask). */
bool vmm_map_huge_2m_with_keyid(vmm_context_t *ctx, uintptr_t virt_addr,
                                 uintptr_t phys_addr_with_keyid, uint64_t flags)
{
    if (!ctx) return false;
    if (virt_addr & VMM_LARGE_PAGE_2M_MASK) return false;
    /* Strip KeyID for alignment check — the encryption-key bits sit
     * above the phys-address bits and don't affect 2 MiB alignment. */
    uintptr_t phys_strip = phys_addr_with_keyid & vmm_pte_addr_mask;
    if (phys_strip & VMM_LARGE_PAGE_2M_MASK) return false;

    spin_lock(&ctx->lock);

    page_table_t *pd = vmm_get_or_create_table(ctx, virt_addr, 2);
    if (!pd) {
        spin_unlock(&ctx->lock);
        return false;
    }

    uint32_t pd_idx  = VMM_PD_INDEX(virt_addr);
    pte_t   *pde     = &pd->entries[pd_idx];
    pte_t    new_pte = vmm_make_pte_with_keyid(phys_addr_with_keyid,
                                                flags | VMM_FLAG_LARGE_PAGE);

    pte_t expected = 0;
    bool  won      = __atomic_compare_exchange_n(pde, &expected, new_pte,
                                                 false,
                                                 __ATOMIC_RELEASE,
                                                 __ATOMIC_ACQUIRE);
    if (!won) {
        spin_unlock(&ctx->lock);
        return false;
    }

    ctx->mapped_pages += VMM_LARGE_PAGE_2M_PAGES;
    if (flags & VMM_FLAG_USER) {
        ctx->user_pages += VMM_LARGE_PAGE_2M_PAGES;
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.user_mapped_pages, VMM_LARGE_PAGE_2M_PAGES);
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.total_mapped_pages, VMM_LARGE_PAGE_2M_PAGES);
    } else {
        ctx->kernel_pages += VMM_LARGE_PAGE_2M_PAGES;
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.kernel_mapped_pages, VMM_LARGE_PAGE_2M_PAGES);
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.total_mapped_pages, VMM_LARGE_PAGE_2M_PAGES);
    }

    spin_unlock(&ctx->lock);
    return true;
}

bool vmm_map_huge_2m(vmm_context_t *ctx, uintptr_t virt_addr,
                     uintptr_t phys_addr, uint64_t flags)
{
    if (!ctx) return false;
    if (virt_addr & VMM_LARGE_PAGE_2M_MASK) return false;
    if (phys_addr & VMM_LARGE_PAGE_2M_MASK) return false;

    spin_lock(&ctx->lock);

    /* Walk to PD level (level==2). vmm_get_or_create_table allocates
     * any missing PML4/PDPT entries and demotes a parent LARGE_PAGE if
     * the walker needs to descend further (won't trigger for a fresh
     * user-VA range). */
    page_table_t *pd = vmm_get_or_create_table(ctx, virt_addr, 2);
    if (!pd) {
        spin_unlock(&ctx->lock);
        return false;
    }

    uint32_t pd_idx  = VMM_PD_INDEX(virt_addr);
    pte_t   *pde     = &pd->entries[pd_idx];
    pte_t    new_pte = vmm_make_pte(phys_addr, flags | VMM_FLAG_LARGE_PAGE);

    pte_t expected = 0;
    bool  won      = __atomic_compare_exchange_n(pde, &expected, new_pte,
                                                 false,
                                                 __ATOMIC_RELEASE,
                                                 __ATOMIC_ACQUIRE);
    if (!won) {
        spin_unlock(&ctx->lock);
        return false;
    }

    /* Accounting: count 512 × 4 KB units so existing stats remain
     * comparable across 4 KB and 2 MB mappings. */
    ctx->mapped_pages += VMM_LARGE_PAGE_2M_PAGES;
    if (flags & VMM_FLAG_USER) {
        ctx->user_pages += VMM_LARGE_PAGE_2M_PAGES;
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.user_mapped_pages, VMM_LARGE_PAGE_2M_PAGES);
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.total_mapped_pages, VMM_LARGE_PAGE_2M_PAGES);
    } else {
        ctx->kernel_pages += VMM_LARGE_PAGE_2M_PAGES;
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.kernel_mapped_pages, VMM_LARGE_PAGE_2M_PAGES);
        atomic_fetch_add_u64((volatile uint64_t *)&global_stats.total_mapped_pages, VMM_LARGE_PAGE_2M_PAGES);
    }

    spin_unlock(&ctx->lock);

    /* First-time map (expected==0) — no stale TLB entry on any core,
     * so the cross-core shootdown is a no-op. */
    return true;
}

bool vmm_unmap_huge_2m(vmm_context_t *ctx, uintptr_t virt_addr)
{
    if (!ctx || (virt_addr & VMM_LARGE_PAGE_2M_MASK)) return false;

    spin_lock(&ctx->lock);

    uint32_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    uint32_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    uint32_t pd_idx   = VMM_PD_INDEX(virt_addr);

    /* 5-level: descend PML5 → PML4 first. */
    page_table_t *pml4_table = vmm_walk_pml5_to_pml4(ctx, virt_addr, false);
    if (!pml4_table) {
        spin_unlock(&ctx->lock);
        return false;
    }
    pte_t pml4_e = pml4_table->entries[pml4_idx];
    if (!(pml4_e & VMM_FLAG_PRESENT)) {
        spin_unlock(&ctx->lock);
        return false;
    }
    page_table_t *pdpt = (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pml4_e));
    pte_t pdpt_e = pdpt->entries[pdpt_idx];
    if (!(pdpt_e & VMM_FLAG_PRESENT) || (pdpt_e & VMM_FLAG_LARGE_PAGE)) {
        /* unmapped, or this VA is inside a 1 GB page — wrong API */
        spin_unlock(&ctx->lock);
        return false;
    }
    page_table_t *pd = (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pdpt_e));
    pte_t pd_e = pd->entries[pd_idx];
    if (!(pd_e & VMM_FLAG_PRESENT) || !(pd_e & VMM_FLAG_LARGE_PAGE)) {
        /* PDE absent or demoted to 4 KB PT — caller should use vmm_unmap_pages */
        spin_unlock(&ctx->lock);
        return false;
    }

    uint64_t flags = vmm_pte_to_flags(pd_e);
    pd->entries[pd_idx] = 0;

    if (flags & VMM_FLAG_USER) {
        if (ctx->user_pages >= VMM_LARGE_PAGE_2M_PAGES) ctx->user_pages -= VMM_LARGE_PAGE_2M_PAGES;
        atomic_fetch_sub_u64((volatile uint64_t *)&global_stats.user_mapped_pages, VMM_LARGE_PAGE_2M_PAGES);
        atomic_fetch_sub_u64((volatile uint64_t *)&global_stats.total_mapped_pages, VMM_LARGE_PAGE_2M_PAGES);
    } else {
        if (ctx->kernel_pages >= VMM_LARGE_PAGE_2M_PAGES) ctx->kernel_pages -= VMM_LARGE_PAGE_2M_PAGES;
        atomic_fetch_sub_u64((volatile uint64_t *)&global_stats.kernel_mapped_pages, VMM_LARGE_PAGE_2M_PAGES);
        atomic_fetch_sub_u64((volatile uint64_t *)&global_stats.total_mapped_pages, VMM_LARGE_PAGE_2M_PAGES);
    }
    if (ctx->mapped_pages >= VMM_LARGE_PAGE_2M_PAGES) ctx->mapped_pages -= VMM_LARGE_PAGE_2M_PAGES;

    spin_unlock(&ctx->lock);

    /* Stale 2 MB TLB entries may live on remote cores running this
     * context — invalidate them. Single-page IPI covers the full 2 MB
     * mapping at the level the TLB cached. */
    vmm_shootdown_page(ctx, virt_addr);
    return true;
}

uintptr_t vmm_virt_to_phys_huge_2m(vmm_context_t *ctx, uintptr_t virt_addr)
{
    if (!ctx || (virt_addr & VMM_LARGE_PAGE_2M_MASK)) return 0;

    uint32_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    uint32_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    uint32_t pd_idx   = VMM_PD_INDEX(virt_addr);

    /* 5-level: descend PML5 → PML4 first. */
    page_table_t *pml4_table = vmm_walk_pml5_to_pml4(ctx, virt_addr, false);
    if (!pml4_table) return 0;
    pte_t pml4_e = pml4_table->entries[pml4_idx];
    if (!(pml4_e & VMM_FLAG_PRESENT)) return 0;
    page_table_t *pdpt = (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pml4_e));
    pte_t pdpt_e = pdpt->entries[pdpt_idx];
    if (!(pdpt_e & VMM_FLAG_PRESENT) || (pdpt_e & VMM_FLAG_LARGE_PAGE)) return 0;
    page_table_t *pd = (page_table_t *)vmm_phys_to_virt(vmm_pte_to_phys(pdpt_e));
    pte_t pd_e = pd->entries[pd_idx];
    if (!(pd_e & VMM_FLAG_PRESENT) || !(pd_e & VMM_FLAG_LARGE_PAGE)) return 0;
    return vmm_pte_to_phys(pd_e);
}

/*
 * vmm_user_buf_in / vmm_user_buf_alloc_out / vmm_user_buf_commit_out / vmm_user_buf_free
 *
 * The single-leaf guarantee of vmm_translate_user_addr is correct (its
 * kernel pointer covers exactly one mapped frame — 4 KiB, 2 MiB or
 * 1 GiB). For user buffers that span more than one leaf we walk the
 * cabin's page tables leaf by leaf and either copy into a freshly-
 * kmalloc'd kernel buffer (for input crates) or copy out from one (for
 * output crates).
 *
 * The leaf-by-leaf walk handles non-contiguous physical frames, a
 * partial first leaf (offset != 0) and a partial last leaf (size not a
 * multiple of the leaf size). The OUT direction additionally requires
 * R/W on each leaf it writes: see vmm_user_window. A heap page the cabin
 * has not touched yet is brought in on the way, as its own first touch
 * would bring it in: see VmmUserCopyWindow.
 */

/*
 * VmmUserCopyWindow — the window a kernel COPY takes onto cabin memory:
 * vmm_user_window, and where that finds no page, the page the cabin's own
 * first touch would bring in, brought in and looked at once more
 * (VmmFirstTouch). A program's buffer is its buffer whether or not it has
 * been through every page yet — malloc hands out heap that the first touch
 * backs — so a read into one and a write out of one see what the program
 * would. A write out of such a page reads the zeros it was given. Whatever
 * else the walk refuses stays refused: a page the cabin does not have, one
 * it holds read-only for a write, one that is not user memory at all.
 *
 * VmmFirstTouch takes the locks the cabin's own fault takes, in the same
 * order, and only for an address in the heap window. The cabin may be
 * running on another core and touch the same page first; vmm_map_page
 * settles that under ctx->lock.
 */
static void *VmmUserCopyWindow(vmm_context_t *ctx, uintptr_t user_vaddr,
                               bool for_write, size_t *out_span)
{
    void *window = vmm_user_window(ctx, user_vaddr, for_write, out_span);
    if (window || VmmFirstTouch(ctx, user_vaddr) <= 0)
        return window;
    return vmm_user_window(ctx, user_vaddr, for_write, out_span);
}

/*
 * vmm_user_buf_in_into — copy a multi-page user-VA range into a caller-
 * supplied kernel buffer. Page-by-page walk handles non-contiguous physical
 * backing and non-page-aligned start/end. NO allocation: the caller owns
 * the destination buffer (stack, heap, or preallocated scratch).
 *
 * Returns OK, or ERR_INVALID_ADDRESS at the first page that is not user
 * memory the cabin has or can have on first touch; on failure the contents
 * of `kbuf` beyond `[0, fail_offset)` are undefined. Caller is expected to
 * treat partial copies as full failure — i.e. discard kbuf or its
 * downstream interpretation.
 */
error_t vmm_user_buf_in_into(vmm_context_t *ctx, uintptr_t user_vaddr,
                              size_t size, void *kbuf)
{
    if (!ctx || !kbuf || size == 0) return ERR_INVALID_ARGUMENT;

    size_t copied = 0;
    while (copied < size) {
        /* Chunk by the LEAF that maps this address, not by 4 KiB: a
         * 2 MiB-backed buffer is one window, and a 4 KiB one is still
         * exactly one page. vmm_user_window reports the span. */
        size_t span = 0;
        void *src = VmmUserCopyWindow(ctx, user_vaddr + copied, false, &span);
        if (!src || span == 0) return ERR_INVALID_ADDRESS;

        size_t chunk = size - copied;
        if (chunk > span) chunk = span;

        memcpy((uint8_t *)kbuf + copied, src, chunk);
        copied += chunk;
    }
    return OK;
}

void *vmm_user_buf_in(vmm_context_t *ctx, uintptr_t user_vaddr, size_t size)
{
    if (!ctx || size == 0) return NULL;

    void *kbuf = kmalloc(size);
    if (!kbuf) return NULL;

    if (vmm_user_buf_in_into(ctx, user_vaddr, size, kbuf) != OK) {
        kfree(kbuf);
        return NULL;
    }
    return kbuf;
}

void *vmm_user_buf_alloc_out(size_t size)
{
    if (size == 0) return NULL;
    void *kbuf = kmalloc(size);
    if (kbuf) memset(kbuf, 0, size);
    return kbuf;
}

error_t vmm_user_buf_commit_out(vmm_context_t *ctx, uintptr_t user_vaddr,
                                 const void *kbuf, size_t size)
{
    if (!ctx || !kbuf || size == 0) return ERR_INVALID_ARGUMENT;

    size_t copied = 0;
    while (copied < size) {
        /* The WRITE window: the cabin's own leaf must carry R/W. The Pull
         * Map alias this memcpy goes through is supervisor-writable, so
         * the hardware will not refuse a store into a page the cabin gave
         * away read-only — see vmm_user_window. */
        size_t span = 0;
        void *dst = VmmUserCopyWindow(ctx, user_vaddr + copied, true, &span);
        if (!dst || span == 0) return ERR_INVALID_ADDRESS;

        size_t chunk = size - copied;
        if (chunk > span) chunk = span;

        memcpy(dst, (const uint8_t *)kbuf + copied, chunk);
        copied += chunk;
    }
    return OK;
}

void vmm_user_buf_free(void *kbuf)
{
    if (kbuf) kfree(kbuf);
}

typedef struct
{
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct
{
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define EM_X86_64 62

#define VMM_CABIN_MAX_CODE_SIZE (VMM_USER_STACK_TOP - VMM_CABIN_CODE_START - (64 * 1024))

int vmm_map_code_region(vmm_context_t *ctx, uintptr_t code_phys, uint64_t size,
                        uintptr_t *out_entry)
{
    if (out_entry) *out_entry = VMM_CABIN_CODE_START;

    if (!ctx || !code_phys || size == 0)
        return -1;

    // binary must be at least large enough to read ELF header
    if (size < sizeof(Elf64_Ehdr))
    {
        debug_printf("[VMM] Binary too small for ELF header (%llu bytes), loading as flat binary\n", (uint64_t)size);
        // fall through to flat binary path below
    }

    void *elf_virt = vmm_phys_to_virt(code_phys);
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_virt;

    if (size < sizeof(Elf64_Ehdr) ||
        ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F')
    {
        if (size > VMM_CABIN_MAX_CODE_SIZE)
        {
            debug_printf("[VMM] ERROR: Flat binary too large (%llu bytes, max %llu)\n",
                         size, (uint64_t)VMM_CABIN_MAX_CODE_SIZE);
            return -1;
        }

        uint64_t pages = (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
        if (pages < 1)
            pages = 1;

        if (pages > VMM_CABIN_MAX_CODE_SIZE / VMM_PAGE_SIZE)
        {
            debug_printf("[VMM] ERROR: Page count overflow in flat binary loader\n");
            return -1;
        }

        debug_printf("[VMM] Loading raw flat binary at 0x%llx (%llu pages)\n",
                     (uint64_t)VMM_CABIN_CODE_START, pages);
        vmm_map_result_t raw_result = vmm_map_pages(ctx, VMM_CABIN_CODE_START,
                                                    code_phys, (size_t)pages,
                                                    VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE);
        if (!raw_result.success)
        {
            debug_printf("[VMM] ERROR: Failed to map raw binary: %s\n", raw_result.error_msg);
            return -1;
        }
        return 0;
    }

    if (ehdr->e_ident[4] != ELFCLASS64)
    {
        debug_printf("[VMM] ERROR: ELF is not 64-bit (class=%u, expected %u)\n",
                     ehdr->e_ident[4], ELFCLASS64);
        return -1;
    }

    if (ehdr->e_ident[5] != ELFDATA2LSB)
    {
        debug_printf("[VMM] ERROR: ELF is not little-endian (data=%u)\n", ehdr->e_ident[5]);
        return -1;
    }

    if (ehdr->e_type != ET_EXEC)
    {
        debug_printf("[VMM] ERROR: ELF is not executable (type=%u, expected %u)\n",
                     ehdr->e_type, ET_EXEC);
        return -1;
    }

    if (ehdr->e_machine != EM_X86_64)
    {
        debug_printf("[VMM] ERROR: ELF is not x86_64 (machine=%u, expected %u)\n",
                     ehdr->e_machine, EM_X86_64);
        return -1;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0)
    {
        debug_printf("[VMM] ERROR: No program headers found\n");
        return -1;
    }

// validate program headers fit within the binary
#define MAX_LOAD_SEGMENTS 32

    uint64_t phdr_end = (uint64_t)ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(Elf64_Phdr);
    if (phdr_end > size)
    {
        debug_printf("[VMM] ERROR: Program headers extend beyond binary (phdr_end=0x%llx, size=0x%llx)\n",
                     phdr_end, (uint64_t)size);
        return -1;
    }

    if (ehdr->e_phnum > MAX_LOAD_SEGMENTS)
    {
        debug_printf("[VMM] ERROR: Too many program headers (%u, max %d)\n",
                     ehdr->e_phnum, MAX_LOAD_SEGMENTS);
        return -1;
    }

    debug_printf("[VMM] Parsing ELF: %u program headers at offset 0x%llx\n",
                 ehdr->e_phnum, ehdr->e_phoff);

    Elf64_Phdr *phdr_base = (Elf64_Phdr *)((uint8_t *)elf_virt + ehdr->e_phoff);
    size_t total_mapped_pages = 0;

    struct
    {
        uintptr_t phys;
        size_t pages;
    } mapped_segs[MAX_LOAD_SEGMENTS];
    int mapped_seg_count = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++)
    {
        Elf64_Phdr *phdr = &phdr_base[i];

        if (phdr->p_type != PT_LOAD)
        {
            continue;
        }

        uint64_t vaddr = phdr->p_vaddr;
        uint64_t memsz = phdr->p_memsz;
        uint64_t filesz = phdr->p_filesz;
        uint64_t file_offset = phdr->p_offset;
        uint32_t flags = phdr->p_flags;

        if (memsz == 0)
        {
            continue;
        }

        // validate segment data fits within the binary
        if (filesz > 0 && (file_offset + filesz > size))
        {
            debug_printf("[VMM] ERROR: ELF segment %u data extends beyond binary (offset=0x%llx, filesz=0x%llx, binary_size=0x%llx)\n",
                         i, file_offset, filesz, (uint64_t)size);
            for (int k = 0; k < mapped_seg_count; k++)
            {
                pmm_free((void *)mapped_segs[k].phys, mapped_segs[k].pages);
            }
            return -ERR_INVALID_ARGUMENT;
        }

        if (filesz > memsz)
        {
            debug_printf("[VMM] ERROR: ELF segment %u filesz (0x%llx) > memsz (0x%llx)\n",
                         i, filesz, memsz);
            for (int k = 0; k < mapped_seg_count; k++)
            {
                pmm_free((void *)mapped_segs[k].phys, mapped_segs[k].pages);
            }
            return -ERR_INVALID_ARGUMENT;
        }

        uint64_t vaddr_aligned = vaddr & VMM_PAGE_MASK;
        uint64_t page_offset = vaddr & VMM_PAGE_OFFSET_MASK;
        uint64_t memsz_aligned = ((memsz + page_offset + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE) * VMM_PAGE_SIZE;
        size_t page_count = memsz_aligned / VMM_PAGE_SIZE;

        if (vaddr_aligned < CABIN_CODE_START_ADDR)
        {
            debug_printf("[VMM] ERROR: Invalid ELF segment vaddr 0x%llx (below 0x%llx)\n", vaddr_aligned, (uint64_t)CABIN_CODE_START_ADDR);
            for (int k = 0; k < mapped_seg_count; k++)
            {
                pmm_free((void *)mapped_segs[k].phys, mapped_segs[k].pages);
            }
            return -ERR_INVALID_ARGUMENT;
        }

        void *segment_phys_ptr = pmm_alloc(page_count);
        if (!segment_phys_ptr)
        {
            debug_printf("[VMM] ERROR: Failed to allocate %zu pages for segment %u\n", page_count, i);
            for (int k = 0; k < mapped_seg_count; k++)
            {
                pmm_free((void *)mapped_segs[k].phys, mapped_segs[k].pages);
            }
            return -1;
        }
        uintptr_t segment_phys = (uintptr_t)segment_phys_ptr;

        void *segment_virt = vmm_phys_to_virt(segment_phys);
        memset(segment_virt, 0, memsz_aligned);

        if (filesz > 0)
        {
            void *file_data = (uint8_t *)elf_virt + file_offset;
            memcpy((uint8_t *)segment_virt + page_offset, file_data, filesz);
        }

        uint64_t vmm_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;

        if (flags & PF_W)
        {
            vmm_flags |= VMM_FLAG_WRITABLE;
        }

        if (!(flags & PF_X))
        {
            vmm_flags |= VMM_FLAG_NO_EXECUTE;
        }

        vmm_map_result_t result = vmm_map_pages(ctx, vaddr_aligned, segment_phys,
                                                page_count, vmm_flags);
        if (!result.success)
        {
            debug_printf("[VMM] ERROR: Failed to map segment %u: %s\n", i, result.error_msg);
            pmm_free((void *)segment_phys, page_count);
            for (int k = 0; k < mapped_seg_count; k++)
            {
                pmm_free((void *)mapped_segs[k].phys, mapped_segs[k].pages);
            }
            return -1;
        }

        if (mapped_seg_count < MAX_LOAD_SEGMENTS)
        {
            mapped_segs[mapped_seg_count].phys = segment_phys;
            mapped_segs[mapped_seg_count].pages = page_count;
            mapped_seg_count++;
        }

        const char *perm_str = "";
        if ((flags & PF_R) && (flags & PF_W) && !(flags & PF_X))
        {
            perm_str = "R+W, NX";
        }
        else if ((flags & PF_R) && (flags & PF_X) && !(flags & PF_W))
        {
            perm_str = "R+X";
        }
        else if ((flags & PF_R) && !(flags & PF_W) && !(flags & PF_X))
        {
            perm_str = "R, NX";
        }
        else
        {
            perm_str = "custom";
        }

        debug_printf("[VMM] Mapped segment %u: virt=0x%04llx-0x%04llx phys=0x%lx pages=%zu flags=%s filesz=%llu memsz=%llu\n",
                     i, vaddr_aligned, vaddr_aligned + memsz_aligned - 1,
                     segment_phys, page_count, perm_str, filesz, memsz);

        total_mapped_pages += page_count;
    }

    debug_printf("[VMM] ELF binary mapped: %zu total pages (W^X enforced)\n", total_mapped_pages);

    /* Publish entry point from the ELF header. Production binaries link
     * with `.text=0xC000` so this typically equals VMM_CABIN_CODE_START
     * (the early-set default), but honouring e_entry lets a future
     * linker move the start without breaking process spawn. */
    if (out_entry) *out_entry = (uintptr_t)ehdr->e_entry;
    return 0;
}

// page fault error code bits (Intel SDM Vol. 3A, Table 6-3)
#define PF_PRESENT (1 << 0)
#define PF_WRITE (1 << 1)
#define PF_USER (1 << 2)
#define PF_RESERVED (1 << 3) // reserved bit set in page table entry
#define PF_INSTR (1 << 4)    // instruction fetch
#define PF_PK   (1 << 5)     // protection-key violation (Phase 2H; SDM §4.6.2)
#define PF_SS   (1 << 6)     // shadow-stack access (Phase 2K placeholder; §17)

int vmm_handle_page_fault(uintptr_t fault_addr, uint64_t error_code)
{
    bool present = error_code & PF_PRESENT;
    bool write = error_code & PF_WRITE;
    bool user = error_code & PF_USER;
    bool reserved = error_code & PF_RESERVED;
    bool instr_fetch = error_code & PF_INSTR;

    debug_printf("[VMM] Page fault at 0x%llx (error=0x%llx)\n", fault_addr, error_code);
    debug_printf("[VMM]   present=%d write=%d user=%d reserved=%d instr=%d\n",
                 present, write, user, reserved, instr_fetch);

    /* Phase 2H — PF.PK decode. Bit 5 of the error code signals a
     * Protection-Key violation (Intel SDM Vol 3A §4.7). We publish a
     * Touch event with the PTE's PKEY field + current PKRU so
     * subscribers can decide policy. The fault still propagates to
     * the existing kill-process / kernel-panic flow below — Phase 2H
     * is observe-only this iteration; per-process PKRU lifecycle is
     * the next step that turns this into a recoverable signal. */
    if (error_code & PF_PK) {
        process_t *pku_proc = process_get_current();
        uint32_t pku_pid = pku_proc ? pku_proc->pid : 0;
        uint8_t pkey = 0;
        vmm_context_t *pku_ctx = (pku_proc && pku_proc->cabin) ? pku_proc->cabin->vmm : vmm_get_current_context();
        if (pku_ctx) {
            uint8_t lvl = 0;
            pte_t *p = vmm_get_leaf_pte(pku_ctx, fault_addr & ~(VMM_PAGE_SIZE - 1), &lvl);
            if (p) pkey = vmm_pte_pkey(__atomic_load_n(p, __ATOMIC_ACQUIRE));
        }
        struct { uint32_t pid; uint32_t pkey; uint64_t va; uint32_t pkru; uint32_t pad; } ev = {
            .pid = pku_pid, .pkey = pkey, .va = fault_addr,
            .pkru = vmm_read_pkru(), .pad = 0,
        };
        /* IRQ-safe publish: TouchPublishIrqPair takes pre-resolved tag
         * handles + bounded payload, copies into a static slot, defers
         * to K-Core. Direct TouchPublish here would risk deadlock if
         * the faulting user thread held a Touch lock the publish path
         * also wants. */
        if (g_vmm_tag_pku_fault != TOUCH_TAG_INVALID) {
            TouchPublishIrqPair(g_vmm_tag_pku_fault, TOUCH_TAG_INVALID,
                                &ev, (uint16_t)sizeof(ev),
                                pku_pid, 0u);
        }
    }

    if (reserved)
    {
        debug_printf("[VMM] ERROR: Reserved bit violation - cannot handle\n");
        return -1;
    }

    process_t *current = process_get_current();
    if (current && current->kernel_stack_guard_base)
    {
        uintptr_t guard_start = (uintptr_t)current->kernel_stack_guard_base;
        uintptr_t guard_end = guard_start + (CONFIG_KERNEL_STACK_GUARD_PAGES * VMM_PAGE_SIZE);

        if (fault_addr >= guard_start && fault_addr < guard_end)
        {
            debug_printf("[VMM] ========================================\n");
            debug_printf("[VMM] WARNING: Kernel stack overflow detected (page fault path)!\n");
            debug_printf("[VMM] ========================================\n");
            debug_printf("[VMM]   Process PID: %u\n", current->pid);
            debug_printf("[VMM]   Fault address: 0x%016lx\n", fault_addr);
            debug_printf("[VMM]   Guard region: 0x%016lx - 0x%016lx\n", guard_start, guard_end);
            debug_printf("[VMM]   Stack region: 0x%016lx - 0x%016lx\n",
                         (uintptr_t)current->kernel_stack,
                         (uintptr_t)current->kernel_stack_top);
            debug_printf("[VMM]   Error code: 0x%llx\n", error_code);
            debug_printf("[VMM] ========================================\n");
            debug_printf("[VMM] Primary recovery via double fault handler (IST)\n");
            return -1;
        }
    }

    /* For user-mode faults the active CR3 belongs to the faulting process'
     * cabin, but `current_context` is only updated once at boot — the
     * scheduler switches CR3 in assembly without touching the C-side global.
     * Reach for the real cabin via process_get_current() instead, falling
     * back to whatever `current_context` says for kernel-mode faults. */
    vmm_context_t *ctx = NULL;
    if (user && current) {
        ctx = current->cabin ? current->cabin->vmm : NULL;
    }
    if (!ctx) {
        ctx = vmm_get_current_context();
    }
    if (ctx && user)
    {
        // ASLR: use per-process stack top from VMM context
        uint64_t stack_top = ctx->stack_top;
        uint64_t guard_base = stack_top - (CONFIG_USER_STACK_TOTAL_PAGES * VMM_PAGE_SIZE);
        uint64_t guard_end = guard_base + (CONFIG_USER_STACK_GUARD_PAGES * VMM_PAGE_SIZE);

        if (fault_addr >= guard_base && fault_addr < guard_end)
        {
            debug_printf("[VMM] ERROR: Stack overflow detected (guard page access at 0x%llx)\n", fault_addr);
            return -1;
        }

        /* MemTag Phase 2B + 2D — capability enforcement at fault time.
         *
         * Phase 2B: for faults on a PRESENT page (real protection-fault)
         * or on a page whose backing region carries a guard the cabin
         * doesn't hold: deny + publish memtag:fault:denied + return -1
         * → cabin gets the standard user-PF kill path. Skip for unmapped
         * pages with no region (lazy-alloc cases continue below).
         *
         * Phase 2D fast-path: single vmm_get_leaf_pte walk yields both
         * phys (via addr_mask) AND the encoded region_id (PTE bits 52-58
         * stamped by StampPteRegion at attach time). MemRegionFromPte
         * verifies the encoded id covers `phys` and falls back to the
         * dense reverse index on collision. Replaces the previous
         * virt_to_phys + MemRegionFromPhys two-step. */
        if (current && current->pid != 0) {
            uintptr_t page_va = fault_addr & ~(VMM_PAGE_SIZE - 1);
            uint8_t   level   = 0;
            pte_t    *pte_ptr = vmm_get_leaf_pte(ctx, page_va, &level);
            if (pte_ptr) {
                pte_t     pte_val = __atomic_load_n(pte_ptr, __ATOMIC_ACQUIRE);
                uintptr_t phys    = pte_val & vmm_get_addr_mask();
                if (phys) {
                    uint32_t rid = MemRegionFromPte(pte_val, phys);
                    if (rid != MEMTAG_INVALID_REGION_ID) {
                        if (!MemTagEnforce(current->pid, fault_addr, rid)) {
                            debug_printf("[VMM] PID %u → 0x%lx: memtag-denied (rid=%u)\n",
                                         current->pid, fault_addr, rid);
                            return -1;
                        }
                    }
                }
            }
        }
    }

    // User heap demand paging — the rule is VmmFirstTouch's, shared with the
    // kernel's copies into and out of cabin memory.
    if (ctx && user && !present)
    {
        int touched = VmmFirstTouch(ctx, fault_addr);
        if (touched != 0)
            return touched > 0 ? 0 : -1;
    }

    // Phase 11: PocketRing / ResultRing / TouchRing slot regions —
    // lazy first-touch mapping. The producer side eagerly maps pages
    // via vmm_ensure_user_page; this fault path covers the rare case
    // of userspace touching a slot VA before the kernel producer has
    // (e.g. premature consumer probe). Once mapped, a slot page stays
    // mapped and is reused via the monotonic-index modulo wrap.
    if (ctx && !present &&
        ((fault_addr >= CABIN_POCKET_SLOTS_BASE && fault_addr < CABIN_POCKET_SLOTS_END) ||
         (fault_addr >= CABIN_RESULT_SLOTS_BASE && fault_addr < CABIN_RESULT_SLOTS_END) ||
         (fault_addr >= CABIN_TOUCH_SLOTS_BASE  && fault_addr < CABIN_TOUCH_SLOTS_END)))
    {
        uintptr_t page_addr = fault_addr & ~(VMM_PAGE_SIZE - 1);

        if (vmm_is_mapped(ctx, page_addr)) {
            return 0;
        }

        void *phys = pmm_alloc_zero(1);
        if (!phys) {
            debug_printf("[VMM] ERROR: Out of memory for ring slot page at 0x%lx\n", page_addr);
            return -1;
        }

        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE;
        vmm_map_result_t r = vmm_map_page(ctx, page_addr, (uintptr_t)phys, flags);
        if (!r.success) {
            pmm_free(phys, 1);
            debug_printf("[VMM] ERROR: Failed to map ring slot page at 0x%lx\n", page_addr);
            return -1;
        }
        debug_printf("[VMM] Demand paging: mapped ring slot page at 0x%lx\n", page_addr);
        return 0;
    }

    if (present)
    {
        debug_printf("[VMM] ERROR: Protection fault - access denied\n");
        return -1;
    }

    debug_printf("[VMM] Page not present - attempting demand paging\n");

    if (!ctx)
    {
        ctx = kernel_context;
    }

    uintptr_t page_addr = fault_addr & ~(VMM_PAGE_SIZE - 1);

    if (page_addr >= VMM_KERNEL_HEAP_BASE &&
        page_addr < VMM_KERNEL_HEAP_BASE + VMM_KERNEL_HEAP_SIZE)
    {

        debug_printf("[VMM] Demand paging: mapping kernel heap page at 0x%llx\n", page_addr);

        void *phys_page = pmm_alloc(1);
        if (!phys_page)
        {
            debug_printf("[VMM] ERROR: Failed to allocate physical page\n");
            return -1;
        }

        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        if (user)
        {
            flags |= VMM_FLAG_USER;
        }

        vmm_map_result_t result = vmm_map_page(ctx, page_addr, (uintptr_t)phys_page, flags);
        if (!result.success)
        {
            debug_printf("[VMM] ERROR: Failed to map page\n");
            pmm_free(phys_page, 1);
            return -1;
        }

        debug_printf("[VMM] SUCCESS: Demand paging successful\n");
        return 0;
    }

    /* No identity-restore fallback. Once vmm_init() flips g_pull_map_active,
     * the identity window is dead — any fault landing there is a real bug
     * and must surface. The early-boot window where !g_pull_map_active is
     * true never raises page faults that need restoration (stage2 identity
     * tables cover low RAM). The previous fallback could recurse via
     * vmm_map_page → pmm_alloc → memset through the very identity window
     * it was trying to repair. */

    debug_printf("[VMM] ERROR: Fault address not in valid range (0x%llx)\n", fault_addr);
    return -1;
}
