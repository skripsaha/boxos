#include "pmm.h"
#include "buddy.h"
#include "vmm.h"
#include "e820.h"
#include "klib.h"
#include "boxos_memory.h"
#include "cpuid.h"
#include "boot_info.h"
#include "linker_symbols.h"

static BuddyZone pmm_buddy;
static bool pmm_initialized = false;

static uint8_t pmm_maxphyaddr = 0;
static uint64_t pmm_max_phys_addr = 0;

static uint64_t pmm_mem_end = 0;

// Deferred high-memory regions (above 4GB identity map limit).
// Saved during pmm_init Phase 1, freed in pmm_activate_pull_map Phase 2.
#define PMM_DEFERRED_MAX 8
typedef struct {
    uintptr_t start;
    uintptr_t end;
} DeferredRegion;
static DeferredRegion pmm_deferred[PMM_DEFERRED_MAX];
static size_t pmm_deferred_count = 0;

static void pmm_init_maxphyaddr(void) {
    pmm_maxphyaddr = cpuid_get_maxphyaddr();
    pmm_max_phys_addr = (1ULL << pmm_maxphyaddr);

    debug_printf("[PMM] MAXPHYADDR detection:\n");
    debug_printf("[PMM]   Physical address bits: %u\n", pmm_maxphyaddr);
    debug_printf("[PMM]   Max physical address: 0x%llx (%llu MB)\n",
           pmm_max_phys_addr - 1,
           pmm_max_phys_addr / (1024 * 1024));
}

void pmm_set_maxphyaddr(uint8_t maxphyaddr) {
    if (maxphyaddr < 32 || maxphyaddr > 52) {
        debug_printf("[PMM] Invalid MAXPHYADDR: %u (must be 32-52)\n", maxphyaddr);
        return;
    }

    pmm_maxphyaddr = maxphyaddr;
    pmm_max_phys_addr = (1ULL << maxphyaddr);

    debug_printf("[PMM] MAXPHYADDR overridden to %u bits (max 0x%llx)\n",
           pmm_maxphyaddr, pmm_max_phys_addr - 1);
}

uint8_t pmm_get_maxphyaddr(void) {
    return pmm_maxphyaddr;
}

uint64_t pmm_get_mem_end(void) {
    return pmm_mem_end;
}

