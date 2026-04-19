#include "pmtag.h"
#include "pmm.h"
#include "buddy.h"
#include "vmm.h"
#include "e820.h"
#include "boot_info.h"
#include "kernel_config.h"
#include "klib.h"
#include "error.h"

PhysTagTable g_pmtag = { 0 };

// Freestanding popcount — no libgcc dependency (__popcountdi2 unavailable).
static inline int pmtag_popcount64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

static void set_bits_in_page_range(size_t first_page, size_t last_page, uint64_t tags) {
    for (size_t p = first_page; p < last_page; p++) {
        uint64_t old_tags = g_pmtag.tag_map[p];
        uint64_t new_tags = old_tags | tags;
        g_pmtag.tag_map[p] = new_tags;
        uint64_t added = new_tags & ~old_tags;
        while (added) {
            int n = __builtin_ctzll(added);
            g_pmtag.bands[n][p / 64] |= (1ULL << (p % 64));
            added &= added - 1;
        }
    }
}

static void clear_bits_in_page_range(size_t first_page, size_t last_page, uint64_t tags) {
    for (size_t p = first_page; p < last_page; p++) {
        uint64_t old_tags = g_pmtag.tag_map[p];
        uint64_t new_tags = old_tags & ~tags;
        g_pmtag.tag_map[p] = new_tags;
        uint64_t removed = old_tags & ~new_tags;
        while (removed) {
            int n = __builtin_ctzll(removed);
            g_pmtag.bands[n][p / 64] &= ~(1ULL << (p % 64));
            removed &= removed - 1;
        }
    }
}

