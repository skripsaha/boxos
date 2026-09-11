
#include "memtag.h"
#include "pmm.h"
#include "vmm.h"
#include "e820.h"
#include "boot_info.h"
#include "kernel_config.h"
#include "touch.h"
#include "cpuid.h"

typedef struct {
    uint32_t  region_id;
    uint16_t  flags;
    uint16_t  reserved;
    uintptr_t base_phys;
    uintptr_t base_virt;
    uint64_t  pages;
} MemTagEventRegion;

typedef struct {
    uint32_t  region_id;
    uint16_t  tag_id;
    uint16_t  reserved;
} MemTagEventTag;

static volatile bool g_touch_publish_enabled = false;


static MemTagRegistry      g_tag_registry;
static MemRegionRegistry   g_region_registry;
static MemTagBitmapIndex   g_bitmap_index;
static bool                g_memtag_initialized = false;

#define MEMTAG_PKU_MAX_KEYS   16u
static uint16_t            g_pku_tag_ids[MEMTAG_PKU_MAX_KEYS];
static bool                g_pku_tag_ids_resolved = false;

static int    PkuValueFromTagId(uint16_t tag_id);
static size_t StampPtePkeyRange(vmm_context_t *ctx, uintptr_t va_base,
                                 uint32_t pages, uint8_t pkey,
                                 uint8_t page_class);

bool MemTagIsInitialized(void) { return g_memtag_initialized; }

MemTagRegistry    *MemTagGetRegistry(void)       { return &g_tag_registry; }
MemRegionRegistry *MemTagGetRegionRegistry(void) { return &g_region_registry; }
MemTagBitmapIndex *MemTagGetBitmap(void)         { return &g_bitmap_index; }


void MemTagEnableTouchPublish(void) { g_touch_publish_enabled = true; }

static void PublishRegionEvent(const char *tag_str, uint32_t region_id) {
    if (!g_touch_publish_enabled) return;
    MemTagEventRegion ev = {0};
    ev.region_id = region_id;
    MemRegionSnapshot s;
    if (MemRegionRegistrySnapshot(&g_region_registry, region_id, &s) == OK) {
        ev.flags     = s.flags;
        ev.base_phys = s.base_phys;
        ev.base_virt = s.base_virt;
        ev.pages     = s.pages;
    }
    TouchPublish(tag_str, &ev, sizeof(ev));
}

static void PublishTagEvent(const char *tag_str, uint32_t region_id, uint16_t tag_id) {
    if (!g_touch_publish_enabled) return;
    MemTagEventTag ev = { .region_id = region_id, .tag_id = tag_id, .reserved = 0 };
    TouchPublish(tag_str, &ev, sizeof(ev));
}


static void MarkInternReserved(const char *kv) {
    uint16_t id = MemTagRegistryInternStr(&g_tag_registry, kv);
    if (id != MEMTAG_INVALID_TAG_ID) {
        MemTagRegistryMarkReserved(&g_tag_registry, id);
    }
}

static void SeedRegionTag(uint32_t region_id, const char *tag) {
    uint16_t tid = MemTagRegistryInternStr(&g_tag_registry, tag);
    if (tid == MEMTAG_INVALID_TAG_ID) return;
    if (MemRegionRegistryAddTag(&g_region_registry, region_id, tid) == OK) {
        MemTagBitmapSet(&g_bitmap_index, tid, region_id);
    }
}

static const char *ZoneTagForPhys(uintptr_t base) {
    if (base < (uintptr_t)CONFIG_PHYS_ZONE_DMA32_END)  return "zone:dma32";
    if (base < (uintptr_t)CONFIG_PHYS_ZONE_USER_END)   return "zone:user";
    return "zone:high";
}

static void SeedReservedTags(void) {
    MarkInternReserved("zone:dma32");
    MarkInternReserved("zone:user");
    MarkInternReserved("zone:high");
    MarkInternReserved("purpose:kernel");
    MarkInternReserved("purpose:mmio");
    MarkInternReserved("purpose:shared");
    MarkInternReserved("purpose:firmware");
    MarkInternReserved("pinned:true");
    MarkInternReserved("cache:wb");
    MarkInternReserved("cache:wt");
    MarkInternReserved("cache:uc");
    MarkInternReserved("cache:uc-");
    MarkInternReserved("cache:wc");
    MarkInternReserved("cache:wp");
    MarkInternReserved("mce:poisoned");
    MarkInternReserved("mce:corrected");
    MarkInternReserved("mce:fault:detected");
    MarkInternReserved("mce:fault:recovered");
    MarkInternReserved("mce:fault:fatal");
    MarkInternReserved("mce:migration:completed");
    MarkInternReserved("mce:migration:failed");
    MarkInternReserved("mce:migration:unmapped");
    MarkInternReserved("apei:ghes:ready");
    MarkInternReserved("apei:memory:error");
    MarkInternReserved("apei:processor:error");
    MarkInternReserved("apei:pcie:error");
    MarkInternReserved("apei:generic:error");
    MarkInternReserved("cet:enabled");
    MarkInternReserved("shstk:enabled");
    MarkInternReserved("ibt:enabled");
    MarkInternReserved("iommu:ready");
    MarkInternReserved("iommu:fault");
    MarkInternReserved("iommu:dma:mapped");
    MarkInternReserved("iommu:dma:unmapped");
    MarkInternReserved("iommu:domain:attached");
    MarkInternReserved("pku:ready");
    MarkInternReserved("pku:fault:denied");
    MarkInternReserved("pku:0");  MarkInternReserved("pku:1");
    MarkInternReserved("pku:2");  MarkInternReserved("pku:3");
    MarkInternReserved("pku:4");  MarkInternReserved("pku:5");
    MarkInternReserved("pku:6");  MarkInternReserved("pku:7");
    MarkInternReserved("pku:8");  MarkInternReserved("pku:9");
    MarkInternReserved("pku:10"); MarkInternReserved("pku:11");
    MarkInternReserved("pku:12"); MarkInternReserved("pku:13");
    MarkInternReserved("pku:14"); MarkInternReserved("pku:15");
    MarkInternReserved("lam:ready");
    MarkInternReserved("lam:enabled:u48");
    MarkInternReserved("lam:enabled:u57");
    MarkInternReserved("lam:fault:tag");
    MarkInternReserved("tme:ready");
    MarkInternReserved("tme:locked");
    MarkInternReserved("tme:active");
    MarkInternReserved("tme:mk_active");
    MarkInternReserved("tme:pool:ready");
    MarkInternReserved("tme:keyid:rekey:failed");
    MarkInternReserved("tme:keyid:0");  MarkInternReserved("tme:keyid:1");
    MarkInternReserved("tme:keyid:2");  MarkInternReserved("tme:keyid:3");
    MarkInternReserved("tme:keyid:4");  MarkInternReserved("tme:keyid:5");
    MarkInternReserved("tme:keyid:6");  MarkInternReserved("tme:keyid:7");
    MarkInternReserved("tme:keyid:8");  MarkInternReserved("tme:keyid:9");
    MarkInternReserved("tme:keyid:10"); MarkInternReserved("tme:keyid:11");
    MarkInternReserved("tme:keyid:12"); MarkInternReserved("tme:keyid:13");
    MarkInternReserved("tme:keyid:14"); MarkInternReserved("tme:keyid:15");
    MarkInternReserved("cet:ready");
    MarkInternReserved("cet:shstk:supervisor");
    MarkInternReserved("cet:shstk:user");
    MarkInternReserved("cet:fault:cp");
    MarkInternReserved("cet:ibt:enabled");
}

