#include "pmm.h"
#include "e820.h"
#include "klib.h"
#include "boxos_memory.h"
#include "cpuid.h"
#include "boot_info.h"

typedef struct {
    uintptr_t base;
    size_t pages;
    uint8_t* bitmap;
    spinlock_t lock;
    size_t last_free;
    size_t free_count;  // maintained incrementally — avoids O(n) scan in pmm_free_pages()
} pmm_zone_t;

static pmm_zone_t pmm_zone;
static bool pmm_initialized = false;

static uint8_t pmm_maxphyaddr = 0;
static uint64_t pmm_max_phys_addr = 0;

static uint64_t pmm_mem_end = 0;

static void pmm_reserve_region(uintptr_t base, uintptr_t end, const char* name);
static void pmm_set_bit(size_t bit, pmm_frame_state_t state);
static pmm_frame_state_t pmm_get_bit(size_t bit);
static size_t pmm_find_free_sequence(size_t count);

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

    // detect MAXPHYADDR before filtering memory to avoid Reserved Bit Violation
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

    // Place bitmap after all boot infrastructure (kernel + page tables + stack)
    // boot_info->stack_base is the highest address used by the bootloader
    boot_info_t *bi = boot_info_get();
    uintptr_t bitmap_start;
    if (boot_info_valid(bi)) {
        bitmap_start = ALIGN_UP((uintptr_t)bi->stack_base, 4096);
    } else {
        // Fallback: place after kernel (legacy behavior)
        bitmap_start = ALIGN_UP((uintptr_t)&_kernel_end, 4096);
    }
    pmm_zone.bitmap = (uint8_t*)bitmap_start;
    debug_printf("[PMM] Bitmap placed at %p (after boot infrastructure)\n", pmm_zone.bitmap);

    size_t temp_pages = (mem_end - (uintptr_t)pmm_zone.bitmap) / PMM_PAGE_SIZE;
    size_t bitmap_size = (temp_pages + 7) / 8;
    debug_printf("[PMM] Bitmap size = %d bytes\n", (int)bitmap_size);

    pmm_zone.base = ALIGN_UP((uintptr_t)pmm_zone.bitmap + bitmap_size, 4096);
    debug_printf("[PMM] Managing pages from 0x%p\n", (void*)pmm_zone.base);

    if (pmm_zone.base >= mem_end) {
        panic("[PMM] FATAL: Kernel loaded at 0x%lx beyond usable memory (max 0x%lx, MAXPHYADDR %u bits)!",
              pmm_zone.base, mem_end - 1, pmm_maxphyaddr);
    }

    pmm_zone.pages = (mem_end - pmm_zone.base) / PMM_PAGE_SIZE;
    debug_printf("[PMM] Total pages = %d\n", (int)pmm_zone.pages);

    if (pmm_zone.pages == 0) {
        panic("[PMM] ERROR: No usable pages!");
    }

    memset(pmm_zone.bitmap, 0xFF, bitmap_size);

    // filter regions beyond MAXPHYADDR to prevent Reserved Bit Violation
    size_t unusable_pages = 0;

    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].type == E820_USABLE && entries[i].length > 0) {
            uintptr_t start = entries[i].base;
            uintptr_t end = entries[i].base + entries[i].length;

            if (start >= pmm_max_phys_addr) {
                debug_printf("[PMM] Skipping region 0x%lx-0x%lx (beyond MAXPHYADDR 0x%llx)\n",
                       start, end, pmm_max_phys_addr - 1);
                unusable_pages += (end - start) / PMM_PAGE_SIZE;
                continue;
            }

            if (end > pmm_max_phys_addr) {
                debug_printf("[PMM] Clamping region 0x%lx-0x%lx to MAXPHYADDR limit 0x%llx\n",
                       start, end, pmm_max_phys_addr - 1);
                unusable_pages += (end - pmm_max_phys_addr) / PMM_PAGE_SIZE;
                end = pmm_max_phys_addr;
            }

            if (end <= pmm_zone.base) continue;
            if (start < pmm_zone.base) start = pmm_zone.base;
            if (start >= end) continue;

            size_t start_page = (start - pmm_zone.base) / PMM_PAGE_SIZE;
            size_t end_page = (end - pmm_zone.base) / PMM_PAGE_SIZE;
            if (end_page > pmm_zone.pages) end_page = pmm_zone.pages;

            debug_printf("[PMM] Freeing pages: %zu .. %zu\n", start_page, end_page - 1);

            for (size_t j = start_page; j < end_page; j++) {
                pmm_set_bit(j, PMM_FRAME_FREE);
            }

            debug_printf("[PMM] Verifying first 10 freed pages...\n");
            for (size_t j = start_page; j < start_page + 10 && j < end_page; j++) {
                pmm_frame_state_t state = pmm_get_bit(j);
                debug_printf("[PMM]   Page %zu: state=%d (0=FREE, 1=USED)\n", j, state);
            }
        }
    }

    if (unusable_pages > 0) {
        debug_printf("[PMM] WARNING: %zu pages (%zu MB) beyond MAXPHYADDR marked unusable\n",
               unusable_pages, (unusable_pages * PMM_PAGE_SIZE) / (1024 * 1024));
    }

    extern uintptr_t _kernel_start;
    extern uintptr_t _kernel_end;
    pmm_reserve_region((uintptr_t)&_kernel_start, (uintptr_t)&_kernel_end, "Kernel");
    pmm_reserve_region((uintptr_t)pmm_zone.bitmap, (uintptr_t)pmm_zone.bitmap + bitmap_size, "Bitmap");

    pmm_zone.last_free = 0;
    // free_count was maintained incrementally by pmm_set_bit during init above

    spinlock_init(&pmm_zone.lock);
    pmm_initialized = true;

    debug_printf("[PMM] Initialized: %d MB available, %d pages.\n",
        (int)((pmm_zone.pages * PMM_PAGE_SIZE) / (1024 * 1024)),
        (int)pmm_zone.pages
    );
}

