#include "pmm.h"
#include "acpi.h"
#include "touch.h"
#include "buddy.h"
#include "pmtag.h"
#include "vmm.h"
#include "memtag.h"
#include "e820.h"
#include "klib.h"
#include "boxos_memory.h"
#include "cpuid.h"
#include "boot_info.h"
#include "linker_symbols.h"
#include "error.h"

static BuddyZone pmm_buddy;
static bool pmm_initialized = false;

static uint8_t pmm_maxphyaddr = 0;
static uint64_t pmm_max_phys_addr = 0;

static uint64_t pmm_mem_end = 0;

/*
 * Pristine free count just after E820 USABLE entries were inserted into the
 * buddy zone, before any boot-time reserves were applied.  This is the actual
 * installed RAM (in pages) — distinct from `pmm_buddy.total_pages` which is
 * (mem_end - zone_base) / PAGE_SIZE and includes PCI MMIO holes, ACPI/UEFI
 * runtime regions, and any other gaps between usable E820 chunks.  Reporting
 * `total - free` against the SPAN inflates "used" by every gigabyte of hole
 * the kernel never owned in the first place.
 */
static size_t pmm_usable_pages = 0;

typedef struct {
    uintptr_t start;
    uintptr_t end;
} DeferredRegion;

// Allocated from bootstrap physical memory in pmm_init(), sized to E820 entry count.
// No slots are ever dropped — capacity = total E820 entries.
static DeferredRegion *pmm_deferred     = NULL;
static size_t          pmm_deferred_cap = 0;
static size_t          pmm_deferred_count = 0;

static error_t pmm_init_maxphyaddr(void) {
    uint8_t maxphyaddr = cpuid_get_maxphyaddr();
    if (maxphyaddr < 32 || maxphyaddr > 52) {
        debug_printf("[PMM] Invalid MAXPHYADDR from CPUID: %u\n", maxphyaddr);
        return ERR_CPU_ERROR;
    }
    
    pmm_maxphyaddr = maxphyaddr;
    pmm_max_phys_addr = (1ULL << pmm_maxphyaddr);

    debug_printf("[PMM] MAXPHYADDR: %u bits (max 0x%llx)\n",
           pmm_maxphyaddr, pmm_max_phys_addr - 1);
    return OK;
}

error_t pmm_set_maxphyaddr(uint8_t maxphyaddr) {
    if (maxphyaddr < 32 || maxphyaddr > 52) {
        return ERR_INVALID_ARGUMENT;
    }

    pmm_maxphyaddr = maxphyaddr;
    pmm_max_phys_addr = (1ULL << maxphyaddr);

    debug_printf("[PMM] MAXPHYADDR set to %u bits\n", pmm_maxphyaddr);
    return OK;
}

uint8_t pmm_get_maxphyaddr(void) {
    return pmm_maxphyaddr;
}

uint64_t pmm_get_mem_end(void) {
    return pmm_mem_end;
}

/* Add `[seed_start, seed_end)` to the buddy zone, carving out every
 * sub-range that overlaps a non-USABLE E820 entry. Real BIOSes can
 * publish overlapping descriptors (USABLE that crosses an ACPI NVS
 * region; USABLE that brushes the MCFG ECAM hole reported as RESERVED).
 * Letting the buddy receive such overlap = handing the allocator pages
 * firmware actively owns. BIOS_BOOT_SPEC §15.3.
 *
 * Iterative sweep: maintain a `[cur, seed_end)` cursor; on each step
 * find the lowest non-USABLE entry that overlaps the cursor, emit the
 * gap before it (if any), then jump the cursor past its end. */