static void SeedZoneRegions(void) {
    e820_entry_t *entries = memory_map_get_entries();
    size_t entry_count    = memory_map_get_entry_count();
    uint64_t mem_end      = pmm_get_mem_end();

    size_t zones_created = 0, mmio_created = 0;

    for (size_t i = 0; i < entry_count; i++) {
        uintptr_t base = (uintptr_t)entries[i].base;
        uintptr_t end  = base + (uintptr_t)entries[i].length;
        if (entries[i].length == 0) continue;
        if (end > mem_end) end = (uintptr_t)mem_end;
        if (base >= end) continue;

        if (entries[i].type == E820_USABLE) {
            uintptr_t cursor = base;
            while (cursor < end) {
                uintptr_t chunk_end = end;
                const char *tag = ZoneTagForPhys(cursor);

                if (cursor < (uintptr_t)CONFIG_PHYS_ZONE_DMA32_END &&
                    chunk_end > (uintptr_t)CONFIG_PHYS_ZONE_DMA32_END) {
                    chunk_end = (uintptr_t)CONFIG_PHYS_ZONE_DMA32_END;
                }
                if (cursor < (uintptr_t)CONFIG_PHYS_ZONE_USER_END &&
                    chunk_end > (uintptr_t)CONFIG_PHYS_ZONE_USER_END) {
                    chunk_end = (uintptr_t)CONFIG_PHYS_ZONE_USER_END;
                }

                size_t pages = (chunk_end - cursor) / PMM_PAGE_SIZE;
                if (pages > 0) {
                    uint32_t rid = MemRegionRegistryCreate(
                        &g_region_registry,
                        cursor, 0, NULL, pages,
                        MEMTAG_REGION_FLAG_KERNEL |
                        MEMTAG_REGION_FLAG_PHYSICAL);
                    if (rid != MEMTAG_INVALID_REGION_ID) {
                        SeedRegionTag(rid, tag);
                        SeedRegionTag(rid, "cache:wb");
                        zones_created++;
                    }
                }
                cursor = chunk_end;
            }
        } else {
            size_t pages = (end - base) / PMM_PAGE_SIZE;
            if (!pages) pages = 1;
            uint32_t rid = MemRegionRegistryCreate(
                &g_region_registry, base, 0, NULL, pages,
                MEMTAG_REGION_FLAG_KERNEL | MEMTAG_REGION_FLAG_PHYSICAL);
            if (rid != MEMTAG_INVALID_REGION_ID) {
                SeedRegionTag(rid, "purpose:mmio");
                SeedRegionTag(rid, "cache:uc");
                mmio_created++;
            }
        }
    }

    boot_info_t *bi = boot_info_get();
    if (boot_info_valid(bi) && bi->kernel_start < bi->kernel_end) {
        uintptr_t k_base = (uintptr_t)bi->kernel_start;
        uintptr_t k_end  = (uintptr_t)bi->kernel_end;
        size_t k_pages = (k_end - k_base + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        if (k_pages > 0) {
            uint32_t rid = MemRegionRegistryCreate(
                &g_region_registry, k_base, 0, NULL, k_pages,
                MEMTAG_REGION_FLAG_KERNEL | MEMTAG_REGION_FLAG_PHYSICAL |
                MEMTAG_REGION_FLAG_PINNED);
            if (rid != MEMTAG_INVALID_REGION_ID) {
                SeedRegionTag(rid, "purpose:kernel");
                SeedRegionTag(rid, "cache:wb");
            }
        }
    }

    debug_printf("[MEMTAG] Seeded %zu zone regions + %zu MMIO regions\n",
                 zones_created, mmio_created);
}


error_t MemTagInit(void) {
    if (g_memtag_initialized) return ERR_ALREADY_INITIALIZED;

    error_t e = MemTagRegistryInit(&g_tag_registry);
    if (e != OK) {
        debug_printf("[MEMTAG] tag_registry init failed: %s\n", ErrorString(e));
        return e;
    }

    uint64_t mem_end = pmm_get_mem_end();
    if (!mem_end) {
        MemTagRegistryShutdown(&g_tag_registry);
        return ERR_NOT_INITIALIZED;
    }

    e = MemRegionRegistryInit(&g_region_registry, (size_t)mem_end);
    if (e != OK) {
        debug_printf("[MEMTAG] region_registry init failed: %s\n", ErrorString(e));
        MemTagRegistryShutdown(&g_tag_registry);
        return e;
    }

    e = MemTagBitmapInit(&g_bitmap_index,
                          MEMTAG_BITMAP_INITIAL_TAGS,
                          MEMTAG_BITMAP_INITIAL_REGS);
    if (e != OK) {
        debug_printf("[MEMTAG] bitmap init failed: %s\n", ErrorString(e));
        MemRegionRegistryShutdown(&g_region_registry);
        MemTagRegistryShutdown(&g_tag_registry);
        return e;
    }

    g_memtag_initialized = true;

    SeedReservedTags();
    SeedZoneRegions();

    MemTagStats s;
    MemTagGetStats(&s);
    debug_printf("[MEMTAG] Ready — tags=%u  regions=%u  reg_gen=%lu  bmp_gen=%lu\n",
                 s.tag_count, s.region_active,
                 (unsigned long)s.registry_generation,
                 (unsigned long)s.bitmap_generation);
    return OK;
}


uint16_t MemTagInternStr(const char *kv) {
    if (!g_memtag_initialized || !kv) return MEMTAG_INVALID_TAG_ID;
    return MemTagRegistryInternStr(&g_tag_registry, kv);
}

uint16_t MemTagResolveStr(const char *kv) {
    if (!g_memtag_initialized || !kv) return MEMTAG_INVALID_TAG_ID;
    return MemTagRegistryLookupStr(&g_tag_registry, kv);
}

uint16_t MemTagIntern(const char *key, const char *value) {
    if (!g_memtag_initialized || !key) return MEMTAG_INVALID_TAG_ID;
    return MemTagRegistryIntern(&g_tag_registry, key, value);
}

uint16_t MemTagResolve(const char *key, const char *value) {
    if (!g_memtag_initialized || !key) return MEMTAG_INVALID_TAG_ID;
    return MemTagRegistryLookup(&g_tag_registry, key, value);
}

const char *MemTagKey(uint16_t tag_id) {
    if (!g_memtag_initialized) return NULL;
    return MemTagRegistryKey(&g_tag_registry, tag_id);
}

const char *MemTagValue(uint16_t tag_id) {
    if (!g_memtag_initialized) return NULL;
    return MemTagRegistryValue(&g_tag_registry, tag_id);
}


static void ApplyDerivedTags(uint32_t region_id, uintptr_t base_phys) {
    if (!base_phys) return;
    uint32_t domain = pmm_phys_domain(base_phys);
    if (domain == 0xFFFFFFFFu) return;
    char buf[32];
    ksnprintf(buf, sizeof(buf), "numa:domain:%u", domain);
    uint16_t tid = MemTagRegistryInternStr(&g_tag_registry, buf);
    if (tid == MEMTAG_INVALID_TAG_ID) return;
    if (MemRegionRegistryAddTag(&g_region_registry, region_id, tid) == OK) {
        MemTagBitmapSet(&g_bitmap_index, tid, region_id);
        MemTagRegistryRefInc(&g_tag_registry, tid);
    }
}

uint32_t MemRegionCreate(uintptr_t base_phys, uintptr_t base_virt,
                          void *ctx, size_t pages, uint16_t flags) {
    if (!g_memtag_initialized) return MEMTAG_INVALID_REGION_ID;
    uint32_t rid = MemRegionRegistryCreate(&g_region_registry,
                                            base_phys, base_virt, ctx, pages, flags);
    if (rid != MEMTAG_INVALID_REGION_ID) {
        ApplyDerivedTags(rid, base_phys);
        PublishRegionEvent("memtag:region:added", rid);
    }
    return rid;
}

void MemRegionDestroy(uint32_t region_id) {
    if (!g_memtag_initialized || region_id == MEMTAG_INVALID_REGION_ID) return;

    PublishRegionEvent("memtag:region:released", region_id);

    uint16_t tag_id;
    while (MemRegionRegistryListTags(&g_region_registry, region_id, &tag_id, 1) == 1) {
        if (MemRegionRegistryRemoveTag(&g_region_registry, region_id, tag_id) != OK)
            break;
        MemTagBitmapClear(&g_bitmap_index, tag_id, region_id);
        MemTagRegistryRefDec(&g_tag_registry, tag_id);
    }

    MemRegionRegistryDestroy(&g_region_registry, region_id);
}

error_t MemRegionInfo(uint32_t region_id, MemRegionSnapshot *out) {
    if (!g_memtag_initialized) return ERR_NOT_INITIALIZED;
    return MemRegionRegistrySnapshot(&g_region_registry, region_id, out);
}

bool MemRegionIsActive(uint32_t region_id) {
    if (!g_memtag_initialized) return false;
    return MemRegionRegistryIsActive(&g_region_registry, region_id);
}

uint32_t MemRegionFromPhys(uintptr_t phys) {
    if (!g_memtag_initialized) return MEMTAG_INVALID_REGION_ID;
    return MemRegionRegistryFromPhys(&g_region_registry, phys);
}

uint32_t MemRegionFromVirt(void *ctx, uintptr_t virt) {
    if (!g_memtag_initialized) return MEMTAG_INVALID_REGION_ID;
    vmm_context_t *vctx = (vmm_context_t *)ctx;
    if (!vctx) vctx = vmm_get_current_context();
    if (!vctx) return MEMTAG_INVALID_REGION_ID;

    uint8_t level = 0;
    pte_t *leaf = vmm_get_leaf_pte(vctx, virt, &level);
    if (!leaf) return MEMTAG_INVALID_REGION_ID;
    pte_t entry = __atomic_load_n(leaf, __ATOMIC_ACQUIRE);
    if (!(entry & VMM_FLAG_PRESENT)) return MEMTAG_INVALID_REGION_ID;

    uintptr_t page_size = (level == 3) ? (1ULL << 30)
                        : (level == 2) ? (1ULL << 21)
                                       : (1ULL << 12);
    uintptr_t phys = (vmm_pte_to_phys(entry) & ~(page_size - 1)) + (virt & (page_size - 1));
    if (!phys) return MEMTAG_INVALID_REGION_ID;
    return MemRegionFromPhys(phys);
}


error_t MemTagApply(uint32_t region_id, const char *tag_str) {
    if (!g_memtag_initialized || !tag_str)
        return ERR_INVALID_ARGUMENT;
    if (region_id == MEMTAG_INVALID_REGION_ID)
        return ERR_INVALID_ARGUMENT;

    uint16_t tid = MemTagRegistryInternStr(&g_tag_registry, tag_str);
    if (tid == MEMTAG_INVALID_TAG_ID) return ERR_NO_MEMORY;

    error_t e = MemRegionRegistryAddTag(&g_region_registry, region_id, tid);
    if (e != OK) return e;

    MemTagBitmapSet(&g_bitmap_index, tid, region_id);
    MemTagRegistryRefInc(&g_tag_registry, tid);
    PublishTagEvent("memtag:tag:applied", region_id, tid);

    int pku_v = PkuValueFromTagId(tid);
    if (pku_v >= 0) {
        (void)MemTagSweepPkey(region_id, (uint8_t)pku_v);
    }
    return OK;
}

error_t MemTagApplyMany(uint32_t region_id, const char *const *tag_strs) {
    if (!tag_strs) return ERR_INVALID_ARGUMENT;
    for (size_t i = 0; tag_strs[i] != NULL; i++) {
        error_t e = MemTagApply(region_id, tag_strs[i]);
        if (e != OK) return e;
    }
    return OK;
}

error_t MemTagClear(uint32_t region_id, const char *tag_str) {
    if (!g_memtag_initialized || !tag_str)
        return ERR_INVALID_ARGUMENT;
    if (region_id == MEMTAG_INVALID_REGION_ID)
        return ERR_INVALID_ARGUMENT;

    uint16_t tid = MemTagRegistryLookupStr(&g_tag_registry, tag_str);
    if (tid == MEMTAG_INVALID_TAG_ID) return OK;

    error_t e = MemRegionRegistryRemoveTag(&g_region_registry, region_id, tid);
    if (e == OK) {
        MemTagBitmapClear(&g_bitmap_index, tid, region_id);
        MemTagRegistryRefDec(&g_tag_registry, tid);
        PublishTagEvent("memtag:tag:cleared", region_id, tid);

        int pku_v = PkuValueFromTagId(tid);
        if (pku_v >= 0) {
            uint8_t new_pkey = MemRegionEffectivePkey(region_id);
            (void)MemTagSweepPkey(region_id, new_pkey);
        }
    }
    return e;
}

bool MemRegionHasTag(uint32_t region_id, uint16_t tag_id) {
    if (!g_memtag_initialized) return false;
    return MemRegionRegistryHasTag(&g_region_registry, region_id, tag_id);
}

bool MemRegionHasTagStr(uint32_t region_id, const char *tag_str) {
    if (!g_memtag_initialized || !tag_str) return false;
    uint16_t tid = MemTagRegistryLookupStr(&g_tag_registry, tag_str);
    if (tid == MEMTAG_INVALID_TAG_ID) return false;
    return MemRegionHasTag(region_id, tid);
}

size_t MemRegionListTags(uint32_t region_id, uint16_t *out, size_t max) {
    if (!g_memtag_initialized) return 0;
    return MemRegionRegistryListTags(&g_region_registry, region_id, out, max);
}

error_t MemTagApplyByPhys(uintptr_t base_phys, size_t pages, const char *tag_str) {
    if (!g_memtag_initialized || !base_phys || !pages || !tag_str)
        return ERR_INVALID_ARGUMENT;

    uint32_t rid = MemRegionFromPhys(base_phys);
    if (rid != MEMTAG_INVALID_REGION_ID) {
        MemRegion *r = MemRegionRegistrySlot(&g_region_registry, rid);
        if (r && (r->flags & MEMTAG_REGION_FLAG_ACTIVE) &&
            r->base_phys == base_phys && r->pages == pages &&
            !(r->flags & MEMTAG_REGION_FLAG_KERNEL)) {
            return MemTagApply(rid, tag_str);
        }
    }

    rid = MemRegionCreate(base_phys, 0, NULL, pages, MEMTAG_REGION_FLAG_PHYSICAL);
    if (rid == MEMTAG_INVALID_REGION_ID) return ERR_NO_MEMORY;
    return MemTagApply(rid, tag_str);
}

error_t MemTagClearByPhys(uintptr_t base_phys, const char *tag_str) {
    if (!g_memtag_initialized || !base_phys || !tag_str)
        return ERR_INVALID_ARGUMENT;
    uint32_t rid = MemRegionFromPhys(base_phys);
    if (rid == MEMTAG_INVALID_REGION_ID) return OK;
    return MemTagClear(rid, tag_str);
}


static size_t ResolveTagStrs(const char *const *strs, uint16_t *out, size_t max) {
    size_t n = 0;
    if (!strs) return 0;
    for (size_t i = 0; strs[i] != NULL && n < max; i++) {
        out[n++] = MemTagRegistryLookupStr(&g_tag_registry, strs[i]);
    }
    return n;
}


void MemTagQueryAndInto_(const char *const *tag_strs, MemTagResult *out) {
    if (!out) return;
    out->count = 0;
    if (!g_memtag_initialized) return;

    uint16_t ids[64];
    size_t n = ResolveTagStrs(tag_strs, ids, 64);
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == MEMTAG_INVALID_TAG_ID) return;
    }
    out->count = MemTagBitmapQueryAnd(&g_bitmap_index, ids, (uint16_t)n,
                                       out->region_ids,
                                       sizeof(out->region_ids) /
                                       sizeof(out->region_ids[0]));
}

