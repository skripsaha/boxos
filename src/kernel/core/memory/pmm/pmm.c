#include "pmm.h"
#include "acpi.h"
#include "touch.h"
#include "buddy.h"
#include "vmm.h"
#include "memtag.h"
#include "tme.h"
#include "e820.h"
#include "klib.h"
#include "klib_logring.h"
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

static uintptr_t pmm_poisoned_bitmap_phys = 0;
static size_t    pmm_poisoned_bitmap_pages = 0;
static volatile uint64_t pmm_poisoned_set_count = 0;

#define PMM_MCE_RETRY_MAX 8

static inline uint8_t *pmm_poisoned_bitmap_virt(void) {
    if (!pmm_poisoned_bitmap_phys) return NULL;
    return (uint8_t *)vmm_phys_to_virt(pmm_poisoned_bitmap_phys);
}

static size_t pmm_usable_pages = 0;

typedef struct {
    uintptr_t start;
    uintptr_t end;
} DeferredRegion;

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
        debug_printf("[PMM] BUG: deferred table overflow at 0x%lx-0x%lx\n", start, end);
        return ERR_NO_MEMORY;
    }

    pmm_deferred[pmm_deferred_count].start = start;
    pmm_deferred[pmm_deferred_count].end   = end;
    pmm_deferred_count++;
    return OK;
}


#define EFI_TYPE_BOOT_SERVICES_CODE  3u
#define EFI_TYPE_BOOT_SERVICES_DATA  4u
#define EFI_MEMORY_DESC_TYPE_OFFSET  0u
#define EFI_MEMORY_DESC_PHYS_OFFSET  8u
#define EFI_MEMORY_DESC_PAGES_OFFSET 24u
#define EFI_MEMORY_DESC_MIN_SIZE     40u

static bool   pmm_bs_held        = false;
static size_t pmm_bs_pages_held  = 0;
static bool   pmm_bs_partial     = false;

static bool pmm_for_each_boot_services_range(
        void (*visit)(uintptr_t start, uintptr_t end))
{
    boot_info_t *bi = boot_info_get();
    if (!boot_info_valid(bi)) return false;
    if (bi->boot_method != 1) return false;
    if (!bi->efi_mmap_phys || !bi->efi_mmap_size ||
        bi->efi_mmap_desc_size < EFI_MEMORY_DESC_MIN_SIZE) return false;

    uint8_t *map = (uint8_t *)vmm_phys_to_virt((uintptr_t)bi->efi_mmap_phys);
    if (!map) return false;

    uint32_t desc_size = bi->efi_mmap_desc_size;
    uint8_t *end = map + bi->efi_mmap_size;

    for (uint8_t *p = map; p + desc_size <= end; p += desc_size) {
        uint32_t type;
        uint64_t phys, pages;
        memcpy(&type,  p + EFI_MEMORY_DESC_TYPE_OFFSET,  4);
        memcpy(&phys,  p + EFI_MEMORY_DESC_PHYS_OFFSET,  8);
        memcpy(&pages, p + EFI_MEMORY_DESC_PAGES_OFFSET, 8);

        if (type != EFI_TYPE_BOOT_SERVICES_CODE &&
            type != EFI_TYPE_BOOT_SERVICES_DATA) continue;
        if (pages == 0) continue;

        visit((uintptr_t)phys, (uintptr_t)(phys + pages * PMM_PAGE_SIZE));
    }
    return true;
}

static size_t pmm_pages_in_zone(uintptr_t start, uintptr_t end)
{
    uintptr_t zone_end = pmm_buddy.base + pmm_buddy.total_pages * PMM_PAGE_SIZE;
    if (start < pmm_buddy.base) start = pmm_buddy.base;
    if (end   > zone_end)       end   = zone_end;
    if (start >= end) return 0;
    start = (start + PMM_PAGE_SIZE - 1) & ~(uintptr_t)(PMM_PAGE_SIZE - 1);
    end  &= ~(uintptr_t)(PMM_PAGE_SIZE - 1);
    return (start < end) ? (end - start) / PMM_PAGE_SIZE : 0;
}