void* pmm_alloc(size_t pages) {
    if (!pages || !pmm_initialized) {
        return NULL;
    }

    spin_lock(&pmm_zone.lock);

    size_t start = pmm_find_free_sequence(pages);
    if (start == (size_t)-1) {
        spin_unlock(&pmm_zone.lock);
        return NULL;
    }

    for (size_t i = 0; i < pages; i++) {
        pmm_set_bit(start + i, PMM_FRAME_USED);
    }

    void* addr = (void*)(pmm_zone.base + start * PMM_PAGE_SIZE);

    // validate allocation doesn't exceed MAXPHYADDR — would cause Reserved Bit Violation in PTE
    uintptr_t phys_addr = (uintptr_t)addr;
    uintptr_t phys_end = phys_addr + (pages * PMM_PAGE_SIZE) - 1;

    if (phys_end >= pmm_max_phys_addr) {
        debug_printf("[PMM] ERROR: Allocated address range 0x%lx-0x%lx exceeds MAXPHYADDR (0x%llx)\n",
               phys_addr, phys_end, pmm_max_phys_addr - 1);
        debug_printf("[PMM]        This would cause Reserved Bit Violation!\n");
        debug_printf("[PMM]        Rolling back allocation...\n");

        for (size_t i = 0; i < pages; i++) {
            pmm_set_bit(start + i, PMM_FRAME_FREE);
        }

        if (start < pmm_zone.last_free) {
            pmm_zone.last_free = start;
        }

        spin_unlock(&pmm_zone.lock);
        return NULL;
    }

    spin_unlock(&pmm_zone.lock);
    return addr;
}

void* pmm_alloc_zero(size_t pages) {
    void* addr = pmm_alloc(pages);
    if (addr) {
        // pmm_alloc returns a physical address; this memset relies on identity
        // mapping (phys == virt) which is set up by the bootloader for low memory.
        // For addresses above the identity-mapped region, this would need
        // vmm_phys_to_virt(), but all PMM-managed memory is within the
        // identity-mapped range (0-256MB via 2MB large pages).
        memset(addr, 0, pages * PMM_PAGE_SIZE);
    }
    return addr;
}

void pmm_free(void* addr, size_t pages) {
    if (!addr || !pages || !pmm_initialized) return;

    uintptr_t base = (uintptr_t)addr;
    if (base < pmm_zone.base || base >= pmm_zone.base + pmm_zone.pages * PMM_PAGE_SIZE) {
        panic("PMM: Invalid free address %p", addr);
    }

    size_t first = (base - pmm_zone.base) / PMM_PAGE_SIZE;

    spin_lock(&pmm_zone.lock);

    // validate ALL pages before freeing ANY to prevent partial free on double-free detection
    for (size_t i = first; i < first + pages; i++) {
        if (pmm_get_bit(i) == PMM_FRAME_FREE) {
            debug_printf("[PMM] ERROR: Double free detected!\n");
            debug_printf("[PMM]   Address: 0x%lx\n", base + (i - first) * PMM_PAGE_SIZE);
            debug_printf("[PMM]   Page index: %lu (of %lu)\n", i, pmm_zone.pages);
            debug_printf("[PMM]   Pages requested: %lu\n", pages);
            panic("PMM: Double free detected at page %d", i);
        }
    }

    for (size_t i = first; i < first + pages; i++) {
        pmm_set_bit(i, PMM_FRAME_FREE);
    }

    if (first < pmm_zone.last_free) {
        pmm_zone.last_free = first;
    }

    spin_unlock(&pmm_zone.lock);
}