void MemTagQueryOrInto_(const char *const *tag_strs, MemTagResult *out) {
    if (!out) return;
    out->count = 0;
    if (!g_memtag_initialized) return;

    uint16_t ids[64];
    size_t n = ResolveTagStrs(tag_strs, ids, 64);
    uint16_t known[64]; uint16_t kn = 0;
    for (size_t i = 0; i < n; i++)
        if (ids[i] != MEMTAG_INVALID_TAG_ID) known[kn++] = ids[i];
    if (kn == 0) return;
    out->count = MemTagBitmapQueryOr(&g_bitmap_index, known, kn,
                                      out->region_ids,
                                      sizeof(out->region_ids) /
                                      sizeof(out->region_ids[0]));
}

void MemTagQueryMixedInto_(const char *const *required,
                            const char *const *any,
                            const char *const *excluded,
                            MemTagResult *out) {
    if (!out) return;
    out->count = 0;
    if (!g_memtag_initialized) return;

    uint16_t req[64], an[64], ex[64];
    size_t nr = ResolveTagStrs(required, req, 64);
    size_t na = ResolveTagStrs(any,      an,  64);
    size_t ne = ResolveTagStrs(excluded, ex,  64);

    for (size_t i = 0; i < nr; i++)
        if (req[i] == MEMTAG_INVALID_TAG_ID) return;

    uint16_t a2[64], e2[64]; uint16_t na2 = 0, ne2 = 0;
    for (size_t i = 0; i < na; i++)
        if (an[i] != MEMTAG_INVALID_TAG_ID) a2[na2++] = an[i];
    for (size_t i = 0; i < ne; i++)
        if (ex[i] != MEMTAG_INVALID_TAG_ID) e2[ne2++] = ex[i];

    out->count = MemTagBitmapQueryMixed(&g_bitmap_index,
                                         req, (uint16_t)nr,
                                         a2,  na2,
                                         e2,  ne2,
                                         out->region_ids,
                                         sizeof(out->region_ids) /
                                         sizeof(out->region_ids[0]));
}