static void pmm_buddy_free_carved(BuddyZone *zone,
                                  uintptr_t seed_start, uintptr_t seed_end,
                                  const e820_entry_t *entries, size_t count)
{
    uintptr_t cur = seed_start;
    while (cur < seed_end) {
        uintptr_t hit_start = 0, hit_end = 0;
        bool       have_hit = false;
        for (size_t i = 0; i < count; i++) {
            if (entries[i].type == E820_USABLE || entries[i].length == 0) continue;
            uintptr_t rs = entries[i].base;
            uintptr_t re = entries[i].base + entries[i].length;
            if (re <= cur || rs >= seed_end) continue;
            if (!have_hit || rs < hit_start) {
                hit_start = rs;
                hit_end   = re;
                have_hit  = true;
            }
        }
        if (!have_hit) {
            buddy_free_range(zone, cur, seed_end);
            return;
        }
        if (hit_start > cur) buddy_free_range(zone, cur, hit_start);
        if (hit_end >= seed_end) return;
        cur = hit_end;
    }
}

static error_t pmm_defer_region(uintptr_t start, uintptr_t end) {
    if (!pmm_deferred || pmm_deferred_count >= pmm_deferred_cap) {
        // Should never happen: capacity set to entry_count in pmm_init().
        debug_printf("[PMM] BUG: deferred table overflow at 0x%lx-0x%lx\n", start, end);
        return ERR_NO_MEMORY;
    }

    pmm_deferred[pmm_deferred_count].start = start;
    pmm_deferred[pmm_deferred_count].end   = end;
    pmm_deferred_count++;
    return OK;
}