error_t PhysTagInit(void) {
    if (g_pmtag.initialized) {
        return ERR_ALREADY_INITIALIZED;
    }

    uint64_t mem_end = pmm_get_mem_end();
    if (mem_end == 0) {
        return ERR_NOT_INITIALIZED;
    }

    size_t page_count = mem_end / PMM_PAGE_SIZE;
    size_t band_words = (page_count + 63) / 64;

    // Allocate directly from PMM — these are permanent kernel metadata pages,
    // not heap allocations. Pull map is live (vmm_init ran before PhysTagInit).
    size_t tag_map_bytes = page_count * sizeof(uint64_t);
    size_t tag_map_pages = (tag_map_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    uintptr_t tag_map_phys = (uintptr_t)pmm_alloc(tag_map_pages);
    if (!tag_map_phys) {
        return ERR_NO_MEMORY;
    }
    uint64_t *tag_map = (uint64_t *)vmm_phys_to_virt(tag_map_phys);
    memset(tag_map, 0, tag_map_bytes);

    size_t band_bytes = 64 * band_words * sizeof(uint64_t);
    size_t band_pages = (band_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uintptr_t band_phys = (uintptr_t)pmm_alloc(band_pages);
    if (!band_phys) {
        pmm_free((void *)tag_map_phys, tag_map_pages);
        return ERR_NO_MEMORY;
    }
    uint64_t *band_mem = (uint64_t *)vmm_phys_to_virt(band_phys);
    memset(band_mem, 0, band_bytes);

    g_pmtag.tag_map   = tag_map;
    g_pmtag.page_count = page_count;
    g_pmtag.base_phys  = 0;
    g_pmtag.mem_end    = mem_end;
    g_pmtag.band_words = band_words;
    for (int i = 0; i < 64; i++) {
        g_pmtag.bands[i] = band_mem + (size_t)i * band_words;
    }
    spinlock_init(&g_pmtag.lock);
    g_pmtag.initialized = true;

    e820_entry_t *entries = memory_map_get_entries();
    size_t entry_count = memory_map_get_entry_count();

    for (size_t i = 0; i < entry_count; i++) {
        uintptr_t base = (uintptr_t)entries[i].base;
        uintptr_t end  = base + (uintptr_t)entries[i].length;

        if (entries[i].length == 0) continue;
        if (end > mem_end) end = (uintptr_t)mem_end;
        if (base >= end) continue;

        if (entries[i].type == E820_USABLE) {
            uintptr_t cursor = base;

            if (cursor < (uintptr_t)CONFIG_PHYS_ZONE_DMA32_END && cursor < end) {
                uintptr_t chunk_end = end < (uintptr_t)CONFIG_PHYS_ZONE_DMA32_END
                                    ? end
                                    : (uintptr_t)CONFIG_PHYS_ZONE_DMA32_END;
                PhysTagSet(cursor, chunk_end, PHYS_TAG_DMA32);
                cursor = chunk_end;
            }

            if (cursor >= (uintptr_t)CONFIG_PHYS_ZONE_DMA32_END &&
                cursor < (uintptr_t)CONFIG_PHYS_ZONE_USER_END && cursor < end) {
                uintptr_t chunk_end = end < (uintptr_t)CONFIG_PHYS_ZONE_USER_END
                                    ? end
                                    : (uintptr_t)CONFIG_PHYS_ZONE_USER_END;
                PhysTagSet(cursor, chunk_end, PHYS_TAG_USER);
                cursor = chunk_end;
            }

            if (cursor >= (uintptr_t)CONFIG_PHYS_ZONE_USER_END && cursor < end) {
                PhysTagSet(cursor, end, PHYS_TAG_HIGH);
            }
        } else {
            PhysTagSet(base, end, PHYS_TAG_MMIO);
        }
    }

    boot_info_t *bi = boot_info_get();
    if (boot_info_valid(bi) && bi->kernel_start < bi->kernel_end) {
        PhysTagSet((uintptr_t)bi->kernel_start, (uintptr_t)bi->kernel_end, PHYS_TAG_KERNEL);
    }

    debug_printf("[PMTAG] Initialized: %zu pages, tag_map=%zuKB, bands=%zuKB\n",
                 page_count,
                 (page_count * sizeof(uint64_t)) / 1024,
                 (64 * band_words * sizeof(uint64_t)) / 1024);

    return OK;
}

void PhysTagSet(uintptr_t start, uintptr_t end, uint64_t tags) {
    if (!g_pmtag.initialized || !tags) return;
    if (start < g_pmtag.base_phys) start = g_pmtag.base_phys;
    if (end > g_pmtag.mem_end) end = g_pmtag.mem_end;
    if (start >= end) return;

    size_t first_page = (start - g_pmtag.base_phys) / PMM_PAGE_SIZE;
    size_t last_page  = (end - g_pmtag.base_phys + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    if (last_page > g_pmtag.page_count) last_page = g_pmtag.page_count;

    spin_lock(&g_pmtag.lock);
    set_bits_in_page_range(first_page, last_page, tags);
    spin_unlock(&g_pmtag.lock);
}

void PhysTagClear(uintptr_t start, uintptr_t end, uint64_t tags) {
    if (!g_pmtag.initialized || !tags) return;
    if (start < g_pmtag.base_phys) start = g_pmtag.base_phys;
    if (end > g_pmtag.mem_end) end = g_pmtag.mem_end;
    if (start >= end) return;

    size_t first_page = (start - g_pmtag.base_phys) / PMM_PAGE_SIZE;
    size_t last_page  = (end - g_pmtag.base_phys + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    if (last_page > g_pmtag.page_count) last_page = g_pmtag.page_count;

    spin_lock(&g_pmtag.lock);
    clear_bits_in_page_range(first_page, last_page, tags);
    spin_unlock(&g_pmtag.lock);
}

uint64_t PhysTagGet(uintptr_t phys_addr) {
    if (!g_pmtag.initialized) return 0;
    if (phys_addr < g_pmtag.base_phys || phys_addr >= g_pmtag.mem_end) return 0;
    size_t page_idx = (phys_addr - g_pmtag.base_phys) / PMM_PAGE_SIZE;
    return g_pmtag.tag_map[page_idx];
}

static bool phys_tag_find_span_from(uint64_t required_tags, uintptr_t from_addr,
                                    uintptr_t *out_start, uintptr_t *out_end) {
    if (!g_pmtag.initialized) return false;
    if (required_tags == 0) {
        *out_start = g_pmtag.base_phys;
        *out_end   = g_pmtag.mem_end;
        return true;
    }

    size_t from_page = 0;
    if (from_addr > g_pmtag.base_phys) {
        from_page = (from_addr - g_pmtag.base_phys) / PMM_PAGE_SIZE;
    }
    if (from_page >= g_pmtag.page_count) return false;

    size_t from_word = from_page / 64;
    bool in_span = false;
    size_t span_start_page = 0;

    // Extract which bands to AND together
    uint64_t bits_to_check = required_tags;

    for (size_t w = from_word; w < g_pmtag.band_words; w++) {
        uint64_t mask = ~0ULL;
        uint64_t tmp = bits_to_check;
        while (tmp) {
            int n = __builtin_ctzll(tmp);
            mask &= g_pmtag.bands[n][w];
            tmp &= tmp - 1;
        }

        // Mask off bits before from_page in the first word
        if (w == from_word && (from_page % 64) != 0) {
            mask &= ~((1ULL << (from_page % 64)) - 1);
        }

        if (mask == 0) {
            if (in_span) {
                // span ended at last bit of previous word
                size_t span_end_page = w * 64;
                *out_start = g_pmtag.base_phys + span_start_page * PMM_PAGE_SIZE;
                *out_end   = g_pmtag.base_phys + span_end_page   * PMM_PAGE_SIZE;
                if (*out_end > g_pmtag.mem_end) *out_end = g_pmtag.mem_end;
                return true;
            }
            continue;
        }

        if (!in_span) {
            // Find first set bit in this word — that is the span start
            int first_bit = __builtin_ctzll(mask);
            span_start_page = w * 64 + (size_t)first_bit;
            in_span = true;
        }

        // Scan for end of contiguous run within this word.
        // If not all 64 bits are set, the span ends somewhere in this word.
        if (mask != ~0ULL) {
            // start_bit: the bit within this word where the span starts.
            // If span_start_page is in a PREVIOUS word, the entire current word
            // (from bit 0) is part of the span — start_bit = 0.
            // If span_start_page is in THIS word, start from its offset.
            int start_bit = 0;
            if (span_start_page / 64 == w) {
                start_bit = (int)(span_start_page % 64);
            }

            // Find the first 0-bit at or after start_bit.
            // ~mask gives 1s where bits were 0. Mask off bits before start_bit.
            uint64_t zeros_after_start = ~mask;
            if (start_bit > 0) {
                zeros_after_start &= ~((1ULL << start_bit) - 1);
            }

            if (zeros_after_start) {
                int first_zero = __builtin_ctzll(zeros_after_start);
                size_t span_end_page = w * 64 + (size_t)first_zero;
                *out_start = g_pmtag.base_phys + span_start_page * PMM_PAGE_SIZE;
                *out_end   = g_pmtag.base_phys + span_end_page   * PMM_PAGE_SIZE;
                if (*out_end > g_pmtag.mem_end) *out_end = g_pmtag.mem_end;
                return true;
            }
            // All bits from start_bit to 63 are set — span continues into next word.
        }
    }

    if (in_span) {
        *out_start = g_pmtag.base_phys + span_start_page * PMM_PAGE_SIZE;
        *out_end   = g_pmtag.mem_end;
        return true;
    }

    return false;
}

bool PhysTagFindSpan(uint64_t required_tags, uintptr_t *out_start, uintptr_t *out_end) {
    return phys_tag_find_span_from(required_tags, g_pmtag.base_phys, out_start, out_end);
}

void* PhysAllocTagged(size_t pages, uint64_t required_tags) {
    if (!g_pmtag.initialized || !pages) return NULL;

    uintptr_t search_start = g_pmtag.base_phys;

    for (int attempt = 0; attempt < 16; attempt++) {
        uintptr_t span_start, span_end;

        if (!phys_tag_find_span_from(required_tags, search_start, &span_start, &span_end)) {
            break;
        }

        void *result = buddy_alloc_range(pmm_get_buddy_zone(), pages, span_start, span_end);
        if (result != NULL) {
            return result;
        }

        search_start = span_end;
        if (search_start >= g_pmtag.mem_end) break;
    }

    return NULL;
}

void PhysAllocTaggedFree(void *addr, size_t pages) {
    if (!addr || !pages) return;
    pmm_free(addr, pages);
}

void PhysTagDump(void) {
    if (!g_pmtag.initialized) {
        debug_printf("[PMTAG] Not initialized\n");
        return;
    }

    size_t tag_map_kb  = (g_pmtag.page_count * sizeof(uint64_t)) / 1024;
    size_t bands_kb    = (64 * g_pmtag.band_words * sizeof(uint64_t)) / 1024;

    debug_printf("[PMTAG] pages=%zu  tag_map=%zuKB  bands=%zuKB\n",
                 g_pmtag.page_count, tag_map_kb, bands_kb);

    struct { uint64_t bit; const char *name; } known_tags[] = {
        { PHYS_TAG_DMA32,  "DMA32"  },
        { PHYS_TAG_USER,   "USER"   },
        { PHYS_TAG_HIGH,   "HIGH"   },
        { PHYS_TAG_KERNEL, "KERNEL" },
        { PHYS_TAG_MMIO,   "MMIO"   },
        { PHYS_TAG_SHARED, "SHARED" },
    };

    for (size_t t = 0; t < 6; t++) {
        int band_idx = __builtin_ctzll(known_tags[t].bit);
        size_t count = 0;
        for (size_t w = 0; w < g_pmtag.band_words; w++) {
            count += (size_t)pmtag_popcount64(g_pmtag.bands[band_idx][w]);
        }
        debug_printf("[PMTAG]   %-8s  %zu pages (%zu MB)\n",
                     known_tags[t].name, count,
                     (count * PMM_PAGE_SIZE) / (1024 * 1024));
    }
}