MemTagResult MemTagQueryAnd_(const char *const *tag_strs) {
    MemTagResult r;
    MemTagQueryAndInto_(tag_strs, &r);
    return r;
}

MemTagResult MemTagQueryOr_(const char *const *tag_strs) {
    MemTagResult r;
    MemTagQueryOrInto_(tag_strs, &r);
    return r;
}

MemTagResult MemTagQueryMixed_(const char *const *required,
                                const char *const *any,
                                const char *const *excluded) {
    MemTagResult r;
    MemTagQueryMixedInto_(required, any, excluded, &r);
    return r;
}


static uint64_t ZoneBiasFromTag(const char *tag) {
    if (!tag) return 0;
    if (strcmp(tag, "zone:dma32") == 0) return PHYS_TAG_DMA32;
    if (strcmp(tag, "zone:user")  == 0) return PHYS_TAG_USER;
    if (strcmp(tag, "zone:high")  == 0) return PHYS_TAG_HIGH;
    return 0;
}

static void *AllocAndRegister(size_t pages, const char *tag_str, bool zero) {
    uint64_t zone_bias = ZoneBiasFromTag(tag_str);
    void *phys = zero
        ? _pmm_alloc_zero_impl(pages, zone_bias)
        : _pmm_alloc_impl(pages, zone_bias);
    if (!phys) return NULL;

    if (g_memtag_initialized && tag_str) {
        uint32_t rid = MemRegionRegistryCreate(&g_region_registry,
                                                (uintptr_t)phys, 0, NULL, pages,
                                                MEMTAG_REGION_FLAG_PHYSICAL);
        if (rid != MEMTAG_INVALID_REGION_ID) {
            uint16_t tid = MemTagRegistryInternStr(&g_tag_registry, tag_str);
            if (tid != MEMTAG_INVALID_TAG_ID) {
                MemRegionRegistryAddTag(&g_region_registry, rid, tid);
                MemTagBitmapSet(&g_bitmap_index, tid, rid);
                MemTagRegistryRefInc(&g_tag_registry, tid);
            }
            ApplyDerivedTags(rid, (uintptr_t)phys);
        }
    }
    return phys;
}

void *MemTagPmmAlloc(size_t pages, const char *tag_str) {
    return AllocAndRegister(pages, tag_str, false);
}

void *MemTagPmmAllocZero(size_t pages, const char *tag_str) {
    return AllocAndRegister(pages, tag_str, true);
}

void MemTagPmmFreed(uintptr_t base_phys, size_t pages) {
    if (!g_memtag_initialized || !base_phys) return;
    size_t first = base_phys / PMM_PAGE_SIZE;
    size_t last  = first + pages;
    if (first >= g_region_registry.page_count) return;
    if (last  >  g_region_registry.page_count) last = g_region_registry.page_count;

    uint32_t prev_id = MEMTAG_INVALID_REGION_ID;
    for (size_t p = first; p < last; p++) {
        uint32_t rid = g_region_registry.id_by_page[p];
        if (rid == MEMTAG_INVALID_REGION_ID || rid == prev_id) continue;
        prev_id = rid;
        MemRegion *r = MemRegionRegistrySlot(&g_region_registry, rid);
        if (!r) continue;
        if (r->flags & MEMTAG_REGION_FLAG_KERNEL) continue;
        MemRegionDestroy(rid);
    }
}


#define MEMTAG_MASK_WORDS  16
#define MEMTAG_MASK_BITS   (MEMTAG_MASK_WORDS * 64)


#define MEMTAG_ENFORCE_REGIONS_PER_CALL   256u

#define MEMTAG_ENFORCE_ATTACHES_PER_REGION  32u

#define MEMTAG_GUARDS_PER_REGION  64u

#define MEMTAG_SWEEP_PIDS_PER_CALL  256u

static inline bool MaskHas(const uint64_t *mask, uint16_t tag_id) {
    if (tag_id >= MEMTAG_MASK_BITS) return false;
    return (mask[tag_id >> 6] >> (tag_id & 63)) & 1ULL;
}

static inline void MaskSetAtomic(uint64_t *mask, uint16_t tag_id) {
    if (tag_id >= MEMTAG_MASK_BITS) return;
    __atomic_or_fetch(&mask[tag_id >> 6], (1ULL << (tag_id & 63)),
                      __ATOMIC_ACQ_REL);
}

static inline void MaskClearAtomic(uint64_t *mask, uint16_t tag_id) {
    if (tag_id >= MEMTAG_MASK_BITS) return;
    __atomic_and_fetch(&mask[tag_id >> 6], ~(1ULL << (tag_id & 63)),
                       __ATOMIC_ACQ_REL);
}


error_t MemTagSetGuard(const char *tag_str, bool guard) {
    if (!g_memtag_initialized || !tag_str) return ERR_INVALID_ARGUMENT;
    uint16_t tid = MemTagRegistryInternStr(&g_tag_registry, tag_str);
    if (tid == MEMTAG_INVALID_TAG_ID) return ERR_NO_MEMORY;

    bool was_guard = MemTagRegistryIsGuard(&g_tag_registry, tid);
    if (guard) MemTagRegistryMarkGuard(&g_tag_registry, tid);
    else       MemTagRegistryClearGuard(&g_tag_registry, tid);

    if (was_guard != guard) {
        (void)MemTagSweepGuard(tid, guard);
    }

    if (g_touch_publish_enabled) {
        struct { uint16_t tag_id; uint8_t guard; uint8_t reserved; } ev;
        ev.tag_id   = tid;
        ev.guard    = guard ? 1 : 0;
        ev.reserved = 0;
        TouchPublish("memtag:guard:set", &ev, sizeof(ev));
    }
    return OK;
}

bool MemTagIsGuard(uint16_t tag_id) {
    if (!g_memtag_initialized) return false;
    return MemTagRegistryIsGuard(&g_tag_registry, tag_id);
}


struct process_t;
typedef struct process_t process_t;
process_t *process_find_ref(uint32_t pid);
void       process_ref_dec(process_t *proc);

extern uint64_t *process_active_memtags(process_t *proc);
extern void     *process_get_cabin(process_t *proc);

static void   SnapshotMask(const uint64_t *mask, uint64_t *mask_snap);
static size_t EnforceForCabinTag(vmm_context_t *target_ctx,
                                  const uint64_t *mask,
                                  uint16_t tag_id, bool revoke);

static void SnapshotMask(const uint64_t *mask, uint64_t *mask_snap) {
    if (mask) {
        for (uint32_t i = 0; i < MEMTAG_MASK_WORDS; i++)
            mask_snap[i] = __atomic_load_n(&mask[i], __ATOMIC_ACQUIRE);
    } else {
        for (uint32_t i = 0; i < MEMTAG_MASK_WORDS; i++) mask_snap[i] = 0;
    }
}