error_t pmm_init(void) {
    if (pmm_initialized) {
        return ERR_ALREADY_INITIALIZED;
    }

    error_t err = pmm_init_maxphyaddr();
    if (err != OK) {
        return err;
    }

    e820_entry_t* entries = memory_map_get_entries();
    size_t entry_count = memory_map_get_entry_count();

    if (entry_count == 0) {
        return ERR_E820_FAILED;
    }

    uintptr_t mem_end = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].type == E820_USABLE && entries[i].length > 0) {
            uintptr_t region_end = entries[i].base + entries[i].length;
            if (region_end > mem_end) {
                mem_end = region_end;
            }
        }
    }

    if (mem_end > pmm_max_phys_addr) {
        mem_end = pmm_max_phys_addr;
    }

    pmm_mem_end = mem_end;

    if (mem_end <= LOW_MEMORY_END) {
        panic("[PMM] Not enough usable RAM!");
    }

    boot_info_t *bi = boot_info_get();
    bool bi_ok = boot_info_valid(bi);
    uintptr_t map_start = bi_ok
        ? ALIGN_UP((uintptr_t)bi->stack_base, PMM_PAGE_SIZE)
        : ALIGN_UP((uintptr_t)&_kernel_phys_end, PMM_PAGE_SIZE);

    uint8_t* alloc_map = (uint8_t*)map_start;
    uintptr_t alloc_map_phys = map_start;
    size_t temp_pages = (mem_end - map_start) / PMM_PAGE_SIZE;
    size_t alloc_map_size = (temp_pages + 7) / 8;

    // Allocate the deferred region array from bootstrap physical memory,
    // immediately after the buddy bitmap. Sized to entry_count so that
    // every E820 high-memory region is recorded — no silent drops.
    uintptr_t deferred_base = ALIGN_UP(map_start + alloc_map_size, 8);
    size_t    deferred_size = entry_count * sizeof(DeferredRegion);
    pmm_deferred     = (DeferredRegion *)deferred_base;
    pmm_deferred_cap = entry_count;
    pmm_deferred_count = 0;

    uintptr_t zone_base = ALIGN_UP(deferred_base + deferred_size, PMM_PAGE_SIZE);

    if (zone_base >= mem_end) {
        panic("[PMM] Kernel too large for available memory!");
    }

    size_t total_pages = (mem_end - zone_base) / PMM_PAGE_SIZE;

    if (total_pages == 0) {
        panic("[PMM] No usable pages!");
    }

    buddy_init(&pmm_buddy, zone_base, total_pages,
               alloc_map, alloc_map_phys, alloc_map_size);

    uintptr_t buddy_zone_end = zone_base + total_pages * PMM_PAGE_SIZE;
    if (buddy_zone_end > pmm_max_phys_addr) {
        panic("[PMM] ASSERT FAILED: buddy zone end 0x%lx exceeds MAXPHYADDR 0x%llx — "
              "memory detection is inconsistent",
              buddy_zone_end, pmm_max_phys_addr);
    }

    #define IDENTITY_MAP_LIMIT 0x100000000ULL
    size_t unusable_pages = 0;

    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].type == E820_USABLE && entries[i].length > 0) {
            uintptr_t start = entries[i].base;
            uintptr_t end = entries[i].base + entries[i].length;

            if (start >= pmm_max_phys_addr) {
                unusable_pages += (end - start) / PMM_PAGE_SIZE;
                continue;
            }

            if (end > pmm_max_phys_addr) {
                unusable_pages += (end - pmm_max_phys_addr) / PMM_PAGE_SIZE;
                end = pmm_max_phys_addr;
            }

            if (start >= IDENTITY_MAP_LIMIT || end > IDENTITY_MAP_LIMIT) {
                uintptr_t hi_start = (start >= IDENTITY_MAP_LIMIT) ? start : IDENTITY_MAP_LIMIT;
                uintptr_t hi_end = end;
                if (hi_start < hi_end) {
                    pmm_defer_region(hi_start, hi_end);
                }
                if (start >= IDENTITY_MAP_LIMIT) continue;
                end = IDENTITY_MAP_LIMIT;
            }

            if (end <= zone_base) continue;
            if (start < zone_base) start = zone_base;
            if (start >= end) continue;

            /* Carve out any overlap with non-USABLE entries before
             * inserting into the buddy. Protects against BIOSes that
             * publish USABLE brushing a RESERVED region (MCFG ECAM
             * hole, SMRAM leak, ACPI NVS inside USABLE). */
            pmm_buddy_free_carved(&pmm_buddy, start, end,
                                  entries, entry_count);
        }
    }

    if (unusable_pages > 0) {
        debug_printf("[PMM] %zu pages beyond MAXPHYADDR unusable\n", unusable_pages);
    }

    /* Count actual installed RAM by summing every USABLE E820 entry, capped
     * at MAXPHYADDR.  This INCLUDES high memory that's currently deferred
     * (will be promoted in pmm_activate_pull_map) and INCLUDES the area
     * below zone_base where the kernel image / alloc_map / deferred table
     * live — those pages are physically present even though they're never
     * inserted into the buddy.  Read against pmm_buddy.free_count, this
     * gives the correct (usable - free = used = reserves + allocations)
     * relation throughout the boot. */
    {
        size_t usable_total = 0;
        for (size_t i = 0; i < entry_count; i++) {
            if (entries[i].type != E820_USABLE || entries[i].length == 0) continue;
            uintptr_t s = entries[i].base;
            uintptr_t e = entries[i].base + entries[i].length;
            if (s >= pmm_max_phys_addr) continue;
            if (e > pmm_max_phys_addr) e = pmm_max_phys_addr;
            if (e > s) usable_total += (e - s) / PMM_PAGE_SIZE;
        }
        pmm_usable_pages = usable_total;
    }

    if (bi_ok) {
        buddy_reserve_range(&pmm_buddy, (uintptr_t)bi->kernel_start, (uintptr_t)bi->kernel_end);
    } else {
        buddy_reserve_range(&pmm_buddy, CONFIG_KERNEL_PHYS_ADDR,
                            ALIGN_UP((uintptr_t)&_kernel_phys_end, PMM_PAGE_SIZE));
    }
    buddy_reserve_range(&pmm_buddy, alloc_map_phys, alloc_map_phys + alloc_map_size);
    buddy_reserve_range(&pmm_buddy, deferred_base, deferred_base + deferred_size);

    pmm_initialized = true;

    debug_printf("[PMM] Initialized: %zu MB available\n",
        (pmm_buddy.free_count * PMM_PAGE_SIZE) / (1024 * 1024));
    return OK;
}