void pmm_init(void) {
    if (pmm_initialized) {
        debug_printf("[PMM] Already initialized!\n");
        return;
    }

    pmm_init_maxphyaddr();

    debug_printf("[PMM] Fetching e820 memory map...\n");
    e820_entry_t* entries = memory_map_get_entries();
    size_t entry_count = memory_map_get_entry_count();
    debug_printf("[PMM] e820 entry count = %d\n", entry_count);

    if (entry_count == 0) {
        panic("[PMM] ERROR: e820 map is empty!");
    }

    for (size_t i = 0; i < entry_count; i++) {
        if(entries[i].type != 0){
            kprintf("  #%d: base=0x%p len=0x%p type=%d\n", i, (void*)entries[i].base, (void*)entries[i].length, entries[i].type);
        }
    }
    kprintf("  Other entries are ZERO.\n");
    kprintf("  %[W]Do not use type 2 entries. Only type 1 - only RAM.%[D]\n\n");

    uintptr_t mem_end = 0;
    for (size_t i = 0; i < entry_count; i++) {
        debug_printf("[PMM] Entry %zu: base=0x%llx len=0x%llx type=%u\n",
                i, entries[i].base, entries[i].length, entries[i].type);
        if (entries[i].type == E820_USABLE && entries[i].length > 0) {
            debug_printf("[PMM] --> USABLE!\n");
            uintptr_t region_end = entries[i].base + entries[i].length;
            if (region_end > mem_end) {
                mem_end = region_end;
                debug_printf("[PMM] --> New mem_end = 0x%llx\n", (unsigned long long)mem_end);
            }
        }
    }
    debug_printf("[PMM] mem_end = 0x%p\n", (void*)mem_end);

    debug_printf("[PMM] E820 Memory Map (%zu entries):\n", entry_count);
    for (size_t i = 0; i < entry_count; i++) {
        const char* type_str = "UNKNOWN";
        switch (entries[i].type) {
            case E820_USABLE:     type_str = "USABLE"; break;
            case E820_RESERVED:   type_str = "RESERVED"; break;
            case E820_ACPI_RECL:  type_str = "ACPI_RECL"; break;
            case E820_ACPI_NVS:   type_str = "ACPI_NVS"; break;
            case E820_BAD:        type_str = "BAD"; break;
        }
        debug_printf("[PMM]   [%zu] 0x%016llx-0x%016llx (%10llu bytes) %s\n",
                     i,
                     (uint64_t)entries[i].base,
                     (uint64_t)(entries[i].base + entries[i].length - 1),
                     (uint64_t)entries[i].length,
                     type_str);
    }

    if (mem_end > pmm_max_phys_addr) {
        debug_printf("[PMM] WARNING: e820 reports RAM up to 0x%lx, clamping to MAXPHYADDR 0x%llx\n",
               mem_end, pmm_max_phys_addr - 1);
        mem_end = pmm_max_phys_addr;
    }

    pmm_mem_end = mem_end;

    if (mem_end <= LOW_MEMORY_END) {
        panic("[PMM] ERROR: Not enough usable RAM!");
    }

    // Place alloc_map after all boot infrastructure (kernel + page tables + stack)
    boot_info_t *bi = boot_info_get();
    uintptr_t map_start;
    if (boot_info_valid(bi)) {
        map_start = ALIGN_UP((uintptr_t)bi->stack_base, 4096);
    } else {
        map_start = ALIGN_UP((uintptr_t)&_kernel_phys_end, 4096);
    }

    uint8_t* alloc_map = (uint8_t*)map_start;
    uintptr_t alloc_map_phys = map_start;
    size_t temp_pages = (mem_end - map_start) / PMM_PAGE_SIZE;
    size_t alloc_map_size = (temp_pages + 7) / 8;
    debug_printf("[PMM] Buddy alloc_map at %p (%zu bytes)\n", alloc_map, alloc_map_size);

    uintptr_t zone_base = ALIGN_UP(map_start + alloc_map_size, 4096);
    debug_printf("[PMM] Buddy zone base: 0x%lx\n", zone_base);

    if (zone_base >= mem_end) {
        panic("[PMM] FATAL: Kernel too large for available memory!");
    }

    size_t total_pages = (mem_end - zone_base) / PMM_PAGE_SIZE;
    debug_printf("[PMM] Buddy total pages: %zu\n", total_pages);

    if (total_pages == 0) {
        panic("[PMM] ERROR: No usable pages!");
    }

    // Initialize buddy allocator (marks all pages as allocated)
    buddy_init(&pmm_buddy, zone_base, total_pages,
               alloc_map, alloc_map_phys, alloc_map_size);

    // Phase 1: Free usable e820 regions below 4GB (identity-mapped, accessible now).
    // Regions above 4GB are deferred to pmm_activate_pull_map() when Pull Map is live.
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

            // Defer regions above identity map limit to phase 2
            if (start >= IDENTITY_MAP_LIMIT || end > IDENTITY_MAP_LIMIT) {
                uintptr_t hi_start = (start >= IDENTITY_MAP_LIMIT) ? start : IDENTITY_MAP_LIMIT;
                uintptr_t hi_end = end;
                if (hi_start < hi_end && pmm_deferred_count < PMM_DEFERRED_MAX) {
                    pmm_deferred[pmm_deferred_count].start = hi_start;
                    pmm_deferred[pmm_deferred_count].end = hi_end;
                    pmm_deferred_count++;
                    debug_printf("[PMM] Deferred high-memory region: 0x%lx-0x%lx\n", hi_start, hi_end);
                }
                if (start >= IDENTITY_MAP_LIMIT) continue;
                end = IDENTITY_MAP_LIMIT;
            }

            if (end <= zone_base) continue;
            if (start < zone_base) start = zone_base;
            if (start >= end) continue;

            debug_printf("[PMM] Buddy: freeing 0x%lx-0x%lx (%zu pages)\n",
                         start, end, (end - start) / PMM_PAGE_SIZE);
            buddy_free_range(&pmm_buddy, start, end);
        }
    }

    if (unusable_pages > 0) {
        debug_printf("[PMM] WARNING: %zu pages beyond MAXPHYADDR marked unusable\n", unusable_pages);
    }

    // Reserve kernel and alloc_map regions
    kprintf("PMM: Reserved Kernel at %p-%p\n", bi->kernel_start, bi->kernel_end);
    buddy_reserve_range(&pmm_buddy, (uintptr_t)bi->kernel_start, (uintptr_t)bi->kernel_end);
    kprintf("PMM: Reserved AllocMap at %p-%p\n", (void*)alloc_map_phys, (void*)(alloc_map_phys + alloc_map_size));
    buddy_reserve_range(&pmm_buddy, alloc_map_phys, alloc_map_phys + alloc_map_size);

    pmm_initialized = true;

    debug_printf("[PMM] Buddy allocator initialized: %zu MB available, %zu pages free\n",
        (pmm_buddy.free_count * PMM_PAGE_SIZE) / (1024 * 1024),
        pmm_buddy.free_count);

    // Log free list distribution
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
        if (pmm_buddy.free_lists[o].count > 0) {
            debug_printf("[PMM]   Order %2d (%6zuKB blocks): %zu free\n",
                         o, (1UL << o) * 4, pmm_buddy.free_lists[o].count);
        }
    }
}