error_t MemCabinGrant(uint32_t pid, uint16_t tag_id) {
    if (!g_memtag_initialized || tag_id == MEMTAG_INVALID_TAG_ID)
        return ERR_INVALID_ARGUMENT;
    if (tag_id >= MEMTAG_MASK_BITS) return ERR_NO_MEMORY;

    process_t *proc = process_find_ref(pid);
    if (!proc) return ERR_OBJECT_NOT_FOUND;
    uint64_t *mask = process_active_memtags(proc);
    bool already_held = mask ? MaskHas(mask, tag_id) : false;
    if (mask && !already_held) MaskSetAtomic(mask, tag_id);

    if (!already_held && MemTagRegistryIsGuard(&g_tag_registry, tag_id)) {
        vmm_context_t *ctx = (vmm_context_t *)process_get_cabin(proc);
        if (ctx) {
            uint64_t mask_snap[MEMTAG_MASK_WORDS];
            SnapshotMask(mask, mask_snap);
            (void)EnforceForCabinTag(ctx, mask_snap, tag_id, false);
        }
    }
    process_ref_dec(proc);

    if (g_touch_publish_enabled) {
        struct { uint32_t pid; uint16_t tag_id; uint16_t reserved; } ev;
        ev.pid = pid; ev.tag_id = tag_id; ev.reserved = 0;
        TouchPublish("memtag:cabin:granted", &ev, sizeof(ev));
    }
    return OK;
}

error_t MemCabinRevoke(uint32_t pid, uint16_t tag_id) {
    if (!g_memtag_initialized || tag_id == MEMTAG_INVALID_TAG_ID)
        return ERR_INVALID_ARGUMENT;
    if (tag_id >= MEMTAG_MASK_BITS) return OK;

    process_t *proc = process_find_ref(pid);
    if (!proc) return ERR_OBJECT_NOT_FOUND;
    uint64_t *mask = process_active_memtags(proc);
    bool was_held = mask ? MaskHas(mask, tag_id) : false;
    if (mask && was_held) MaskClearAtomic(mask, tag_id);

    if (was_held && MemTagRegistryIsGuard(&g_tag_registry, tag_id)) {
        vmm_context_t *ctx = (vmm_context_t *)process_get_cabin(proc);
        if (ctx) {
            uint64_t mask_snap[MEMTAG_MASK_WORDS];
            SnapshotMask(mask, mask_snap);
            (void)EnforceForCabinTag(ctx, mask_snap, tag_id, true);
        }
    }
    process_ref_dec(proc);

    if (g_touch_publish_enabled) {
        struct { uint32_t pid; uint16_t tag_id; uint16_t reserved; } ev;
        ev.pid = pid; ev.tag_id = tag_id; ev.reserved = 0;
        TouchPublish("memtag:cabin:revoked", &ev, sizeof(ev));
    }
    return OK;
}

bool MemCabinHolds(uint32_t pid, uint16_t tag_id) {
    if (!g_memtag_initialized || tag_id == MEMTAG_INVALID_TAG_ID) return false;
    if (tag_id >= MEMTAG_MASK_BITS) return false;
    process_t *proc = process_find_ref(pid);
    if (!proc) return false;
    uint64_t *mask = process_active_memtags(proc);
    bool h = mask ? MaskHas(mask, tag_id) : false;
    process_ref_dec(proc);
    return h;
}

size_t MemCabinListTags(uint32_t pid, uint16_t *out, size_t max) {
    if (!g_memtag_initialized || !out) return 0;
    process_t *proc = process_find_ref(pid);
    if (!proc) return 0;
    uint64_t *mask = process_active_memtags(proc);
    size_t n = 0;
    if (mask) {
        for (uint16_t i = 0; i < MEMTAG_MASK_BITS && n < max; i++) {
            uint64_t w = __atomic_load_n(&mask[i >> 6], __ATOMIC_ACQUIRE);
            if ((w >> (i & 63)) & 1ULL) out[n++] = i;
        }
    }
    process_ref_dec(proc);
    return n;
}


static size_t GuardsOnRegion(uint32_t region_id, uint16_t *out, size_t max) {
    uint16_t tags[MEMTAG_GUARDS_PER_REGION];
    size_t n = MemRegionRegistryListTags(&g_region_registry, region_id, tags,
                                          MEMTAG_GUARDS_PER_REGION);
    if (n == 0) return 0;
    size_t ng = 0;
    for (size_t i = 0; i < n && ng < max; i++) {
        if (MemTagRegistryIsGuard(&g_tag_registry, tags[i])) out[ng++] = tags[i];
    }
    return ng;
}

bool MemRegionAccessAllowed(uint32_t pid, uint32_t region_id) {
    if (!g_memtag_initialized) return true;
    if (region_id == MEMTAG_INVALID_REGION_ID) return false;

    uint16_t guards[MEMTAG_GUARDS_PER_REGION];
    size_t ng = GuardsOnRegion(region_id, guards, MEMTAG_GUARDS_PER_REGION);
    if (ng == 0) return true;

    process_t *proc = process_find_ref(pid);
    if (!proc) return false;
    uint64_t *mask = process_active_memtags(proc);

    bool allowed = true;
    for (size_t i = 0; i < ng; i++) {
        if (!mask || !MaskHas(mask, guards[i])) { allowed = false; break; }
    }
    process_ref_dec(proc);
    return allowed;
}


bool MemTagEnforce(uint32_t pid, uintptr_t va, uint32_t region_id) {
    if (region_id == MEMTAG_INVALID_REGION_ID) return true;
    if (MemRegionAccessAllowed(pid, region_id)) return true;

    if (g_touch_publish_enabled) {
        struct {
            uint32_t pid;
            uint32_t region_id;
            uint64_t va;
            uint16_t missing_tag_id;
            uint16_t reserved[3];
        } ev;
        ev.pid             = pid;
        ev.region_id       = region_id;
        ev.va              = (uint64_t)va;
        ev.missing_tag_id  = MemRegionFirstMissingGuard(pid, region_id);
        ev.reserved[0] = ev.reserved[1] = ev.reserved[2] = 0;
        TouchPublish("memtag:fault:denied", &ev, sizeof(ev));
    }
    return false;
}

bool MemTagEnforcePhys(uint32_t pid, uintptr_t phys) {
    if (!phys) return true;
    uint32_t rid = MemRegionFromPhys(phys);
    if (rid == MEMTAG_INVALID_REGION_ID) return true;
    return MemTagEnforce(pid, 0, rid);
}

uint16_t MemRegionFirstMissingGuard(uint32_t pid, uint32_t region_id) {
    if (!g_memtag_initialized) return MEMTAG_INVALID_TAG_ID;
    if (region_id == MEMTAG_INVALID_REGION_ID) return MEMTAG_INVALID_TAG_ID;

    uint16_t guards[MEMTAG_GUARDS_PER_REGION];
    size_t ng = GuardsOnRegion(region_id, guards, MEMTAG_GUARDS_PER_REGION);
    if (ng == 0) return MEMTAG_INVALID_TAG_ID;

    process_t *proc = process_find_ref(pid);
    uint64_t *mask = proc ? process_active_memtags(proc) : NULL;

    uint16_t missing = MEMTAG_INVALID_TAG_ID;
    for (size_t i = 0; i < ng; i++) {
        if (!mask || !MaskHas(mask, guards[i])) { missing = guards[i]; break; }
    }
    if (proc) process_ref_dec(proc);
    return missing;
}


void MemTagGetStats(MemTagStats *out) {
    if (!out) return;
    if (!g_memtag_initialized) { memset(out, 0, sizeof(*out)); return; }
    out->tag_count           = MemTagRegistryTotalTags(&g_tag_registry);
    out->region_active       = MemRegionRegistryActiveCount(&g_region_registry);
    out->region_slot_count   = g_region_registry.slot_count;
    out->region_slot_cap     = g_region_registry.slot_cap;
    out->registry_generation = MemTagRegistryGeneration(&g_tag_registry);
    out->region_generation   = MemRegionRegistryGeneration(&g_region_registry);
    out->bitmap_generation   = MemTagBitmapGeneration(&g_bitmap_index);
    out->cache_hits          = MemTagBitmapCacheHits(&g_bitmap_index);
    out->cache_misses        = MemTagBitmapCacheMisses(&g_bitmap_index);
}

void MemTagDump(void) {
    if (!g_memtag_initialized) {
        debug_printf("[MEMTAG] not initialized\n");
        return;
    }
    MemTagRegistryDump(&g_tag_registry);
    MemRegionRegistryDump(&g_region_registry, &g_tag_registry);
    MemTagBitmapDump(&g_bitmap_index);
}



static void StampPteRegion(vmm_context_t *ctx, uintptr_t va_base,
                            uint32_t pages, uint32_t region_id,
                            uint8_t page_class);