uint32_t pmm_phys_domain(uintptr_t phys) {
    return acpi_numa_domain_for_phys((uint64_t)phys);
}

size_t pmm_pages_in_domain(uint32_t domain) {
    const acpi_numa_info_t* n = acpi_get_numa();
    if (!n) return 0;
    uint64_t total = 0;
    for (uint8_t i = 0; i < n->mem_count; i++) {
        if (!(n->mem[i].flags & 0x1)) continue;          /* not enabled */
        if (n->mem[i].domain != domain) continue;
        total += n->mem[i].length;
    }
    return (size_t)(total / PMM_PAGE_SIZE);
}

void* pmm_alloc_in_domain(size_t pages, uint32_t domain) {
    if (!pmm_initialized || !pages) return NULL;
    const acpi_numa_info_t* n = acpi_get_numa();
    if (domain == ACPI_NUMA_DOMAIN_UNKNOWN || !n) {
        return buddy_alloc(&pmm_buddy, pages);
    }

    /* Range-constrained allocation: walk every SRAT memory range that
     * belongs to `domain` and ask buddy_alloc_range for pages within
     * its [base, base+length). First range that satisfies wins. This
     * is O(domain_ranges) and never causes buddy churn (unlike the
     * sample-retry approach we used before). */
    for (uint8_t i = 0; i < n->mem_count; i++) {
        if (!(n->mem[i].flags & 0x1)) continue;     /* not enabled */
        if (n->mem[i].domain != domain) continue;
        uintptr_t lo = (uintptr_t)n->mem[i].base;
        uintptr_t hi = (uintptr_t)(n->mem[i].base + n->mem[i].length);
        void* p = buddy_alloc_range(&pmm_buddy, pages, lo, hi);
        if (p) return p;
    }

    /* No domain-local memory free at this size — fall back. */
    return buddy_alloc(&pmm_buddy, pages);
}

/* Aggregate the SRAT enabled memory ranges into one log line per domain
 * with total bytes per domain. Read-only — no allocator decisions taken
 * here; the future NUMA-aware buddy partition consumes this same data
 * directly via acpi_get_numa(). */
void pmm_log_numa_topology(void) {
    const acpi_numa_info_t* n = acpi_get_numa();
    if (!n) {
        debug_printf("[PMM] NUMA: no SRAT — uniform memory\n");
        return;
    }
    debug_printf("[PMM] NUMA: %u domain(s), %u CPU(s), %u memory range(s)\n",
                 n->domain_count, n->cpu_count, n->mem_count);
    /* Topology summary on `numa:topology` so scheduler/affinity
     * daemons can plan placement without re-walking SRAT. */
    struct { uint8_t domains; uint16_t cpus; uint8_t mem_ranges; } topo =
        { n->domain_count, n->cpu_count, n->mem_count };
    TouchPublish("numa:topology", &topo, sizeof(topo));
    for (uint8_t d = 0; d < n->domain_count; d++) {
        uint32_t dom = n->domains[d];
        uint64_t total = 0;
        uint8_t  ranges = 0;
        uint8_t  cpus = 0;
        for (uint8_t i = 0; i < n->mem_count; i++) {
            if (n->mem[i].domain == dom &&
                (n->mem[i].flags & 0x1)) {
                total += n->mem[i].length;
                ranges++;
            }
        }
        for (uint8_t i = 0; i < n->cpu_count; i++) {
            if (n->cpus[i].enabled && n->cpus[i].domain == dom) cpus++;
        }
        debug_printf("[PMM]   domain %u: %lu MB across %u range(s), %u CPU(s)\n",
                     dom, (unsigned long)(total >> 20), ranges, cpus);
    }
}