static void pmm_bs_reserve_one(uintptr_t start, uintptr_t end)
{
    size_t expected = pmm_pages_in_zone(start, end);
    size_t before   = pmm_buddy.free_count;
    buddy_reserve_range(&pmm_buddy, start, end);
    size_t taken = before - pmm_buddy.free_count;

    if (taken != expected) {
        pmm_bs_partial = true;
        kprintf("[PMM] boot-services range 0x%lx..0x%lx was only %zu of %zu "
                "page(s) free — this memory will not be handed back\n",
                (unsigned long)start, (unsigned long)end, taken, expected);
    }
    pmm_bs_pages_held += taken;
}

static void pmm_bs_release_one(uintptr_t start, uintptr_t end)
{
    buddy_free_range(&pmm_buddy, start, end);
}

void PmmHoldBootServicesMemory(void)
{
    if (pmm_bs_held) return;
    pmm_bs_pages_held = 0;
    pmm_bs_partial    = false;
    if (!pmm_for_each_boot_services_range(pmm_bs_reserve_one)) return;

    pmm_bs_held = true;
    debug_printf("[PMM] holding %zu page(s) of EFI boot-services memory until "
                 "SetVirtualAddressMap has returned\n", pmm_bs_pages_held);
}

void PmmReleaseBootServicesMemory(void)
{
    if (!pmm_bs_held) return;
    pmm_bs_held = false;

    if (pmm_bs_partial) {
        kprintf("[PMM] keeping %zu page(s) of EFI boot-services memory: at "
                "least one range was not this allocator's to give back\n",
                pmm_bs_pages_held);
        pmm_bs_pages_held = 0;
        return;
    }

    (void)pmm_for_each_boot_services_range(pmm_bs_release_one);
    kprintf("[PMM] EFI boot-services memory returned to the machine "
            "(%zu page(s))\n", pmm_bs_pages_held);
    pmm_bs_pages_held = 0;
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

    uintptr_t deferred_base = ALIGN_UP(map_start + alloc_map_size, 8);
    size_t    deferred_size = entry_count * sizeof(DeferredRegion);
    pmm_deferred     = (DeferredRegion *)deferred_base;
    pmm_deferred_cap = entry_count;
    pmm_deferred_count = 0;

    uintptr_t zone_base = ALIGN_UP(deferred_base + deferred_size, VMM_LARGE_PAGE_2M_SIZE);

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

            pmm_buddy_free_carved(&pmm_buddy, start, end,
                                  entries, entry_count);
        }
    }

    if (unusable_pages > 0) {
        debug_printf("[PMM] %zu pages beyond MAXPHYADDR unusable\n", unusable_pages);
    }

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

    PmmHoldBootServicesMemory();

    {
        uintptr_t keep_phys = 0;
        uint64_t  keep_len  = 0;
        if (LogKeepWindow(&keep_phys, &keep_len)) {
            buddy_reserve_range(&pmm_buddy, keep_phys, keep_phys + keep_len);
            debug_printf("[PMM] carry-over log window held at 0x%lx (%lu bytes)\n",
                         (unsigned long)keep_phys, (unsigned long)keep_len);
        }
    }

    {
        size_t pages_to_track = (size_t)(pmm_mem_end / PMM_PAGE_SIZE);
        size_t bitmap_bytes   = (pages_to_track + 7u) / 8u;
        size_t bitmap_pages   = (bitmap_bytes + PMM_PAGE_SIZE - 1u) / PMM_PAGE_SIZE;
        if (bitmap_pages > 0) {
            void *phys = buddy_alloc(&pmm_buddy, bitmap_pages);
            if (phys) {
                uint8_t *zero_virt = (uint8_t *)vmm_phys_to_virt((uintptr_t)phys);
                memset(zero_virt, 0, bitmap_pages * PMM_PAGE_SIZE);
                pmm_poisoned_bitmap_phys  = (uintptr_t)phys;
                pmm_poisoned_bitmap_pages = pages_to_track;
                debug_printf("[PMM] MCE poison bitmap: %zu pages tracked, "
                             "%zu bytes (%zu pages allocated at phys=0x%lx)\n",
                             pages_to_track, bitmap_bytes, bitmap_pages,
                             (unsigned long)phys);
            } else {
                debug_printf("[PMM] MCE poison bitmap alloc FAILED (%zu pages) — "
                             "MCE handler will still log + Touch, but allocator "
                             "won't auto-skip poisoned phys\n", bitmap_pages);
            }
        }
    }

    debug_printf("[PMM] Initialized: %zu MB available\n",
        (pmm_buddy.free_count * PMM_PAGE_SIZE) / (1024 * 1024));
    return OK;
}