error_t MemRegionAttachCabin(uint32_t region_id, void *ctx,
                              uintptr_t va_base, uint32_t pages,
                              uint8_t page_class, uint64_t orig_flags) {
    if (!g_memtag_initialized) return ERR_NOT_INITIALIZED;
    error_t e = MemRegionRegistryAttach(&g_region_registry, region_id, ctx,
                                         va_base, pages, page_class, orig_flags);
    if (e == OK) {
        StampPteRegion((vmm_context_t *)ctx, va_base, pages,
                        region_id, page_class);
        uint8_t pkey = MemRegionEffectivePkey(region_id);
        (void)StampPtePkeyRange((vmm_context_t *)ctx, va_base, pages,
                                 pkey, page_class);
    }
    return e;
}

error_t MemRegionDetachCabin(uint32_t region_id, void *ctx, uintptr_t va_base) {
    if (!g_memtag_initialized) return ERR_NOT_INITIALIZED;
    return MemRegionRegistryDetach(&g_region_registry, region_id, ctx, va_base);
}

size_t MemTagDetachAllForCabin(void *ctx) {
    if (!g_memtag_initialized || !ctx) return 0;
    return MemRegionRegistryDetachAllForCtx(&g_region_registry, ctx);
}


static void StampPteRegion(vmm_context_t *ctx, uintptr_t va_base,
                            uint32_t pages, uint32_t region_id,
                            uint8_t page_class) {
    if (!ctx || !pages) return;

    uint64_t encoded = ((uint64_t)(region_id & MEMTAG_PTE_REGION_MAX))
                       << MEMTAG_PTE_REGION_SHIFT;
    bool is_2m = (page_class == MEMTAG_ATTACH_CLASS_2M);
    uint32_t step = is_2m ? VMM_LARGE_PAGE_2M_PAGES : 1u;

    for (uint32_t p = 0; p < pages; p += step) {
        uintptr_t va = va_base + (uint64_t)p * PMM_PAGE_SIZE;
        uint8_t level = 0;
        pte_t *pte = vmm_get_leaf_pte(ctx, va, &level);
        if (!pte) continue;
        if (is_2m  && level != 2) continue;
        if (!is_2m && level != 1) continue;

        pte_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
        for (;;) {
            pte_t new_val = (old & ~MEMTAG_PTE_REGION_MASK) | encoded;
            if (new_val == old) break;
            if (__atomic_compare_exchange_n(pte, &old, new_val, false,
                                             __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE)) {
                break;
            }
        }
    }
}