// ─── Core allocator ──────────────────────────────────────────────────────────
// Called via the pmm_alloc(pages[, tag]) macro defined in pmm.h.
//
// tags == 0                  → any zone, first-fit from buddy
// tags == PHYS_TAG_DMA32     → [0,  DMA32_END)  O(log max_order)
// tags == PHYS_TAG_USER      → [DMA32_END, 4GB) O(log max_order)
// tags == PHYS_TAG_HIGH      → [4GB, mem_end)   O(log max_order)
// tags == anything else      → PhysAllocTagged  O(band scan)
// ─────────────────────────────────────────────────────────────────────────────
void* _pmm_alloc_impl(size_t pages, uint64_t tags) {
    if (!pages || !pmm_initialized) return NULL;

    void* addr;

    if (tags == 0) {
        addr = buddy_alloc(&pmm_buddy, pages);
    } else if (tags == PHYS_TAG_DMA32) {
        addr = buddy_alloc_range(&pmm_buddy, pages,
                                 0,
                                 (uintptr_t)CONFIG_PHYS_ZONE_DMA32_END);
    } else if (tags == PHYS_TAG_USER) {
        addr = buddy_alloc_range(&pmm_buddy, pages,
                                 (uintptr_t)CONFIG_PHYS_ZONE_DMA32_END,
                                 (uintptr_t)CONFIG_PHYS_ZONE_USER_END);
    } else if (tags == PHYS_TAG_HIGH) {
        addr = buddy_alloc_range(&pmm_buddy, pages,
                                 (uintptr_t)CONFIG_PHYS_ZONE_USER_END,
                                 (uintptr_t)pmm_mem_end);
    } else {
        // Dynamic / semantic tags (SHARED, KERNEL, MMIO, user-defined):
        // PhysAllocTagged scans PMTAG band index to find a matching span,
        // then calls buddy_alloc_range internally.
        // MAXPHYADDR check is inside PhysAllocTagged → return directly.
        return PhysAllocTagged(pages, tags);
    }

    if (!addr) {
        /* Diagnostic: any pmm_alloc failure is a memory pressure signal.
         * Rate-limited so a stuck-allocator loop doesn't flood serial. */
        static volatile uint64_t g_pmm_fail = 0;
        uint64_t cnt = __atomic_add_fetch(&g_pmm_fail, 1, __ATOMIC_RELAXED);
        if (cnt == 1 || cnt == 10 || (cnt % 100) == 0) {
            kprintf("[TRC] PMM_FAIL pages=%lu tags=0x%lx free=%zu cnt=%lu\n",
                    (unsigned long)pages, (unsigned long)tags,
                    pmm_buddy.free_count, cnt);
        }
        return NULL;
    }

    uintptr_t phys_end = (uintptr_t)addr + pages * PMM_PAGE_SIZE - 1;
    if (phys_end >= pmm_max_phys_addr) {
        debug_printf("[PMM] WARNING: alloc 0x%lx exceeds MAXPHYADDR — zone boundary misconfigured\n",
                     (uintptr_t)addr);
        buddy_free(&pmm_buddy, addr, pages);
        return NULL;
    }

    return addr;
}

void* _pmm_alloc_zero_impl(size_t pages, uint64_t tags) {
    void* addr = _pmm_alloc_impl(pages, tags);
    if (addr) {
        void* virt = vmm_phys_to_virt((uintptr_t)addr);
        memset(virt, 0, pages * PMM_PAGE_SIZE);
    }
    return addr;
}

void pmm_free(void* addr, size_t pages) {
    if (!addr || !pages || !pmm_initialized) return;

    uintptr_t base = (uintptr_t)addr;
    uintptr_t zone_end = pmm_buddy.base + pmm_buddy.total_pages * PMM_PAGE_SIZE;
    if (base < pmm_buddy.base || base >= zone_end) {
        panic("PMM: Invalid free address %p", addr);
    }

    buddy_free(&pmm_buddy, addr, pages);
    MemTagRemoveRegion(base);
}

size_t pmm_total_pages(void) {
    /* Installed RAM in pages — count of E820 USABLE entries inserted into
     * the buddy at init.  NOT (mem_end - zone_base)/PAGE_SIZE: that span
     * includes MMIO holes the kernel never owns. */
    return pmm_usable_pages;
}