void pmm_set_poisoned(uintptr_t phys) {
    uint8_t *bitmap = pmm_poisoned_bitmap_virt();
    if (!bitmap) return;
    size_t page = phys / PMM_PAGE_SIZE;
    if (page >= pmm_poisoned_bitmap_pages) return;
    size_t byte = page >> 3;
    uint8_t bit = (uint8_t)(1u << (page & 7u));
    uint8_t prev = __atomic_fetch_or(&bitmap[byte], bit, __ATOMIC_RELEASE);
    if (!(prev & bit)) {
        __atomic_add_fetch(&pmm_poisoned_set_count, 1, __ATOMIC_RELAXED);
    }
}

bool pmm_is_poisoned(uintptr_t phys) {
    uint8_t *bitmap = pmm_poisoned_bitmap_virt();
    if (!bitmap) return false;
    size_t page = phys / PMM_PAGE_SIZE;
    if (page >= pmm_poisoned_bitmap_pages) return false;
    size_t byte = page >> 3;
    uint8_t bit = (uint8_t)(1u << (page & 7u));
    uint8_t v = __atomic_load_n(&bitmap[byte], __ATOMIC_ACQUIRE);
    return (v & bit) != 0;
}

bool pmm_is_range_poisoned(uintptr_t phys, size_t pages) {
    if (!pmm_poisoned_bitmap_phys || !pages) return false;
    if (__atomic_load_n(&pmm_poisoned_set_count, __ATOMIC_RELAXED) == 0)
        return false;
    for (size_t i = 0; i < pages; i++) {
        if (pmm_is_poisoned(phys + i * PMM_PAGE_SIZE)) return true;
    }
    return false;
}

size_t pmm_poisoned_page_count(void) {
    return (size_t)__atomic_load_n(&pmm_poisoned_set_count, __ATOMIC_RELAXED);
}

uint32_t pmm_phys_domain(uintptr_t phys) {
    return acpi_numa_domain_for_phys((uint64_t)phys);
}

