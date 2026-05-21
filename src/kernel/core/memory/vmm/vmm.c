#include "vmm.h"
#include "pmm.h"
#include "klib.h"
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
#include "cabin_layout.h"

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
#define PAT_TYPE_WB    0x06U
#define PAT_TYPE_WT    0x04U
#define PAT_TYPE_UCM   0x07U   /* UC- (weakly uncacheable) */
#define PAT_TYPE_UC    0x00U
#define PAT_TYPE_WC    0x01U   /* Write Combining            */

static void vmm_pat_init(void)
{
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

    debug_printf("[VMM] PAT MSR programmed: PA6=WC (framebuffer Write Combining enabled)\n");
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
    
    // PCID exhausted — reset with full TLB flush
    pcid_next = 2;
    pcid_free_count = 0;
    *out_pcid = 1;

    uintptr_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    asm volatile("mov %0, %%cr4" : : "r"(cr4 & ~(1ULL << 7)) : "memory");
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    if (g_amp.multicore_active && g_amp.total_cores > 1)
    {
        vmm_shootdown_pages(kernel_context, 0, 0);
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

uint64_t vmm_build_cr3(vmm_context_t *ctx)
{
    if (!ctx)
        return 0;
    if (g_pcid_active)
    {
        return ctx->pml4_phys | (uint64_t)ctx->pcid;
    }
    return ctx->pml4_phys;
}

uint64_t vmm_build_cr3_noflush(vmm_context_t *ctx)
{
    if (!ctx)
        return 0;
    if (g_pcid_active)
    {
        return ctx->pml4_phys | (uint64_t)ctx->pcid | CR3_NOFLUSH;
    }
    return ctx->pml4_phys;
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
    volatile bool active;
} __attribute__((aligned(64))) g_shootdown;

static spinlock_t g_shootdown_lock;

void vmm_tlb_shootdown_handler(void)
{
    if (!g_shootdown.active)
        return;

    uintptr_t addr = g_shootdown.addr;
    uint32_t count = g_shootdown.page_count;

    if (addr == 0 || count == 0 || count > 64)
    {
        /* Full TLB flush — must clear CR3 bit 63 (NOFLUSH) or the CPU
         * keeps stale entries when PCID is on. See vmm_flush_tlb for
         * the full story. */
        uintptr_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        cr3 &= ~(1ULL << 63);
        asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    }
    else
    {
        for (uint32_t i = 0; i < count; i++)
        {
            asm volatile("invlpg (%0)" : : "r"(addr + i * VMM_PAGE_SIZE) : "memory");
        }
    }

    atomic_fetch_sub_u32(&g_shootdown.pending_acks, 1);
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
        if (!g_amp.cores[c].online)
            continue;

        if (is_kernel)
        {
            targets[target_count++] = c;
        }
        else
        {
            scheduler_state_t *rs = scheduler_get_core(c);
            process_t *rp = rs->current_process; // snapshot, no lock needed
            if (rp && rp->cabin && rp->cabin->pml4_phys == ctx->pml4_phys)
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

    // Arm the shootdown descriptor and broadcast
    spin_lock(&g_shootdown_lock);

    g_shootdown.addr = (page_count <= 64) ? virt_addr : 0;
    g_shootdown.page_count = (page_count <= 64) ? (uint32_t)page_count : 0;
    atomic_store_u32(&g_shootdown.pending_acks, target_count);
    g_shootdown.active = true;
    mfence();

    for (uint8_t i = 0; i < target_count; i++)
    {
        lapic_send_ipi(g_amp.cores[targets[i]].lapic_id, IPI_SHOOTDOWN_VECTOR);
    }

    /* Spin until all targets ACK with timeout.
     *
     * Two pre-existing bugs in this loop, exposed when more call-sites
     * started using cross-core shootdown:
     *   (1) `tsc_freq_mhz * 100` is 100 *microseconds*, not 100 ms as
     *       the comment claimed — far too tight for the IPI round-trip
     *       on a heavily-loaded BSP.
     *   (2) If `cpu_get_tsc_freq_mhz()` hasn't completed calibration
     *       (it returns 0), `timeout_cycles` is 0 and the very first
     *       iteration trips the timeout while the ACKs are still
     *       in-flight — observed as
     *       "TLB shootdown timeout: 0 cores did not ACK (pending_acks=0, spins=1)".
     *   (3) Timeout fired between the while-condition read and this
     *       check — by the time we panic, ACKs may have arrived.
     *       Re-load `pending_acks` and break gracefully if so. */
    uint64_t tsc_freq_mhz = cpu_get_tsc_freq_mhz();
    if (tsc_freq_mhz < 100) tsc_freq_mhz = 1000; /* fallback: assume 1 GHz */
    uint64_t timeout_cycles = tsc_freq_mhz * 100000ULL; /* 100 ms */
    uint64_t start_tsc = rdtsc();
    uint32_t spins = 0;
    const uint32_t max_spins = 10000000; // Safety limit

    while (atomic_load_u32(&g_shootdown.pending_acks) != 0)
    {
        cpu_pause();
        spins++;

        // Check timeout (both by cycles and spin count)
        if (spins >= max_spins || (rdtsc() - start_tsc) > timeout_cycles)
        {
            uint32_t remaining = atomic_load_u32(&g_shootdown.pending_acks);
            if (remaining == 0) break; /* race: ACKs landed during timeout calc */
            panic("TLB shootdown timeout: %u cores did not ACK (pending_acks=%u, spins=%u)",
                  remaining, remaining, spins);
        }
    }

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
 * crash logs. */
void vmm_shootdown_all_cores_full(void)
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
        if (!g_amp.cores[c].online) continue;
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

    spin_lock(&g_shootdown_lock);

    g_shootdown.addr = 0;          /* 0 ⇒ handler does full flush */
    g_shootdown.page_count = 0;
    atomic_store_u32(&g_shootdown.pending_acks, target_count);
    g_shootdown.active = true;
    mfence();

    for (uint8_t i = 0; i < target_count; i++) {
        lapic_send_ipi(g_amp.cores[targets[i]].lapic_id, IPI_SHOOTDOWN_VECTOR);
    }

    uint64_t tsc_freq_mhz = cpu_get_tsc_freq_mhz();
    uint64_t timeout_cycles = tsc_freq_mhz * 100;          /* 100 ms */
    uint64_t start_tsc = rdtsc();
    uint32_t spins = 0;
    const uint32_t max_spins = 10000000;

    while (atomic_load_u32(&g_shootdown.pending_acks) != 0) {
        cpu_pause();
        spins++;
        if (spins >= max_spins || (rdtsc() - start_tsc) > timeout_cycles) {
            uint32_t remaining = atomic_load_u32(&g_shootdown.pending_acks);
            panic("Full TLB shootdown timeout: %u/%u cores did not ACK",
                  remaining, target_count);
        }
    }

    g_shootdown.active = false;
    spin_unlock(&g_shootdown_lock);
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

    page_table_t *current_table = ctx->pml4;
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

    page_table_t *pml4 = ctx->pml4;
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

static void vmm_free_user_space_tables(vmm_context_t *ctx)
{
    if (!ctx || !ctx->pml4)
        return;

    debug_printf("[VMM] Freeing user space tables for context CR3=0x%lx\n", ctx->pml4_phys);

    // bitmap to detect duplicate PT entries pointing to the same physical page
    // sized dynamically from pmm_get_mem_end() so all physical RAM is covered
    uint64_t dedup_mem_end = pmm_get_mem_end();
    size_t dedup_total_pages = dedup_mem_end / VMM_PAGE_SIZE;
    size_t dedup_bitmap_size = (dedup_total_pages + 7) / 8;

    uint8_t *freed_bitmap = kmalloc(dedup_bitmap_size);
    bool has_dedup = (freed_bitmap != NULL);
    if (!has_dedup)
    {
        debug_printf("[VMM] WARNING: kmalloc(%zu) failed for dedup bitmap — skipping user page frees to avoid double-free\n", dedup_bitmap_size);
    }
    else
    {
        memset(freed_bitmap, 0, dedup_bitmap_size);
    }
    // When has_dedup is false we still walk the tables to free page-table
    // structures but SKIP freeing data pages (pmm_free) to prevent
    // double-free if two PTEs point to the same physical frame.

    for (int p4 = 0; p4 < 256; p4++)
    {
        pte_t pml4_entry = ctx->pml4->entries[p4];
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

                    uintptr_t virt = (p4 * 512ULL * 1024 * 1024 * 1024) +
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

                    /* Phase 1 of the two-phase teardown: zero the PTE here
                     * but DO NOT pmm_free the underlying page yet — only
                     * mark it in the dedup bitmap. The actual frees happen
                     * after a cross-core TLB shootdown so no AMP core can
                     * still have cached translations to a PA that PMM is
                     * about to hand out to a different cabin. */
                    if (!is_identity_mapped && has_dedup)
                    {
                        size_t page_idx = phys / VMM_PAGE_SIZE;
                        if (page_idx < dedup_total_pages)
                        {
                            size_t byte_idx = page_idx / 8;
                            size_t bit_idx = page_idx % 8;
                            if (!(freed_bitmap[byte_idx] & (1 << bit_idx)))
                            {
                                freed_bitmap[byte_idx] |= (1 << bit_idx);
                                freed_pages++;
                            }
                        }
                    }

                    if (!is_identity_mapped)
                    {
                        pt->entries[p1] = 0;
                    }
                }

                if (freed_pages > 0)
                {
                    debug_printf("[VMM]   Marked %d data pages for deferred free\n", freed_pages);
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
        ctx->pml4->entries[p4] = 0;
    }

    /* Phase 2: cross-core TLB shootdown. ALL online cores must drop any
     * cached entries that referenced this cabin's user mappings (PCID
     * caching means a CR3 swap alone does not flush them). Only after
     * the shootdown completes is it safe to return the underlying
     * physical pages to PMM — otherwise the next pmm_alloc on another
     * core could hand the page to a different cabin while a stale TLB
     * entry still maps it on a third core, causing the wild-write
     * memory-corruption crash we hunted on 2026-04-29. */
    vmm_shootdown_all_cores_full();

    /* Phase 3: now safe to actually return the pages. */
    if (freed_bitmap)
    {
        size_t freed_total = 0;
        for (size_t pg = 0; pg < dedup_total_pages; pg++)
        {
            size_t byte_idx = pg / 8;
            size_t bit_idx = pg % 8;
            if (freed_bitmap[byte_idx] & (1 << bit_idx))
            {
                pmm_free((void *)(uintptr_t)(pg * VMM_PAGE_SIZE), 1);
                freed_total++;
            }
        }
        kfree(freed_bitmap);
        debug_printf("[VMM] User space tables: deferred-freed %zu data pages after TLB shootdown\n",
                     freed_total);
    }
    else
    {
        /* No dedup bitmap: we did not collect any frees. Still flush
         * locally so this CPU does not see stale entries. Clear bit 63
         * (NOFLUSH) explicitly — see vmm_flush_tlb. */
        uintptr_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        cr3 &= ~(1ULL << 63);
        asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
        debug_printf("[VMM] User space tables freed (no dedup bitmap, no data pages reclaimed)\n");
    }

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

    if (*pte & VMM_FLAG_PRESENT)
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

    vmm_flush_tlb_page(virt_addr);

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
     * → PAT index = (PAT_bit<<2)|(PCD<<1)|PWT = 4|2|0 = 6 = WC (programmed by vmm_pat_init)
     */
    uint64_t wc_flags = VMM_FLAGS_KERNEL_RW | VMM_FLAG_CACHE_DISABLE | VMM_FLAG_PAT_BIT;

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

    uintptr_t phys = vmm_virt_to_phys(vmm_get_current_context(), (uintptr_t)addr);
    if (phys)
    {
        debug_printf("[VMM] vfree: unmapped single page virt=%p phys=%p\n", addr, (void *)phys);
        vmm_unmap_page(vmm_get_current_context(), (uintptr_t)addr);
        pmm_free((void *)phys, 1);
    }
    else
    {
        debug_printf("[VMM] vfree: WARNING: could not resolve physical address for %p\n", addr);
    }
}

uintptr_t vmm_virt_to_phys(vmm_context_t *ctx, uintptr_t virt_addr)
{
    if (!ctx)
        return 0;

    spin_lock(&ctx->lock);

    pte_t *pte = vmm_get_pte(ctx, virt_addr);
    if (!pte || !(*pte & VMM_FLAG_PRESENT))
    {
        spin_unlock(&ctx->lock);
        return 0;
    }

    uintptr_t phys_base = vmm_pte_to_phys(*pte);
    uintptr_t offset = virt_addr & VMM_PAGE_OFFSET_MASK;

    spin_unlock(&ctx->lock);

    return phys_base + offset;
}

bool vmm_is_mapped(vmm_context_t *ctx, uintptr_t virt_addr)
{
    return vmm_virt_to_phys(ctx, virt_addr) != 0;
}

int vmm_ensure_user_page(vmm_context_t *ctx, uintptr_t user_vaddr, bool writable)
{
    if (!ctx) return -1;

    uintptr_t page_addr = user_vaddr & ~(VMM_PAGE_SIZE - 1);
    if (vmm_is_mapped(ctx, page_addr)) {
        return 0;
    }

    void *phys = pmm_alloc_zero(1);
    if (!phys) return -1;

    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NO_EXECUTE;
    if (writable) flags |= VMM_FLAG_WRITABLE;

    vmm_map_result_t r = vmm_map_page(ctx, page_addr, (uintptr_t)phys, flags);
    if (!r.success) {
        /* Race tolerance: another producer mapped the same page between
         * our vmm_is_mapped probe and vmm_map_page. vmm_map_page rejects
         * the duplicate; free our spare phys page and report success if
         * the page is now genuinely mapped. Without this, MPSC producers
         * landing on a fresh slot page will see spurious failures. */
        pmm_free(phys, 1);
        if (vmm_is_mapped(ctx, page_addr)) {
            return 0;
        }
        return -1;
    }
    return 0;
}

uint64_t vmm_get_page_flags(vmm_context_t *ctx, uintptr_t virt_addr)
{
    if (!ctx)
        return 0;

    spin_lock(&ctx->lock);

    pte_t *pte = vmm_get_pte(ctx, virt_addr);
    if (!pte || !(*pte & VMM_FLAG_PRESENT))
    {
        spin_unlock(&ctx->lock);
        return 0;
    }

    uint64_t flags = vmm_pte_to_flags(*pte);
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

    spinlock_init(&kernel_heap_lock);
    spinlock_init(&vmalloc_lock);
    spinlock_init(&kernel_mmio_lock);
    spinlock_init(&g_shootdown_lock);

    kernel_context = vmm_create_context();
    if (!kernel_context)
    {
        panic("Failed to create kernel VMM context");
    }

    debug_printf("[VMM] Kernel context created at %p\n", kernel_context);
    debug_printf("[VMM] PML4 physical address: 0x%p\n", (void *)kernel_context->pml4_phys);

    // Higher-half kernel mapping: map kernel code+data+stack at PML4[511] PDPT[510]
    // using 2MB large pages. No identity mapping — kernel runs at 0xFFFFFFFF80100000+
    uint64_t total_mem = pmm_get_total_memory();

#define LARGE_PAGE_SIZE (2 * 1024 * 1024)
#define HIGHER_HALF_BASE 0xFFFFFFFF80000000ULL
// Map first 64MB at higher-half (covers kernel + boot infrastructure)
#define HIGHER_HALF_MAP_SIZE (64ULL * 1024 * 1024)
    size_t large_pages_mapped = 0;

    debug_printf("[VMM] Setting up higher-half kernel mapping at 0x%p (%llu MB)...\n",
                 (void *)HIGHER_HALF_BASE, HIGHER_HALF_MAP_SIZE / (1024 * 1024));

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

    // Pull Map: map all physical RAM at PULL_MAP_BASE using 1GB or 2MB pages
    bool use_1gb_pages = g_cpu_caps.has_1gb_pages;

    uintptr_t pull_pdpt_phys = vmm_alloc_page_table();
    if (!pull_pdpt_phys)
    {
        panic("Failed to allocate Pull Map PDPT");
    }
    // g_pull_map_active is false, so vmm_phys_to_virt returns identity — safe to access
    page_table_t *pull_pdpt = (page_table_t *)vmm_phys_to_virt(pull_pdpt_phys);

    if (use_1gb_pages)
    {
        size_t num_1gb = (total_mem + (1ULL << 30) - 1) >> 30;
        if (num_1gb > 512)
            num_1gb = 512;

        debug_printf("[VMM] Pull Map: using 1GB pages (%zu entries for %llu MB)\n",
                     num_1gb, total_mem / (1024 * 1024));

        for (size_t i = 0; i < num_1gb; i++)
        {
            uintptr_t phys = (uintptr_t)i << 30;
            pull_pdpt->entries[i] = vmm_make_pte(phys,
                                                 VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_GLOBAL | VMM_FLAG_LARGE_PAGE);
        }
    }
    else
    {
        size_t num_1gb_ranges = (total_mem + (1ULL << 30) - 1) >> 30;
        if (num_1gb_ranges > 512)
            num_1gb_ranges = 512;

        debug_printf("[VMM] Pull Map: using 2MB pages (no 1GB page support), %zu PD tables\n",
                     num_1gb_ranges);

        for (size_t i = 0; i < num_1gb_ranges; i++)
        {
            uintptr_t pd_phys = vmm_alloc_page_table();
            if (!pd_phys)
            {
                panic("Failed to allocate Pull Map PD");
            }
            page_table_t *pd = (page_table_t *)vmm_phys_to_virt(pd_phys);

            for (int j = 0; j < 512; j++)
            {
                uintptr_t phys = ((uintptr_t)i << 30) + ((uintptr_t)j << 21);
                if (phys >= total_mem)
                    break;
                pd->entries[j] = vmm_make_pte(phys,
                                              VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_GLOBAL | VMM_FLAG_LARGE_PAGE);
            }

            pull_pdpt->entries[i] = vmm_make_pte(pd_phys,
                                                 VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
        }
    }

    kernel_context->pml4->entries[PULL_MAP_PML4_INDEX] =
        vmm_make_pte(pull_pdpt_phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);

    debug_printf("[VMM] Pull Map installed at PML4[%d] = 0x%lx\n",
                 PULL_MAP_PML4_INDEX, pull_pdpt_phys);

    debug_printf("[VMM] Kernel heap will be mapped on demand starting at 0x%p\n",
                 (void *)VMM_KERNEL_HEAP_BASE);

    debug_printf("[VMM] Kernel MMIO region: %p - %p (on-demand)\n",
                 (void *)VMM_KERNEL_MMIO_BASE,
                 (void *)(VMM_KERNEL_MMIO_BASE + VMM_KERNEL_MMIO_SIZE));

    // Save values that will be inaccessible after identity mapping is removed.
    // kernel_context was kmalloc'd at identity address — will be a dangling pointer after switch.
    uintptr_t saved_pml4_phys = kernel_context->pml4_phys;
    uintptr_t saved_ctx_phys = (uintptr_t)kernel_context;

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
    debug_printf("[VMM]   Pull Map:        0x%p - 0x%p (%s pages)\n",
                 (void *)PULL_MAP_BASE,
                 (void *)(PULL_MAP_BASE + total_mem),
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

    debug_printf("[VMM] Page table dump for virtual address 0x%p:\n", (void *)virt_addr);
    debug_printf("[VMM]   PML4 index: %d\n", VMM_PML4_INDEX(virt_addr));
    debug_printf("[VMM]   PDPT index: %d\n", VMM_PDPT_INDEX(virt_addr));
    debug_printf("[VMM]   PD index:   %d\n", VMM_PD_INDEX(virt_addr));
    debug_printf("[VMM]   PT index:   %d\n", VMM_PT_INDEX(virt_addr));

    spin_lock(&ctx->lock);

    page_table_t *pml4 = ctx->pml4;
    if (!pml4)
    {
        debug_printf("[VMM]   PML4: NULL\n");
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

vmm_context_t *vmm_create_cabin(uint64_t *cabin_info_phys, uint64_t *pocket_ring_phys, uint64_t *result_ring_phys)
{
    if (!cabin_info_phys || !pocket_ring_phys || !result_ring_phys)
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

    // Allocate physical pages for CabinInfo (1 page), PocketRing (1 page), ResultRing (9 pages)
    void *info_phys = pmm_alloc(CABIN_INFO_PAGES);
    if (!info_phys)
    {
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to allocate CabinInfo page");
        return NULL;
    }

    void *pocket_phys = pmm_alloc(CABIN_POCKET_RING_PAGES);
    if (!pocket_phys)
    {
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to allocate PocketRing page");
        return NULL;
    }

    void *result_phys = pmm_alloc(CABIN_RESULT_RING_PAGES);
    if (!result_phys)
    {
        pmm_free(pocket_phys, CABIN_POCKET_RING_PAGES);
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to allocate ResultRing pages");
        return NULL;
    }

    memset(vmm_phys_to_virt((uintptr_t)info_phys), 0, CABIN_INFO_SIZE);
    memset(vmm_phys_to_virt((uintptr_t)pocket_phys), 0, CABIN_POCKET_RING_SIZE);
    memset(vmm_phys_to_virt((uintptr_t)result_phys), 0, CABIN_RESULT_RING_SIZE);

    if (vmm_setup_null_trap(cabin_ctx) != 0)
    {
        pmm_free(result_phys, CABIN_RESULT_RING_PAGES);
        pmm_free(pocket_phys, CABIN_POCKET_RING_PAGES);
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to setup NULL trap");
        return NULL;
    }

    if (vmm_map_cabin_info(cabin_ctx, (uintptr_t)info_phys) != 0)
    {
        pmm_free(result_phys, CABIN_RESULT_RING_PAGES);
        pmm_free(pocket_phys, CABIN_POCKET_RING_PAGES);
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to map CabinInfo");
        return NULL;
    }

    if (vmm_map_pocket_ring(cabin_ctx, (uintptr_t)pocket_phys) != 0)
    {
        pmm_free(result_phys, CABIN_RESULT_RING_PAGES);
        pmm_free(pocket_phys, CABIN_POCKET_RING_PAGES);
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to map PocketRing");
        return NULL;
    }

    if (vmm_map_result_ring(cabin_ctx, (uintptr_t)result_phys) != 0)
    {
        pmm_free(result_phys, CABIN_RESULT_RING_PAGES);
        pmm_free(pocket_phys, CABIN_POCKET_RING_PAGES);
        pmm_free(info_phys, CABIN_INFO_PAGES);
        vmm_destroy_context(cabin_ctx);
        vmm_set_error("Failed to map ResultRing");
        return NULL;
    }

    if (g_cpu_caps_page_phys != 0)
    {
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        vmm_map_result_t map_result = vmm_map_page(cabin_ctx, CPU_CAPS_PAGE_ADDR, g_cpu_caps_page_phys, flags);
        if (!map_result.success)
        {
            debug_printf("[VMM] WARNING: Failed to map CPU caps page at 0x%lx: %s\n",
                         CPU_CAPS_PAGE_ADDR, map_result.error_msg);
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

    *cabin_info_phys = (uint64_t)info_phys;
    *pocket_ring_phys = (uint64_t)pocket_phys;
    *result_ring_phys = (uint64_t)result_phys;

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

void *vmm_translate_user_addr(vmm_context_t *ctx, uintptr_t user_vaddr, size_t size)
{
    if (!ctx || size == 0)
        return NULL;

    // Validate that the entire range falls within a single page
    // (cross-page translations require per-page walks)
    uintptr_t page_start = user_vaddr & VMM_PAGE_MASK;
    uintptr_t end_addr = user_vaddr + size - 1;
    uintptr_t page_end = end_addr & VMM_PAGE_MASK;

    if (page_start != page_end)
    {
        // Range crosses page boundary — only translate first page's portion.
        // Caller must handle multi-page data in chunks.
        size = VMM_PAGE_SIZE - (user_vaddr & VMM_PAGE_OFFSET_MASK);
    }

    // Check that the page is user-accessible (not just present)
    pte_t *pte = vmm_get_pte(ctx, user_vaddr);
    if (!pte || !(*pte & VMM_FLAG_PRESENT) || !(*pte & VMM_FLAG_USER))
    {
        return NULL;
    }

    uintptr_t phys = vmm_pte_to_phys(*pte);
    uintptr_t offset = user_vaddr & VMM_PAGE_OFFSET_MASK;

    return vmm_phys_to_virt(phys + offset);
}

int vmm_setup_null_trap(vmm_context_t *ctx)
{
    if (!ctx)
        return -1;
    // 0x0000-0x0FFF is intentionally left unmapped; any access raises a page fault
    return 0;
}

/*
 * vmm_user_buf_in / vmm_user_buf_alloc_out / vmm_user_buf_commit_out / vmm_user_buf_free
 *
 * The single-page guarantee of vmm_translate_user_addr is correct (its
 * kernel pointer covers exactly one phys page). For multi-page user
 * buffers we need to walk the user PT page-by-page and either copy into
 * a freshly-kmalloc'd kernel buffer (for input crates) or copy out from
 * one (for output crates).
 *
 * Page-by-page walk handles non-contiguous physical pages, partial first
 * page (offset != 0), and partial last page (size not a page multiple).
 */

void *vmm_user_buf_in(vmm_context_t *ctx, uintptr_t user_vaddr, size_t size)
{
    if (!ctx || size == 0) return NULL;

    void *kbuf = kmalloc(size);
    if (!kbuf) return NULL;

    size_t copied = 0;
    while (copied < size) {
        uintptr_t off_in_page = (user_vaddr + copied) & VMM_PAGE_OFFSET_MASK;
        size_t this_page = VMM_PAGE_SIZE - off_in_page;
        if (this_page > size - copied) this_page = size - copied;

        void *src = vmm_translate_user_addr(ctx, user_vaddr + copied, this_page);
        if (!src) {
            kfree(kbuf);
            return NULL;
        }
        memcpy((uint8_t *)kbuf + copied, src, this_page);
        copied += this_page;
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

int vmm_user_buf_commit_out(vmm_context_t *ctx, uintptr_t user_vaddr,
                             const void *kbuf, size_t size)
{
    if (!ctx || !kbuf || size == 0) return -1;

    size_t copied = 0;
    while (copied < size) {
        uintptr_t off_in_page = (user_vaddr + copied) & VMM_PAGE_OFFSET_MASK;
        size_t this_page = VMM_PAGE_SIZE - off_in_page;
        if (this_page > size - copied) this_page = size - copied;

        void *dst = vmm_translate_user_addr(ctx, user_vaddr + copied, this_page);
        if (!dst) return -1;
        memcpy(dst, (const uint8_t *)kbuf + copied, this_page);
        copied += this_page;
    }
    return 0;
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
        ctx = current->cabin;
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
    }

    // User heap demand paging
    if (ctx && user && !present)
    {
        uintptr_t heap_max = ctx->heap_start + CONFIG_USER_HEAP_MAX_SIZE;
        if (fault_addr >= ctx->heap_start && fault_addr < heap_max)
        {
            uintptr_t page_addr = fault_addr & ~(VMM_PAGE_SIZE - 1);

            if (vmm_is_mapped(ctx, page_addr))
            {
                return 0;
            }

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
                debug_printf("[VMM] ERROR: Failed to map user heap page at 0x%lx\n", page_addr);
                pmm_free(phys, 1);
                return -1;
            }

            if (page_addr + VMM_PAGE_SIZE > ctx->heap_end)
            {
                ctx->heap_end = page_addr + VMM_PAGE_SIZE;
            }

            debug_printf("[VMM] Demand paging: mapped user heap page at 0x%lx\n", page_addr);
            return 0;
        }
    }

    // Phase 11: PocketRing/ResultRing slot regions — lazy first-touch mapping.
    // The producer (userspace for PocketRing, kernel for ResultRing) walks
    // through 1 MiB of reserved virtual space; pages are allocated on the
    // fly. Once mapped, a slot page stays mapped and is reused via the
    // monotonic-index modulo wrap.
    if (ctx && !present &&
        ((fault_addr >= CABIN_POCKET_SLOTS_BASE && fault_addr < CABIN_POCKET_SLOTS_END) ||
         (fault_addr >= CABIN_RESULT_SLOTS_BASE && fault_addr < CABIN_RESULT_SLOTS_END)))
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

    // Restore identity mapping for low-memory faults (only when Pull Map is not active)
    if (!g_pull_map_active && page_addr < (256ULL * 1024 * 1024))
    {
        debug_printf("[VMM] WARNING: Restoring identity mapping for 0x%llx\n", page_addr);
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        vmm_map_result_t result = vmm_map_page(kernel_context, page_addr, page_addr, flags);
        if (!result.success)
        {
            debug_printf("[VMM] FATAL: Failed to restore identity mapping at 0x%llx\n", page_addr);
            return -1;
        }
        return 0;
    }

    debug_printf("[VMM] ERROR: Fault address not in valid range (0x%llx)\n", fault_addr);
    return -1;
}