void* pmm_alloc(size_t pages) {
    if (!pages || !pmm_initialized) {
        return NULL;
    }

    void* addr = buddy_alloc(&pmm_buddy, pages);
    if (!addr) return NULL;

    // Validate allocation doesn't exceed MAXPHYADDR
    uintptr_t phys_end = (uintptr_t)addr + (pages * PMM_PAGE_SIZE) - 1;
    if (phys_end >= pmm_max_phys_addr) {
        debug_printf("[PMM] ERROR: Allocation 0x%lx exceeds MAXPHYADDR, rolling back\n", (uintptr_t)addr);
        buddy_free(&pmm_buddy, addr, pages);
        return NULL;
    }

    return addr;
}

void* pmm_alloc_zero(size_t pages) {
    void* addr = pmm_alloc(pages);
    if (addr) {
        void* virt = vmm_phys_to_virt((uintptr_t)addr);
        memset(virt, 0, pages * PMM_PAGE_SIZE);
    }
    return addr;  // returns physical address
}

void pmm_free(void* addr, size_t pages) {
    if (!addr || !pages || !pmm_initialized) return;

    uintptr_t base = (uintptr_t)addr;
    uintptr_t zone_end = pmm_buddy.base + pmm_buddy.total_pages * PMM_PAGE_SIZE;
    if (base < pmm_buddy.base || base >= zone_end) {
        panic("PMM: Invalid free address %p", addr);
    }

    buddy_free(&pmm_buddy, addr, pages);
}

size_t pmm_total_pages(void) {
    return pmm_buddy.total_pages;
}

size_t pmm_free_pages(void) {
    spin_lock(&pmm_buddy.lock);
    size_t count = pmm_buddy.free_count;
    spin_unlock(&pmm_buddy.lock);
    return count;
}

size_t pmm_used_pages(void) {
    return pmm_total_pages() - pmm_free_pages();
}

uint64_t pmm_get_total_memory(void) {
    return pmm_buddy.base + (pmm_buddy.total_pages * PMM_PAGE_SIZE);
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

bool pmm_check_integrity(void) {
    boot_info_t *bi = boot_info_get();
    uintptr_t kernel_phys_end = boot_info_valid(bi) ? (uintptr_t)bi->kernel_end : 0;

    for (size_t i = 0; i < pmm_buddy.total_pages; i++) {
        uintptr_t addr = pmm_buddy.base + i * PMM_PAGE_SIZE;

        if (addr < kernel_phys_end) {
            // Kernel pages must be allocated (bit set in alloc_map)
            size_t byte = i / 8;
            size_t bit = i % 8;
            if (!(pmm_buddy.alloc_map[byte] & (1 << bit))) {
                return false;
            }
        }
    }
    return true;
}

void pmm_activate_pull_map(void) {
    buddy_activate_pull_map(&pmm_buddy);
    debug_printf("[PMM] Buddy allocator rebased to Pull Map\n");

    // Phase 2: Free deferred high-memory regions (saved during Phase 1).
    for (size_t i = 0; i < pmm_deferred_count; i++) {
        uintptr_t start = pmm_deferred[i].start;
        uintptr_t end = pmm_deferred[i].end;

        debug_printf("[PMM] Phase 2: freeing high memory 0x%lx-0x%lx (%zu pages)\n",
                     start, end, (end - start) / PMM_PAGE_SIZE);
        buddy_free_range(&pmm_buddy, start, end);
    }

    debug_printf("[PMM] Total memory available: %zu MB (%zu pages)\n",
                 (pmm_buddy.free_count * PMM_PAGE_SIZE) / (1024 * 1024),
                 pmm_buddy.free_count);
}

bool pmm_is_usable_ram(uintptr_t phys_addr, size_t size) {
    if (size == 0) {
        return false;
    }

    uintptr_t range_end = phys_addr + size;

    if (range_end < phys_addr) {
        debug_printf("[PMM] pmm_is_usable_ram: overflow detected for phys=0x%llx size=0x%llx\n",
                     (uint64_t)phys_addr, (uint64_t)size);
        return true;
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

    // Test 1: Single page allocations — keep trying until we get >4GB pages
    #define HIGH_TEST_COUNT 16
    void* allocs[HIGH_TEST_COUNT];
    size_t high_count = 0;

    for (size_t i = 0; i < HIGH_TEST_COUNT * 64 && high_count < HIGH_TEST_COUNT; i++) {
        void* phys = pmm_alloc(1);
        if (!phys) break;

        if ((uintptr_t)phys >= IDENTITY_MAP_LIMIT) {
            allocs[high_count++] = phys;
        } else {
            pmm_free(phys, 1);
        }
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

    if (fail == 0) {
        kprintf("[PMM TEST] %[S]PASSED: all %zu checks OK (>4GB alloc + Pull Map access)%[D]\n", pass);
    } else {
        kprintf("[PMM TEST] %[R]FAILED: %zu pass, %zu fail%[D]\n", pass, fail);
    }
}