size_t pmm_free_pages(void) {
    spin_lock(&pmm_buddy.lock);
    size_t count = pmm_buddy.free_count;
    spin_unlock(&pmm_buddy.lock);
    return count;
}

size_t pmm_used_pages(void) {
    size_t free = pmm_free_pages();
    return pmm_usable_pages > free ? pmm_usable_pages - free : 0;
}

uint64_t pmm_get_total_memory(void) {
    /* Physical TOP of the buddy zone (one-past-last byte).  Used by code
     * that needs the addressable span (e.g. identity-map sizing); for
     * RAM-quantity reporting use pmm_get_total_ram_bytes(). */
    return pmm_buddy.base + (pmm_buddy.total_pages * PMM_PAGE_SIZE);
}

uint64_t pmm_get_total_ram_bytes(void) {
    return (uint64_t)pmm_usable_pages * PMM_PAGE_SIZE;
}

void pmm_dump_stats(void) {
    kprintf("Physical Memory Manager (Buddy Allocator):\n");
    kprintf("  Total pages: %d (%d MB)\n",
           pmm_total_pages(),
           (pmm_total_pages() * PMM_PAGE_SIZE) / (1024 * 1024));
    kprintf("  Used pages:  %d (%d MB)\n",
           pmm_used_pages(),
           (pmm_used_pages() * PMM_PAGE_SIZE) / (1024 * 1024));
    kprintf("  Free pages:  %d (%d MB)\n",
           pmm_free_pages(),
           (pmm_free_pages() * PMM_PAGE_SIZE) / (1024 * 1024));
    kprintf("  Max order:   %d (%d KB max block)\n",
           BUDDY_MAX_ORDER, (1 << BUDDY_MAX_ORDER) * 4);
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
        if (pmm_buddy.free_lists[o].count > 0) {
            kprintf("  Order %2d (%6dKB): %d blocks\n",
                   o, (1 << o) * 4, (int)pmm_buddy.free_lists[o].count);
        }
    }
}

void pmm_print_memory_map(void) {
    e820_entry_t* entries = memory_map_get_entries();
    size_t entry_count = memory_map_get_entry_count();

    kprintf("Memory Map:\n");
    for (size_t i = 0; i < entry_count; i++) {
        kprintf("  %p-%p: %s\n",
               (void*)entries[i].base,
               (void*)(entries[i].base + entries[i].length),
               entries[i].type == E820_USABLE ? "Usable" : "Reserved");
    }
}

void pmm_activate_pull_map(void) {
    buddy_activate_pull_map(&pmm_buddy);
    debug_printf("[PMM] Buddy allocator rebased to Pull Map\n");

    // Rebase pmm_deferred: it was set to a physical address in pmm_init()
    // and is never valid as a virtual address after pull map activation.
    if (pmm_deferred) {
        pmm_deferred = (DeferredRegion*)vmm_phys_to_virt((uintptr_t)pmm_deferred);
    }

    /* Phase 2: free deferred high-memory regions (saved during Phase 1)
     * with the same overlap carving Phase 1 uses (DIMMs above 4 GiB can
     * collide with GPU stolen aperture / hot-plug-reserve slots that
     * firmware leaves RESERVED).
     *
     * E820 pointer resolution. pmm_activate_pull_map runs AFTER the Pull
     * Map is live but typically BEFORE e820_activate_pull_map rebases
     * the e820_entries pointer — so memory_map_get_entries() still
     * returns the identity address (0x504), which is no longer mapped.
     * Resolving it via vmm_virt_to_phys_direct → vmm_phys_to_virt is
     * idempotent: identity stays identity-then-PullMap'd to the live
     * kernel address, and a future call after e820_activate_pull_map
     * (where the returned pointer is already a Pull-Map address) decodes
     * back to phys and re-maps to the same Pull-Map address. Survives
     * any future re-ordering of vmm_init's tail. */
    const e820_entry_t *entries_raw       = memory_map_get_entries();
    uintptr_t          entries_phys       = vmm_virt_to_phys_direct((void *)entries_raw);
    const e820_entry_t *entries_for_carve = (const e820_entry_t *)vmm_phys_to_virt(entries_phys);
    size_t              entries_for_carve_count = memory_map_get_entry_count();
    for (size_t i = 0; i < pmm_deferred_count; i++) {
        uintptr_t start = pmm_deferred[i].start;
        uintptr_t end = pmm_deferred[i].end;

        debug_printf("[PMM] Phase 2: freeing high memory 0x%lx-0x%lx (%zu pages)\n",
                     start, end, (end - start) / PMM_PAGE_SIZE);
        pmm_buddy_free_carved(&pmm_buddy, start, end,
                              entries_for_carve, entries_for_carve_count);
    }

    debug_printf("[PMM] Total memory available: %zu MB (%zu pages)\n",
                 (pmm_buddy.free_count * PMM_PAGE_SIZE) / (1024 * 1024),
                 pmm_buddy.free_count);

    if (g_pmtag.initialized) {
        for (size_t i = 0; i < pmm_deferred_count; i++) {
            PhysTagSet(pmm_deferred[i].start, pmm_deferred[i].end, PHYS_TAG_HIGH);
        }
    }
}