static void pmm_reserve_region(uintptr_t base, uintptr_t end, const char* name) {
    base = ALIGN_DOWN(base, PMM_PAGE_SIZE);
    end = ALIGN_UP(end, PMM_PAGE_SIZE);

    if (base >= end) return;

    // clamp base to zone start to avoid underflow arithmetic
    if (base < pmm_zone.base) {
        debug_printf("[PMM] Reserve: base 0x%lx < zone.base 0x%lx, clamping\n",
                     base, pmm_zone.base);
        base = pmm_zone.base;
    }

    if (base >= pmm_zone.base + pmm_zone.pages * PMM_PAGE_SIZE) {
        return;
    }
    if (end <= pmm_zone.base) {
        return;
    }

    uintptr_t zone_end = pmm_zone.base + pmm_zone.pages * PMM_PAGE_SIZE;
    if (end > zone_end) {
        end = zone_end;
    }

    size_t start_page = (base - pmm_zone.base) / PMM_PAGE_SIZE;
    size_t end_page = (end - pmm_zone.base) / PMM_PAGE_SIZE;

    if (start_page >= pmm_zone.pages) {
        return;
    }
    if (end_page > pmm_zone.pages) {
        end_page = pmm_zone.pages;
    }

    for (size_t i = start_page; i < end_page; i++) {
        pmm_set_bit(i, PMM_FRAME_RESERVED);
    }

    kprintf("PMM: Reserved %s at %p-%p\n", name, (void*)base, (void*)end);
}

static void pmm_set_bit(size_t bit, pmm_frame_state_t state) {
    size_t byte = bit / PMM_BITMAP_ALIGN;
    size_t offset = bit % PMM_BITMAP_ALIGN;
    bool was_free = !(pmm_zone.bitmap[byte] & (1 << offset));

    switch(state) {
        case PMM_FRAME_FREE:
            pmm_zone.bitmap[byte] &= ~(1 << offset);
            if (!was_free) pmm_zone.free_count++;
            break;
        default:
            pmm_zone.bitmap[byte] |= (1 << offset);
            if (was_free) pmm_zone.free_count--;
            break;
    }
}

static pmm_frame_state_t pmm_get_bit(size_t bit) {
    size_t byte = bit / PMM_BITMAP_ALIGN;
    size_t offset = bit % PMM_BITMAP_ALIGN;
    // returns USED (1) or FREE (0); other states are not distinguishable
    return (pmm_zone.bitmap[byte] & (1 << offset)) ? PMM_FRAME_USED : PMM_FRAME_FREE;
}

static size_t pmm_find_free_sequence(size_t count) {
    size_t consecutive = 0;
    size_t start_search_from = pmm_zone.last_free;

    for (size_t i = start_search_from; i < pmm_zone.pages; i++) {
        if (pmm_get_bit(i) == PMM_FRAME_FREE) {
            consecutive++;
            if (consecutive == count) {
                pmm_zone.last_free = i - count + 1;
                return i - count + 1;
            }
        } else {
            consecutive = 0;
        }
    }

    consecutive = 0;
    for (size_t i = 0; i < start_search_from; i++) {
        if (pmm_get_bit(i) == PMM_FRAME_FREE) {
            consecutive++;
            if (consecutive == count) {
                pmm_zone.last_free = i - count + 1;
                return i - count + 1;
            }
        } else {
            consecutive = 0;
        }
    }

    return (size_t)-1;
}

size_t pmm_total_pages(void) {
    return pmm_zone.pages;
}

size_t pmm_free_pages(void) {
    spin_lock(&pmm_zone.lock);
    size_t count = pmm_zone.free_count;
    spin_unlock(&pmm_zone.lock);
    return count;
}

size_t pmm_used_pages(void) {
    return pmm_total_pages() - pmm_free_pages();
}

uint64_t pmm_get_total_memory(void) {
    return pmm_zone.base + (pmm_zone.pages * PMM_PAGE_SIZE);
}

void pmm_dump_stats(void) {
    kprintf("Physical Memory Manager Statistics:\n");
    kprintf("  Total pages: %d (%d MB)\n",
           pmm_total_pages(),
           (pmm_total_pages() * PMM_PAGE_SIZE) / (1024 * 1024));
    kprintf("  Used pages:  %d (%d MB)\n",
           pmm_used_pages(),
           (pmm_used_pages() * PMM_PAGE_SIZE) / (1024 * 1024));
    kprintf("  Free pages:  %d (%d MB)\n",
           pmm_free_pages(),
           (pmm_free_pages() * PMM_PAGE_SIZE) / (1024 * 1024));
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
    for (size_t i = 0; i < pmm_zone.pages; i++) {
        uintptr_t addr = pmm_zone.base + i * PMM_PAGE_SIZE;

        if (addr < (uintptr_t)&_kernel_end &&
            pmm_get_bit(i) != PMM_FRAME_USED) {
            return false;
        }
    }
    return true;
}

bool pmm_is_usable_ram(uintptr_t phys_addr, size_t size) {
    if (size == 0) {
        return false;
    }

    uintptr_t range_end = phys_addr + size;

    // overflow means the range wraps around; treat conservatively as RAM to block MMIO mapping
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