size_t pmm_pages_in_domain(uint32_t domain) {
    const acpi_numa_info_t* n = acpi_get_numa();
    if (!n) return 0;
    uint64_t total = 0;
    for (uint8_t i = 0; i < n->mem_count; i++) {
        if (!(n->mem[i].flags & 0x1)) continue;
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

    for (uint8_t i = 0; i < n->mem_count; i++) {
        if (!(n->mem[i].flags & 0x1)) continue;
        if (n->mem[i].domain != domain) continue;
        uintptr_t lo = (uintptr_t)n->mem[i].base;
        uintptr_t hi = (uintptr_t)(n->mem[i].base + n->mem[i].length);
        void* p = buddy_alloc_range(&pmm_buddy, pages, lo, hi);
        if (p) return p;
    }

    return buddy_alloc(&pmm_buddy, pages);
}

void pmm_log_numa_topology(void) {
    const acpi_numa_info_t* n = acpi_get_numa();
    if (!n) {
        debug_printf("[PMM] NUMA: no SRAT — uniform memory\n");
        return;
    }
    debug_printf("[PMM] NUMA: %u domain(s), %u CPU(s), %u memory range(s)\n",
                 n->domain_count, n->cpu_count, n->mem_count);
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

void* _pmm_alloc_impl(size_t pages, uint64_t tags) {
    if (!pages || !pmm_initialized) return NULL;

    void* addr = NULL;
    uint32_t retries_left = PMM_MCE_RETRY_MAX;

    for (;;) {
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
            return NULL;
        }

        if (!addr) break;
        if (!pmm_is_range_poisoned((uintptr_t)addr, pages)) break;

        for (size_t pi = 0; pi < pages; pi++) {
            pmm_set_poisoned((uintptr_t)addr + pi * PMM_PAGE_SIZE);
        }
        addr = NULL;
        if (retries_left-- == 0) {
            debug_printf("[PMM] MCE retry budget exhausted (pages=%zu tags=0x%lx)\n",
                         pages, (unsigned long)tags);
            break;
        }
    }

    if (!addr) {
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

void *pmm_alloc_with_keyid(size_t pages, uint16_t keyid) {
    void *phys = _pmm_alloc_zero_impl(pages, 0ULL);
    if (!phys) return NULL;

    if (g_tme.mk_active && keyid > 0 && keyid <= g_tme.max_keyid) {
        char tag[32];
        ksnprintf(tag, sizeof(tag), "tme:keyid:%u", (unsigned)keyid);
        (void)MemTagApplyByPhys((uintptr_t)phys, pages, tag);
    }
    return phys;
}

void pmm_free_with_keyid(void *phys, size_t pages, uint16_t keyid) {
    (void)keyid;
    pmm_free(phys, pages);
}

void pmm_free(void* addr, size_t pages) {
    if (!addr || !pages || !pmm_initialized) return;

    uintptr_t base = (uintptr_t)addr;
    uintptr_t zone_end = pmm_buddy.base + pmm_buddy.total_pages * PMM_PAGE_SIZE;
    if (base < pmm_buddy.base || base >= zone_end) {
        panic("PMM: Invalid free address %p", addr);
    }

    if (pmm_is_range_poisoned(base, pages)) {
        MemTagPmmFreed(base, pages);
        return;
    }

    buddy_free(&pmm_buddy, addr, pages);
    MemTagPmmFreed(base, pages);
}

size_t pmm_total_pages(void) {
    return pmm_usable_pages;
}

size_t pmm_max_alloc_pages(void)
{
    return (size_t)1 << BUDDY_MAX_ORDER;
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

    if (pmm_deferred) {
        pmm_deferred = (DeferredRegion*)vmm_phys_to_virt((uintptr_t)pmm_deferred);
    }

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

    #define HIGH_TEST_COUNT 16
    void* allocs[HIGH_TEST_COUNT];
    size_t high_count = 0;

    for (size_t i = 0; i < HIGH_TEST_COUNT && high_count < HIGH_TEST_COUNT; i++) {
        void* phys = pmm_alloc(1, PHYS_TAG_HIGH);
        if (!phys) break;
        if ((uintptr_t)phys < IDENTITY_MAP_LIMIT) {
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
        virt[255] = pattern;

        if (virt[0] == pattern && virt[1] == ~pattern && virt[255] == pattern) {
            pass++;
        } else {
            kprintf("[PMM TEST]   FAIL: single page phys=0x%lx\n", phys);
            fail++;
        }
        pmm_free(allocs[i], 1);
    }

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
            size_t test_offsets[] = {0, pages / 2, pages - 1};
            for (size_t t = 0; t < 3; t++) {
                uintptr_t page_phys = addr + test_offsets[t] * PMM_PAGE_SIZE;
                volatile uint64_t* virt = (volatile uint64_t*)vmm_phys_to_virt(page_phys);

                uint64_t pattern = page_phys ^ 0xCAFEBABE12345678ULL;
                virt[0] = pattern;
                virt[511] = ~pattern;

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