static bool ClearPresentLeaf(vmm_context_t *ctx, uintptr_t va) {
    uint8_t level = 0;
    pte_t *pte = vmm_get_leaf_pte(ctx, va, &level);
    if (!pte) return false;

    pte_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    while (old & VMM_FLAG_PRESENT) {
        pte_t new_val = old & ~VMM_FLAG_PRESENT;
        if (__atomic_compare_exchange_n(pte, &old, new_val, false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            return true;
        }
    }
    return false;
}

static bool RestorePresentLeaf(vmm_context_t *ctx, uintptr_t va,
                                uint64_t orig_flags, bool is_2m) {
    uint8_t level = 0;
    pte_t *pte = vmm_get_leaf_pte(ctx, va, &level);
    if (!pte) return false;
    if (is_2m && level != 2) return false;
    if (!is_2m && level != 1) return false;

    pte_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    for (;;) {
        if (old & VMM_FLAG_PRESENT) return false;
        uintptr_t phys_bits = old & vmm_get_addr_mask();
        if (phys_bits == 0) return false;
        pte_t new_val = phys_bits |
                         (old & MEMTAG_PTE_REGION_MASK) |
                         (orig_flags & VMM_PTE_FLAGS_MASK) |
                         VMM_FLAG_PRESENT;
        if (is_2m) new_val |= VMM_FLAG_LARGE_PAGE;
        if (__atomic_compare_exchange_n(pte, &old, new_val, false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            return true;
        }
    }
}

static bool ClearAttachRange(const MemRegionAttach *att) {
    vmm_context_t *ctx = (vmm_context_t *)att->ctx;
    if (!ctx) return false;

    bool any_toggled = false;
    if (att->page_class == MEMTAG_ATTACH_CLASS_2M) {
        for (uint32_t p = 0; p < att->pages; p += VMM_LARGE_PAGE_2M_PAGES) {
            uintptr_t va = att->va_base + (uint64_t)p * PMM_PAGE_SIZE;
            if (ClearPresentLeaf(ctx, va)) any_toggled = true;
        }
    } else {
        for (uint32_t p = 0; p < att->pages; p++) {
            uintptr_t va = att->va_base + (uint64_t)p * PMM_PAGE_SIZE;
            if (ClearPresentLeaf(ctx, va)) any_toggled = true;
        }
    }
    if (any_toggled) {
        vmm_shootdown_pages(ctx, att->va_base, (size_t)att->pages);
    }
    return any_toggled;
}

static bool RestoreAttachRange(const MemRegionAttach *att) {
    vmm_context_t *ctx = (vmm_context_t *)att->ctx;
    if (!ctx) return false;

    bool any_toggled = false;
    bool is_2m = (att->page_class == MEMTAG_ATTACH_CLASS_2M);
    uint32_t step = is_2m ? VMM_LARGE_PAGE_2M_PAGES : 1;
    for (uint32_t p = 0; p < att->pages; p += step) {
        uintptr_t va = att->va_base + (uint64_t)p * PMM_PAGE_SIZE;
        if (RestorePresentLeaf(ctx, va, att->orig_flags, is_2m)) any_toggled = true;
    }
    if (any_toggled) {
        vmm_shootdown_pages(ctx, att->va_base, (size_t)att->pages);
    }
    return any_toggled;
}


static bool CabinSatisfiesRegion(const uint64_t *mask, uint32_t region_id) {
    uint16_t guards[MEMTAG_GUARDS_PER_REGION];
    size_t ng = GuardsOnRegion(region_id, guards, MEMTAG_GUARDS_PER_REGION);
    if (ng == 0) return true;
    if (!mask) return false;
    for (size_t i = 0; i < ng; i++) {
        if (!MaskHas(mask, guards[i])) return false;
    }
    return true;
}

static size_t EnforceForCabinTag(vmm_context_t *target_ctx,
                                  const uint64_t *mask,
                                  uint16_t tag_id, bool revoke) {
    if (!target_ctx) return 0;

    uint32_t region_ids[MEMTAG_ENFORCE_REGIONS_PER_CALL];
    size_t nr = MemTagBitmapQueryAnd(&g_bitmap_index, &tag_id, 1,
                                      region_ids,
                                      MEMTAG_ENFORCE_REGIONS_PER_CALL);
    if (nr == MEMTAG_ENFORCE_REGIONS_PER_CALL) {
        debug_printf("[MEMTAG] WARN: EnforceForCabinTag hit region cap "
                     "(%u) for tag_id=%u — bump "
                     "MEMTAG_ENFORCE_REGIONS_PER_CALL\n",
                     (unsigned)MEMTAG_ENFORCE_REGIONS_PER_CALL,
                     (unsigned)tag_id);
    }
    if (nr == 0) return 0;

    size_t transitioned = 0;
    MemRegionAttach snap[MEMTAG_ENFORCE_ATTACHES_PER_REGION];
    for (size_t i = 0; i < nr; i++) {
        uint32_t rid = region_ids[i];
        size_t ns = MemRegionRegistrySnapshotAttachs(&g_region_registry,
                                                      rid, snap,
                                                      MEMTAG_ENFORCE_ATTACHES_PER_REGION);
        for (size_t j = 0; j < ns; j++) {
            MemRegionAttach *att = &snap[j];
            if ((vmm_context_t *)att->ctx != target_ctx) continue;

            if (revoke) {
                if (att->state != MEMTAG_ATTACH_ACTIVE) continue;
                if (CabinSatisfiesRegion(mask, rid)) continue;
                if (ClearAttachRange(att)) transitioned++;
                MemRegionRegistrySetAttachState(&g_region_registry, rid,
                                                 att->ctx, att->va_base,
                                                 MEMTAG_ATTACH_REVOKED);
            } else {
                if (att->state != MEMTAG_ATTACH_REVOKED) continue;
                if (!CabinSatisfiesRegion(mask, rid)) continue;
                if (RestoreAttachRange(att)) transitioned++;
                MemRegionRegistrySetAttachState(&g_region_registry, rid,
                                                 att->ctx, att->va_base,
                                                 MEMTAG_ATTACH_ACTIVE);
            }
        }
    }
    return transitioned;
}

size_t MemCabinEnforceRevokePost(uint32_t pid, uint16_t tag_id) {
    if (!g_memtag_initialized) return 0;
    if (tag_id == MEMTAG_INVALID_TAG_ID) return 0;
    if (!MemTagRegistryIsGuard(&g_tag_registry, tag_id)) return 0;

    process_t *proc = process_find_ref(pid);
    if (!proc) return 0;
    vmm_context_t *ctx = (vmm_context_t *)process_get_cabin(proc);
    uint64_t *mask = process_active_memtags(proc);
    uint64_t mask_snap[MEMTAG_MASK_WORDS];
    SnapshotMask(mask, mask_snap);
    size_t n = ctx ? EnforceForCabinTag(ctx, mask_snap, tag_id, true) : 0;
    process_ref_dec(proc);
    return n;
}

size_t MemCabinEnforceGrantPost(uint32_t pid, uint16_t tag_id) {
    if (!g_memtag_initialized) return 0;
    if (tag_id == MEMTAG_INVALID_TAG_ID) return 0;
    if (!MemTagRegistryIsGuard(&g_tag_registry, tag_id)) return 0;

    process_t *proc = process_find_ref(pid);
    if (!proc) return 0;
    vmm_context_t *ctx = (vmm_context_t *)process_get_cabin(proc);
    uint64_t *mask = process_active_memtags(proc);
    uint64_t mask_snap[MEMTAG_MASK_WORDS];
    SnapshotMask(mask, mask_snap);
    size_t n = ctx ? EnforceForCabinTag(ctx, mask_snap, tag_id, false) : 0;
    process_ref_dec(proc);
    return n;
}


extern uint32_t process_snapshot_pids(uint32_t *out, uint32_t max);

size_t MemTagSweepGuard(uint16_t tag_id, bool guard_on) {
    if (!g_memtag_initialized) return 0;
    if (tag_id == MEMTAG_INVALID_TAG_ID) return 0;

    uint32_t pids[MEMTAG_SWEEP_PIDS_PER_CALL];
    uint32_t n = process_snapshot_pids(pids, MEMTAG_SWEEP_PIDS_PER_CALL);

    size_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        process_t *proc = process_find_ref(pids[i]);
        if (!proc) continue;
        vmm_context_t *ctx = (vmm_context_t *)process_get_cabin(proc);
        uint64_t *mask = process_active_memtags(proc);
        uint64_t mask_snap[MEMTAG_MASK_WORDS];
        SnapshotMask(mask, mask_snap);
        if (ctx) total += EnforceForCabinTag(ctx, mask_snap, tag_id, guard_on);
        process_ref_dec(proc);
    }
    return total;
}


static volatile uint64_t g_pku_stamp_count    = 0;
static volatile uint64_t g_pku_conflict_count = 0;
static volatile uint64_t g_pku_sweep_count    = 0;

static void ResolvePkuTagIdsIfNeeded(void) {
    if (g_pku_tag_ids_resolved) return;
    static const char *const PKU_STRS[MEMTAG_PKU_MAX_KEYS] = {
        "pku:0",  "pku:1",  "pku:2",  "pku:3",
        "pku:4",  "pku:5",  "pku:6",  "pku:7",
        "pku:8",  "pku:9",  "pku:10", "pku:11",
        "pku:12", "pku:13", "pku:14", "pku:15",
    };
    for (uint32_t i = 0; i < MEMTAG_PKU_MAX_KEYS; i++) {
        g_pku_tag_ids[i] = MemTagRegistryLookupStr(&g_tag_registry, PKU_STRS[i]);
    }
    __atomic_store_n(&g_pku_tag_ids_resolved, true, __ATOMIC_RELEASE);
}

static int PkuValueFromTagId(uint16_t tag_id) {
    if (tag_id == MEMTAG_INVALID_TAG_ID) return -1;
    ResolvePkuTagIdsIfNeeded();
    for (uint32_t i = 0; i < MEMTAG_PKU_MAX_KEYS; i++) {
        if (g_pku_tag_ids[i] == tag_id) return (int)i;
    }
    return -1;
}

static bool StampPtePkeyLeaf(vmm_context_t *ctx, uintptr_t va,
                              uint8_t pkey, bool is_2m) {
    uint8_t level = 0;
    pte_t *pte = vmm_get_leaf_pte(ctx, va, &level);
    if (!pte) return false;
    if (is_2m  && level != 2) return false;
    if (!is_2m && level != 1) return false;

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    bool cet_active = (cr4 & (1ULL << 23)) != 0;

    uint64_t new_pkey_field = vmm_pte_encode_pkey(pkey);
    pte_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    for (;;) {
        pte_t new_val = (old & ~VMM_PTE_PKEY_MASK) | new_pkey_field;
        if (cet_active && vmm_pte_pkey_cet_conflict(new_val)) {
            __atomic_fetch_add(&g_pku_conflict_count, 1, __ATOMIC_RELAXED);
            return false;
        }
        if (new_val == old) return true;
        if (__atomic_compare_exchange_n(pte, &old, new_val, false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            __atomic_fetch_add(&g_pku_stamp_count, 1, __ATOMIC_RELAXED);
            return true;
        }
    }
}

static size_t StampPtePkeyRange(vmm_context_t *ctx, uintptr_t va_base,
                                 uint32_t pages, uint8_t pkey,
                                 uint8_t page_class) {
    if (!ctx || !pages) return 0;
    bool is_2m = (page_class == MEMTAG_ATTACH_CLASS_2M);
    uint32_t step = is_2m ? VMM_LARGE_PAGE_2M_PAGES : 1u;
    size_t toggled = 0;
    for (uint32_t p = 0; p < pages; p += step) {
        uintptr_t va = va_base + (uint64_t)p * PMM_PAGE_SIZE;
        if (StampPtePkeyLeaf(ctx, va, pkey, is_2m)) toggled++;
    }
    return toggled;
}

uint8_t MemRegionEffectivePkey(uint32_t region_id) {
    if (!g_memtag_initialized) return 0;
    uint16_t buf[MEMTAG_ENFORCE_ATTACHES_PER_REGION];
    size_t n = MemRegionRegistryListTags(&g_region_registry, region_id,
                                          buf, sizeof(buf)/sizeof(buf[0]));
    for (size_t i = 0; i < n; i++) {
        int v = PkuValueFromTagId(buf[i]);
        if (v >= 0) return (uint8_t)v;
    }
    return 0;
}

size_t MemTagSweepPkey(uint32_t region_id, uint8_t new_pkey) {
    if (!g_memtag_initialized) return 0;
    if (region_id == MEMTAG_INVALID_REGION_ID) return 0;
    if (new_pkey > VMM_PTE_PKEY_MAX) return 0;

    MemRegionAttach snap[MEMTAG_ENFORCE_ATTACHES_PER_REGION];
    size_t n = MemRegionRegistrySnapshotAttachs(&g_region_registry,
                                                  region_id, snap,
                                                  MEMTAG_ENFORCE_ATTACHES_PER_REGION);
    if (n == 0) return 0;

    __atomic_fetch_add(&g_pku_sweep_count, 1, __ATOMIC_RELAXED);

    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        MemRegionAttach *att = &snap[i];
        if (!att->ctx) continue;
        size_t s = StampPtePkeyRange((vmm_context_t *)att->ctx,
                                      att->va_base, att->pages,
                                      new_pkey, att->page_class);
        if (s) {
            vmm_shootdown_pages((vmm_context_t *)att->ctx,
                                 att->va_base, (size_t)att->pages);
            total += s;
        }
    }
    return total;
}

error_t MemTagApplyPkey(uint32_t region_id, uint8_t pkey) {
    if (!g_memtag_initialized) return ERR_NOT_INITIALIZED;
    if (region_id == MEMTAG_INVALID_REGION_ID) return ERR_INVALID_ARGUMENT;
    if (pkey >= MEMTAG_PKU_MAX_KEYS) return ERR_INVALID_ARGUMENT;

    uint16_t buf[MEMTAG_ENFORCE_ATTACHES_PER_REGION];
    size_t   n = MemRegionRegistryListTags(&g_region_registry, region_id,
                                            buf, sizeof(buf)/sizeof(buf[0]));
    for (size_t i = 0; i < n; i++) {
        int v = PkuValueFromTagId(buf[i]);
        if (v < 0) continue;
        if (v == (int)pkey) {
            (void)MemTagSweepPkey(region_id, pkey);
            return OK;
        }
        (void)MemRegionRegistryRemoveTag(&g_region_registry, region_id, buf[i]);
        MemTagBitmapClear(&g_bitmap_index, buf[i], region_id);
        MemTagRegistryRefDec(&g_tag_registry, buf[i]);
        PublishTagEvent("memtag:tag:cleared", region_id, buf[i]);
    }

    if (pkey == 0) {
        (void)MemTagSweepPkey(region_id, 0);
        return OK;
    }

    char tag_buf[8];
    const char *digits = "0123456789";
    size_t pos = 0;
    tag_buf[pos++] = 'p';
    tag_buf[pos++] = 'k';
    tag_buf[pos++] = 'u';
    tag_buf[pos++] = ':';
    if (pkey >= 10) {
        tag_buf[pos++] = digits[pkey / 10];
        tag_buf[pos++] = digits[pkey % 10];
    } else {
        tag_buf[pos++] = digits[pkey];
    }
    tag_buf[pos] = 0;

    error_t e = MemTagApply(region_id, tag_buf);
    if (e != OK) return e;
    return OK;
}


uint32_t MemRegionFromPte(uint64_t pte_value, uintptr_t phys) {
    if (!g_memtag_initialized) return MEMTAG_INVALID_REGION_ID;

    uint32_t encoded = (uint32_t)((pte_value & MEMTAG_PTE_REGION_MASK)
                                  >> MEMTAG_PTE_REGION_SHIFT);

    MemRegion *r = MemRegionRegistrySlot(&g_region_registry, encoded);
    if (r && (r->flags & MEMTAG_REGION_FLAG_ACTIVE) && r->base_phys) {
        uintptr_t end = r->base_phys + (uintptr_t)r->pages * PMM_PAGE_SIZE;
        if (phys >= r->base_phys && phys < end) {
            return encoded;
        }
    }
    return MemRegionRegistryFromPhys(&g_region_registry, phys);
}

bool MemTagVerifyPteMetadataBits(void) { return vmm_verify_pte_metadata_bits_52_58(); }

#if 0
bool MemTagVerifyPteMetadataBits_OLD(void) {
    extern uint8_t vmm_maxphyaddr;

    if (vmm_maxphyaddr > 52) {
        debug_printf("[MEMTAG] M5 ABORT: MAXPHYADDR=%u > 52 — "
                     "bits 52-58 are phys, NOT ignored\n",
                     (unsigned)vmm_maxphyaddr);
        return false;
    }

    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    bool cr4_pke = ((cr4 >> 22) & 1u) != 0;
    bool cr4_pks = ((cr4 >> 24) & 1u) != 0;
    bool cr4_cet = ((cr4 >> 23) & 1u) != 0;

    debug_printf("[MEMTAG] M5 PTE-metadata probe: MAXPHYADDR=%u "
                 "CR4.PKE=%d CR4.PKS=%d CR4.CET=%d → bits 52-58 SAFE\n",
                 (unsigned)vmm_maxphyaddr,
                 (int)cr4_pke, (int)cr4_pks, (int)cr4_cet);
    return true;
}
#endif

bool MemTagVerifyPatMsr(void)  { return vmm_verify_pat_msr(); }
void MemTagDumpMtrrLayout(void) { vmm_dump_mtrr_layout(); }

#if 0
bool MemTagVerifyPatMsr_OLD(void) {
    if (!g_cpu_caps.has_pat) {
        debug_printf("[MEMTAG] PAT MSR verify SKIP: CPU lacks PAT (has_pat=0)\n");
        return true;
    }

    uint64_t bsp = vmm_get_pat_msr_value();
    if (bsp == 0) {
        debug_printf("[MEMTAG] PAT MSR verify SKIP: BSP snapshot not yet captured\n");
        return true;
    }

    uint64_t local = MtagRdmsr(MEMTAG_MSR_IA32_PAT);
    if (local != bsp) {
        debug_printf("[MEMTAG] PAT MSR ABORT: local=0x%016lx bsp=0x%016lx "
                     "— Intel SDM Vol 3A §11.12.4 violation (heterogeneous "
                     "PAT across coherent CPUs)\n",
                     (unsigned long)local, (unsigned long)bsp);
        return false;
    }

    debug_printf("[MEMTAG] PAT MSR verify OK: 0x%016lx (matches BSP)\n",
                 (unsigned long)local);
    return true;
}

static const char *MtrrTypeStr(uint8_t t) {
    switch (t) {
        case 0x00: return "UC";
        case 0x01: return "WC";
        case 0x04: return "WT";
        case 0x05: return "WP";
        case 0x06: return "WB";
        default:   return NULL;
    }
}

void MemTagDumpMtrrLayout(void) {
    if (!g_cpu_caps.has_pat) {
        debug_printf("[MEMTAG] MTRR audit SKIP: CPU lacks PAT/MSRs\n");
        return;
    }

    uint64_t cap = MtagRdmsr(MEMTAG_MSR_IA32_MTRRCAP);
    uint64_t def = MtagRdmsr(MEMTAG_MSR_MTRR_DEF_TYPE);

    uint8_t  vcnt    = (uint8_t)(cap & 0xFFu);
    bool     fix_sup = (cap >> 8) & 1u;
    bool     wc_sup  = (cap >> 10) & 1u;
    bool     smrr    = (cap >> 11) & 1u;
    uint8_t  def_typ = (uint8_t)(def & 0xFFu);
    bool     fix_en  = (def >> 10) & 1u;
    bool     mtrr_en = (def >> 11) & 1u;
    const char *def_str = MtrrTypeStr(def_typ);

    debug_printf("[MEMTAG] MTRR audit: cap=0x%016lx def=0x%016lx VCNT=%u "
                 "FIX_sup=%d WC_sup=%d SMRR=%d FIX_en=%d MTRR_en=%d "
                 "DEF_TYPE=%s(0x%02x)\n",
                 (unsigned long)cap, (unsigned long)def,
                 (unsigned)vcnt, (int)fix_sup, (int)wc_sup, (int)smrr,
                 (int)fix_en, (int)mtrr_en,
                 def_str ? def_str : "RESERVED", (unsigned)def_typ);

    if (!mtrr_en) {
        debug_printf("[MEMTAG] MTRR audit WARN: MTRRs DISABLED — every "
                     "range falls back to UC per Intel SDM §11.11.2.1 "
                     "(MTRR_DEF_TYPE.E=0). Firmware misconfig.\n");
    }
    if (def_typ != 0x06u && mtrr_en) {
        debug_printf("[MEMTAG] MTRR audit WARN: DEF_TYPE=%s — non-WB "
                     "default means PAT cache:wb tags may be silently "
                     "demoted (Intel SDM §11.12.5 combination table).\n",
                     def_str ? def_str : "RESERVED");
    }

    uint32_t walk_n = vcnt > 8 ? 8 : vcnt;
    for (uint32_t i = 0; i < walk_n; i++) {
        uint64_t base = MtagRdmsr(MEMTAG_MSR_MTRR_PHYSBASE0 + i * 2);
        uint64_t mask = MtagRdmsr(MEMTAG_MSR_MTRR_PHYSMASK0 + i * 2);
        if (!((mask >> 11) & 1u)) continue;
        uint8_t  ty   = (uint8_t)(base & 0xFFu);
        uint64_t phys = base & ~0xFFFULL;
        uint64_t mphys = mask & ~0xFFFULL;
        const char *ts = MtrrTypeStr(ty);
        debug_printf("[MEMTAG]   MTRR var[%u]: base=0x%016lx mask=0x%016lx "
                     "type=%s(0x%02x)\n",
                     i, (unsigned long)phys, (unsigned long)mphys,
                     ts ? ts : "RESERVED", (unsigned)ty);
    }
}
#endif