BuddyZone* pmm_get_buddy_zone(void) {
    return &pmm_buddy;
}

bool pmm_is_usable_ram(uintptr_t phys_addr, size_t size) {
    if (size == 0) {
        return false;
    }

    uintptr_t range_end = phys_addr + size;

    if (range_end < phys_addr) {
        debug_printf("[PMM] pmm_is_usable_ram: overflow detected for phys=0x%llx size=0x%llx\n",
                     (uint64_t)phys_addr, (uint64_t)size);
        return false;
    }

    e820_entry_t* entries = memory_map_get_entries();
    size_t count = memory_map_get_entry_count();

    for (size_t i = 0; i < count; i++) {
        if (entries[i].type != E820_USABLE) {
            continue;
        }

        uintptr_t e820_start = entries[i].base;
        uintptr_t e820_end = entries[i].base + entries[i].length;

        if (phys_addr < e820_end && e820_start < range_end) {
            debug_printf("[PMM] pmm_is_usable_ram: phys=0x%llx size=0x%llx overlaps USABLE [0x%llx-0x%llx)\n",
                         (uint64_t)phys_addr, (uint64_t)size,
                         (uint64_t)e820_start, (uint64_t)e820_end);
            return true;
        }
    }

    return false;
}

void pmm_test_high_memory(void) {
    if (pmm_deferred_count == 0) {
        kprintf("[PMM TEST] No high memory regions — skipping (RAM <= 4GB)\n");
        return;
    }

    kprintf("[PMM TEST] Testing >4GB allocations...\n");

    size_t pass = 0, fail = 0;

    /* Test 1: Single-page allocations from the >4GB band.
     * Use PHYS_TAG_HIGH to deterministically request high memory via the
     * range path — the untagged buddy_alloc() is first-fit and on a
     * fresh boot the order-0 free list is dominated by sub-4GB pages
     * left over from kernel init, so an untagged loop never touches the
     * high zone (root cause of the false "0 from >4GB" report). */
    #define HIGH_TEST_COUNT 16
    void* allocs[HIGH_TEST_COUNT];
    size_t high_count = 0;

    for (size_t i = 0; i < HIGH_TEST_COUNT && high_count < HIGH_TEST_COUNT; i++) {
        void* phys = pmm_alloc(1, PHYS_TAG_HIGH);
        if (!phys) break;
        if ((uintptr_t)phys < IDENTITY_MAP_LIMIT) {
            /* Should never happen — PHYS_TAG_HIGH is range-bounded. */
            pmm_free(phys, 1);
            continue;
        }
        allocs[high_count++] = phys;
    }

    kprintf("[PMM TEST] Single pages: %zu from >4GB\n", high_count);

    for (size_t i = 0; i < high_count; i++) {
        uintptr_t phys = (uintptr_t)allocs[i];
        volatile uint64_t* virt = (volatile uint64_t*)vmm_phys_to_virt(phys);

        uint64_t pattern = phys ^ 0xB0A0DEADBEEFCAFEULL;
        virt[0] = pattern;
        virt[1] = ~pattern;
        virt[255] = pattern;  // near end of 4KB page

        if (virt[0] == pattern && virt[1] == ~pattern && virt[255] == pattern) {
            pass++;
        } else {
            kprintf("[PMM TEST]   FAIL: single page phys=0x%lx\n", phys);
            fail++;
        }
        pmm_free(allocs[i], 1);
    }

    // Test 2: Large block allocation (256 pages = 1MB) — likely from >4GB contiguous pool
    size_t large_sizes[] = {256, 64, 16};
    for (size_t s = 0; s < 3; s++) {
        size_t pages = large_sizes[s];
        void* phys = pmm_alloc(pages);
        if (!phys) continue;

        uintptr_t addr = (uintptr_t)phys;
        bool is_high = (addr >= IDENTITY_MAP_LIMIT);

        kprintf("[PMM TEST] Large block: %zu pages at 0x%lx %s\n",
                pages, addr, is_high ? "(>4GB)" : "(<4GB)");

        if (is_high) {
            // Verify first, middle, and last page of the block
            size_t test_offsets[] = {0, pages / 2, pages - 1};
            for (size_t t = 0; t < 3; t++) {
                uintptr_t page_phys = addr + test_offsets[t] * PMM_PAGE_SIZE;
                volatile uint64_t* virt = (volatile uint64_t*)vmm_phys_to_virt(page_phys);

                uint64_t pattern = page_phys ^ 0xCAFEBABE12345678ULL;
                virt[0] = pattern;
                virt[511] = ~pattern;  // last uint64_t in page

                if (virt[0] == pattern && virt[511] == ~pattern) {
                    pass++;
                } else {
                    kprintf("[PMM TEST]   FAIL: large block page 0x%lx\n", page_phys);
                    fail++;
                }
            }
        }

        pmm_free(phys, pages);
    }

    // Test 3: pmm_alloc_zero from >4GB — verify zeroed
    void* zero_phys = NULL;
    for (size_t i = 0; i < 128; i++) {
        void* p = pmm_alloc_zero(1);
        if (!p) break;
        if ((uintptr_t)p >= IDENTITY_MAP_LIMIT) {
            zero_phys = p;
            break;
        }
        pmm_free(p, 1);
    }

    if (zero_phys) {
        volatile uint64_t* virt = (volatile uint64_t*)vmm_phys_to_virt((uintptr_t)zero_phys);
        bool zeroed = true;
        for (size_t i = 0; i < 512 && zeroed; i++) {
            if (virt[i] != 0) zeroed = false;
        }
        if (zeroed) {
            pass++;
            kprintf("[PMM TEST] alloc_zero at 0x%lx: zeroed OK\n", (uintptr_t)zero_phys);
        } else {
            fail++;
            kprintf("[PMM TEST] FAIL: alloc_zero at 0x%lx NOT zeroed\n", (uintptr_t)zero_phys);
        }
        pmm_free(zero_phys, 1);
    }

    if (fail == 0 && pass > 0) {
        kprintf("[PMM TEST] %[S]PASSED: all %zu checks OK (>4GB alloc + Pull Map access)%[D]\n", pass);
    } else if (pass == 0 && fail == 0) {
        kprintf("[PMM TEST] %[Y]SKIPPED: zero high-memory pages were exercised — "
                "high zone empty or first-fit returned only <4GB%[D]\n");
    } else {
        kprintf("[PMM TEST] %[R]FAILED: %zu pass, %zu fail%[D]\n", pass, fail);
    }
}
