/*
 * MemTag — Public API + PMM Integration
 *
 * Orchestrates tag_registry + region_registry + tag_bitmap. Boot-seeds
 * reserved tag namespace and applies zone tags to E820/EFI memory map.
 * Provides the PMM integration entry points called from pmm.c.
 *
 * Lifecycle order (BoxOS boot):
 *   1. pmm_init   (sets up buddy allocator + mem_end)
 *   2. mem_init   (kernel heap kmalloc)
 *   3. vmm_init   (page tables + Pull Map)
 *   4. MemTagInit (this file) — needs PMM, kmalloc, vmm_phys_to_virt
 *   5. ...everything else...
 *
 * Until MemTagInit completes, pmm_alloc(tag) is silently un-tagged (region
 * not registered). After init, pmm_alloc(tag) registers a region atomically.
 */

#include "memtag.h"
#include "pmm.h"
#include "vmm.h"
#include "e820.h"
#include "boot_info.h"
#include "kernel_config.h"
#include "touch.h"
#include "cpuid.h"     /* g_cpu_caps.has_pat — Phase 2E PAT/MTRR probes */

/* Touch event payloads. Userspace subscribers (`touch_claim("memtag:region:added")`)
 * receive these via the normal Touch delivery path. */
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

/* Touch publish gate. Touch resolves tags against TagFS state — until TagFS
 * is up, publishes silently no-op. Tracking a flag here lets us SKIP the
 * resolution overhead entirely during boot zone-seeding (called before TagFS
 * exists; would just churn TouchTagResolve only to return INVALID). */
static volatile bool g_touch_publish_enabled = false;

/* ─── Global state ───────────────────────────────────────────────────── */

static MemTagRegistry      g_tag_registry;
static MemRegionRegistry   g_region_registry;
static MemTagBitmapIndex   g_bitmap_index;
static bool                g_memtag_initialized = false;

/* Phase 2H+ — pku:0..pku:15 tag ID cache. Populated lazily on first
 * MemRegionEffectivePkey call (or eagerly post-SeedReservedTags via
 * ResolvePkuTagIds). Without this cache, every attach would re-parse
 * "pku:N" strings to look up tag IDs — overhead on a hot path. */
#define MEMTAG_PKU_MAX_KEYS   16u
static uint16_t            g_pku_tag_ids[MEMTAG_PKU_MAX_KEYS];
static bool                g_pku_tag_ids_resolved = false;

/* Forward decls — Phase 2H+ PKU stamping. Defined under the Phase 2H+
 * block at the bottom of the PTE-manipulation section. MemRegionAttachCabin
 * + MemTagApply / MemTagClear (which appear earlier in the file) reference
 * these. */
static int    PkuValueFromTagId(uint16_t tag_id);
static size_t StampPtePkeyRange(vmm_context_t *ctx, uintptr_t va_base,
                                 uint32_t pages, uint8_t pkey,
                                 uint8_t page_class);

bool MemTagIsInitialized(void) { return g_memtag_initialized; }

MemTagRegistry    *MemTagGetRegistry(void)       { return &g_tag_registry; }
MemRegionRegistry *MemTagGetRegionRegistry(void) { return &g_region_registry; }
MemTagBitmapIndex *MemTagGetBitmap(void)         { return &g_bitmap_index; }

/* ─── Touch publish helpers ───────────────────────────────────────── */

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

/* ─── Boot-time zone seeding ───────────────────────────────────────── */

/* Reserved-tag interning helper. The kernel marks well-known boot tags as
 * MEMTAG_FLAG_KERNEL_RESERVED so a future userspace Pocket op surface
 * can reject user-side intern of these prefixes. Phase 1 just sets the
 * flag; enforcement comes with the Pocket surface in a follow-up. */
static void MarkInternReserved(const char *kv) {
    uint16_t id = MemTagRegistryInternStr(&g_tag_registry, kv);
    if (id != MEMTAG_INVALID_TAG_ID) {
        MemTagRegistryMarkReserved(&g_tag_registry, id);
    }
}

/* Phase 2E — apply derived cache type tag to a region directly through
 * the tag registry + bitmap (bypass tag-interning loop and the bucket
 * fast-path of MemTagApply). Used during boot zone-seeding where we
 * have the region_id in hand and want to avoid the find-by-phys step. */
static void SeedRegionTag(uint32_t region_id, const char *tag) {
    uint16_t tid = MemTagRegistryInternStr(&g_tag_registry, tag);
    if (tid == MEMTAG_INVALID_TAG_ID) return;
    if (MemRegionRegistryAddTag(&g_region_registry, region_id, tid) == OK) {
        MemTagBitmapSet(&g_bitmap_index, tid, region_id);
    }
}

/* Boot tag for a contiguous E820 USABLE range based on phys address.
 * Returns the zone-string for this range (or NULL if MMIO/reserved). */
static const char *ZoneTagForPhys(uintptr_t base) {
    if (base < (uintptr_t)CONFIG_PHYS_ZONE_DMA32_END)  return "zone:dma32";
    if (base < (uintptr_t)CONFIG_PHYS_ZONE_USER_END)   return "zone:user";
    return "zone:high";
}

static void SeedReservedTags(void) {
    /* Force-intern the well-known IDs upfront so subsequent lookups
     * never miss. Order is deterministic — these get IDs 0..N. */
    MarkInternReserved("zone:dma32");
    MarkInternReserved("zone:user");
    MarkInternReserved("zone:high");
    MarkInternReserved("purpose:kernel");
    MarkInternReserved("purpose:mmio");
    MarkInternReserved("purpose:shared");
    MarkInternReserved("purpose:firmware");
    MarkInternReserved("pinned:true");
    /* Phase 2E — cache type namespace (Intel SDM Vol 3A §11.12 memory
     * types). Reserved so userspace can't shadow them. Applied
     * automatically by SeedZoneRegions / vmm_map_mmio / vmm_map_framebuffer. */
    MarkInternReserved("cache:wb");
    MarkInternReserved("cache:wt");
    MarkInternReserved("cache:uc");
    MarkInternReserved("cache:uc-");
    MarkInternReserved("cache:wc");
    MarkInternReserved("cache:wp");
    /* Phase 2F — Machine Check Architecture (Intel SDM Vol 3B §15)
     * event + state namespace. Reserved so userspace can't shadow.
     * mce:poisoned auto-applied by mce_handle() on the page from
     * IA32_MC<i>_ADDR; PMM also tracks it in pmm_poisoned_bitmap so the
     * allocator refuses to hand the page back out. */
    MarkInternReserved("mce:poisoned");
    MarkInternReserved("mce:corrected");
    MarkInternReserved("mce:fault:detected");
    MarkInternReserved("mce:fault:recovered");
    MarkInternReserved("mce:fault:fatal");
    /* MCE page migration (mce_migrate.c) — phys → MemRegion attach
     * chain → per-cabin PTE swap with TLB shootdown. Published from
     * K-Core context (the worker runs via irq_defer, not on the IST
     * stack — so TouchPublishId is OK; mce.c's IST-only tags above
     * use TouchPublishIrqPair). */
    MarkInternReserved("mce:migration:completed");
    MarkInternReserved("mce:migration:failed");
    MarkInternReserved("mce:migration:unmapped");
    /* APEI/GHES runtime — firmware-side error delivery. Memory section
     * events route through the same mce_migrate pipeline as MSR-bank
     * #MC events; other sections (Processor / PCIe / Generic) publish
     * their own tags so a userspace logger can subscribe selectively. */
    MarkInternReserved("apei:ghes:ready");
    MarkInternReserved("apei:memory:error");
    MarkInternReserved("apei:processor:error");
    MarkInternReserved("apei:pcie:error");
    MarkInternReserved("apei:generic:error");
    /* Phase 2K+ — CET lifecycle. cet:fault:cp already reserved by
     * Phase 2K. cet:enabled / shstk:enabled / ibt:enabled fire from
     * cet_lifecycle_init_bsp after CR4.CET + IA32_S_CET/IA32_U_CET
     * are programmed. */
    MarkInternReserved("cet:enabled");
    MarkInternReserved("shstk:enabled");
    MarkInternReserved("ibt:enabled");
    /* Phase 2G — IOMMU lifecycle namespace. Auto-applied by the IOMMU
     * wrapper (iommu_map / iommu_device_attach) so subscribers can use
     * the bitmap inverted index to enumerate every DMA buffer routed
     * through a specific domain or device. The per-domain string
     * `iommu:domain:N` is interned lazily on first map. */
    MarkInternReserved("iommu:ready");
    MarkInternReserved("iommu:fault");
    MarkInternReserved("iommu:dma:mapped");
    MarkInternReserved("iommu:dma:unmapped");
    MarkInternReserved("iommu:domain:attached");
    /* Phase 2H — Protection Keys (PKU/PKS) namespace. PTE bits 62:59
     * encode a 4-bit PKEY (0..15). Per-process PKRU/PKRS gate access.
     * Userspace policy assigns specific tags to specific keys; the
     * stamping mirrors Phase 2D's StampPteRegion shape. pku:ready /
     * pku:fault:denied are lifecycle events; pku:0..pku:15 are the
     * explicit key bindings. */
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
    /* Phase 2I — LAM (Linear Address Masking) namespace. lam:ready /
     * lam:enabled:u48 / lam:enabled:u57 are lifecycle events (BSP+AP
     * probe publishes via Touch once per-process opt-in lands).
     * lam:fault:tag is the future #GP/#PF event when a tagged pointer
     * fails a memory check the OS layers on top (Phase 2I infra; the
     * check itself comes later). */
    MarkInternReserved("lam:ready");
    MarkInternReserved("lam:enabled:u48");
    MarkInternReserved("lam:enabled:u57");
    MarkInternReserved("lam:fault:tag");
    /* Phase 2J — TME / TME-MK lifecycle + per-KeyID namespace.
     * tme:ready fires once when probe completes; tme:locked /
     * tme:active / tme:mk_active reflect the firmware-programmed
     * state. tme:keyid:N labels a region encrypted with KeyID N (used
     * by future allocate-with-keyid API). */
    MarkInternReserved("tme:ready");
    MarkInternReserved("tme:locked");
    MarkInternReserved("tme:active");
    MarkInternReserved("tme:mk_active");
    MarkInternReserved("tme:keyid:0");  MarkInternReserved("tme:keyid:1");
    MarkInternReserved("tme:keyid:2");  MarkInternReserved("tme:keyid:3");
    MarkInternReserved("tme:keyid:4");  MarkInternReserved("tme:keyid:5");
    MarkInternReserved("tme:keyid:6");  MarkInternReserved("tme:keyid:7");
    MarkInternReserved("tme:keyid:8");  MarkInternReserved("tme:keyid:9");
    MarkInternReserved("tme:keyid:10"); MarkInternReserved("tme:keyid:11");
    MarkInternReserved("tme:keyid:12"); MarkInternReserved("tme:keyid:13");
    MarkInternReserved("tme:keyid:14"); MarkInternReserved("tme:keyid:15");
    /* Phase 2K — CET (Control-flow Enforcement) namespace.
     * cet:ready signals probe completion; cet:shstk:supervisor /
     * cet:shstk:user mark a phys range stamped with PTE bit 60 / 61
     * as shadow-stack memory; cet:fault:cp captures #CP (vector 21)
     * Control-Protection Exception events for telemetry. */
    MarkInternReserved("cet:ready");
    MarkInternReserved("cet:shstk:supervisor");
    MarkInternReserved("cet:shstk:user");
    MarkInternReserved("cet:fault:cp");
    MarkInternReserved("cet:ibt:enabled");
}

/* Walk e820 entries and create one zone-descriptor region per contiguous
 * (range, classification). Zone regions are queryable via
 * MemTagQuery("zone:dma32") etc. They carry MEMTAG_REGION_FLAG_KERNEL
 * which marks them as boot-owned: MemTagPmmFreed treats this flag as
 * "do not destroy on free of a sub-allocation". */
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
            /* Split into up to 3 chunks at zone boundaries. */
            uintptr_t cursor = base;
            while (cursor < end) {
                uintptr_t chunk_end = end;
                const char *tag = ZoneTagForPhys(cursor);

                /* Cap chunk to zone boundary */
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
                        /* Phase 2E — USABLE RAM defaults to Write-Back per
                         * the BoxOS PAT layout (PA0=PA4=WB). Tag here so
                         * MemRegionFromPhys lookups carry the cache type
                         * without needing per-mapping region creation. */
                        SeedRegionTag(rid, "cache:wb");
                        zones_created++;
                    }
                }
                cursor = chunk_end;
            }
        } else {
            /* MMIO / reserved / firmware. */
            size_t pages = (end - base) / PMM_PAGE_SIZE;
            if (!pages) pages = 1;
            uint32_t rid = MemRegionRegistryCreate(
                &g_region_registry, base, 0, NULL, pages,
                MEMTAG_REGION_FLAG_KERNEL | MEMTAG_REGION_FLAG_PHYSICAL);
            if (rid != MEMTAG_INVALID_REGION_ID) {
                SeedRegionTag(rid, "purpose:mmio");
                /* Phase 2E — firmware/MMIO/reserved E820 regions are not
                 * inherently UC at the PAT level (PAT only sets PER-PTE
                 * memory type), but BoxOS only ever maps them through
                 * vmm_map_mmio → PAT index 3 = UC. Tag the underlying
                 * phys range so any debug query reports the intended
                 * cache type. The MMIO-VA region created by vmm_map_mmio
                 * (different region_id) ALSO carries cache:uc — they
                 * agree, but for different reasons. */
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
                /* Phase 2E — kernel image (.text/.rodata/.data/.bss)
                 * maps WB via VMM_FLAGS_KERNEL_* with PAT index 0. */
                SeedRegionTag(rid, "cache:wb");
            }
        }
    }

    debug_printf("[MEMTAG] Seeded %zu zone regions + %zu MMIO regions\n",
                 zones_created, mmio_created);
}

/* ─── Init ──────────────────────────────────────────────────────────── */

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

/* ─── Tag interning facades ────────────────────────────────────────── */

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

/* ─── Region lifecycle ─────────────────────────────────────────────── */

/* Apply HW-derived tags to a freshly-created region. NUMA is the only
 * source available in Phase 1; PAT cacheability and PKU/LAM/MCE join in
 * Phase 2 when their drivers feed back into MemTag. */
static void ApplyDerivedTags(uint32_t region_id, uintptr_t base_phys) {
    if (!base_phys) return;
    uint32_t domain = pmm_phys_domain(base_phys);
    if (domain == 0xFFFFFFFFu) return;  /* SRAT unknown / UMA system */
    char buf[32];
    ksnprintf(buf, sizeof(buf), "numa:domain:%u", domain);
    /* Direct registry+bitmap to skip the Touch publish — derived tags
     * are noisy and subscribers should observe the region-added event
     * which carries enough info to derive the domain themselves. */
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

    /* Publish BEFORE destruction so subscribers can snapshot the region. */
    PublishRegionEvent("memtag:region:released", region_id);

    /* Decrement ref-count + clear bitmap for every tag this region holds.
     * RemoveTag pops the FRONT slot each iteration, so we always re-read
     * from index 0 — handles arbitrary tag count without per-batch limit. */
    uint16_t tag_id;
    while (MemRegionRegistryListTags(&g_region_registry, region_id, &tag_id, 1) == 1) {
        if (MemRegionRegistryRemoveTag(&g_region_registry, region_id, tag_id) != OK)
            break;  /* race: someone else cleared concurrently */
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
    uintptr_t phys = vmm_virt_to_phys(vctx, virt);
    if (!phys) return MEMTAG_INVALID_REGION_ID;
    return MemRegionFromPhys(phys);
}

/* ─── Tag apply/clear ──────────────────────────────────────────────── */

error_t MemTagApply(uint32_t region_id, const char *tag_str) {
    if (!g_memtag_initialized || !tag_str)
        return ERR_INVALID_ARGUMENT;
    if (region_id == MEMTAG_INVALID_REGION_ID)
        return ERR_INVALID_ARGUMENT;

    uint16_t tid = MemTagRegistryInternStr(&g_tag_registry, tag_str);
    if (tid == MEMTAG_INVALID_TAG_ID) return ERR_NO_MEMORY;

    error_t e = MemRegionRegistryAddTag(&g_region_registry, region_id, tid);
    if (e != OK) return e;

    /* Bitmap mirrors region.tag_ids. Failure here would leave bitmap stale
     * vs region — best effort. */
    MemTagBitmapSet(&g_bitmap_index, tid, region_id);
    MemTagRegistryRefInc(&g_tag_registry, tid);
    PublishTagEvent("memtag:tag:applied", region_id, tid);

    /* Phase 2H+ — if this apply added/changed a pku:N tag, sweep all
     * attaches so live PTEs reflect the new policy immediately
     * (Phase 2C-style enforcement). PkuValueFromTagId returns -1 for
     * non-pku tags → no-op. Sweep is bounded; safe to call here. */
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
    if (tid == MEMTAG_INVALID_TAG_ID) return OK;  /* never interned */

    error_t e = MemRegionRegistryRemoveTag(&g_region_registry, region_id, tid);
    if (e == OK) {
        MemTagBitmapClear(&g_bitmap_index, tid, region_id);
        MemTagRegistryRefDec(&g_tag_registry, tid);
        PublishTagEvent("memtag:tag:cleared", region_id, tid);

        /* Phase 2H+ — if a pku:N tag was cleared, sweep PTEs back to
         * the new effective key (which may be 0 = no pku, or another
         * pku tag still applied to the same region). */
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

    /* Look up existing region. If one starts EXACTLY at base_phys with the
     * SAME length, reuse it; otherwise create a fresh region. (Don't reuse
     * a partial match — distinct ranges deserve distinct region_ids.) */
    uint32_t rid = MemRegionFromPhys(base_phys);
    if (rid != MEMTAG_INVALID_REGION_ID) {
        MemRegion *r = MemRegionRegistrySlot(&g_region_registry, rid);
        if (r && (r->flags & MEMTAG_REGION_FLAG_ACTIVE) &&
            r->base_phys == base_phys && r->pages == pages &&
            !(r->flags & MEMTAG_REGION_FLAG_KERNEL)) {
            /* Reuse — exact match, non-zone-descriptor. */
            return MemTagApply(rid, tag_str);
        }
        /* Otherwise fall through and create a new region (may overlap a
         * zone descriptor; that's expected). */
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

/* ─── Set-algebra queries ──────────────────────────────────────────── */

/* Resolve NULL-terminated string array to tag_id buffer (returns count).
 * Unknown tags are encoded as MEMTAG_INVALID_TAG_ID — caller must reject. */
static size_t ResolveTagStrs(const char *const *strs, uint16_t *out, size_t max) {
    size_t n = 0;
    if (!strs) return 0;
    for (size_t i = 0; strs[i] != NULL && n < max; i++) {
        out[n++] = MemTagRegistryLookupStr(&g_tag_registry, strs[i]);
    }
    return n;
}

MemTagResult MemTagQueryAnd_(const char *const *tag_strs) {
    MemTagResult r = { .count = 0 };
    if (!g_memtag_initialized) return r;

    uint16_t ids[64];
    size_t n = ResolveTagStrs(tag_strs, ids, 64);
    if (n == 0) return r;
    /* If ANY required tag is unknown → AND is empty. */
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == MEMTAG_INVALID_TAG_ID) return r;
    }
    r.count = MemTagBitmapQueryAnd(&g_bitmap_index, ids, (uint16_t)n,
                                    r.region_ids,
                                    sizeof(r.region_ids) / sizeof(r.region_ids[0]));
    return r;
}

MemTagResult MemTagQueryOr_(const char *const *tag_strs) {
    MemTagResult r = { .count = 0 };
    if (!g_memtag_initialized) return r;

    uint16_t ids[64];
    size_t n = ResolveTagStrs(tag_strs, ids, 64);
    /* Unknown tags in OR-set are filtered (count toward nothing). */
    uint16_t known[64]; uint16_t kn = 0;
    for (size_t i = 0; i < n; i++)
        if (ids[i] != MEMTAG_INVALID_TAG_ID) known[kn++] = ids[i];
    if (kn == 0) return r;
    r.count = MemTagBitmapQueryOr(&g_bitmap_index, known, kn,
                                   r.region_ids,
                                   sizeof(r.region_ids) / sizeof(r.region_ids[0]));
    return r;
}

MemTagResult MemTagQueryMixed_(const char *const *required,
                                const char *const *any,
                                const char *const *excluded) {
    MemTagResult r = { .count = 0 };
    if (!g_memtag_initialized) return r;

    uint16_t req[64], an[64], ex[64];
    size_t nr = ResolveTagStrs(required, req, 64);
    size_t na = ResolveTagStrs(any,      an,  64);
    size_t ne = ResolveTagStrs(excluded, ex,  64);

    /* Any unknown REQUIRED tag → empty result. */
    for (size_t i = 0; i < nr; i++)
        if (req[i] == MEMTAG_INVALID_TAG_ID) return r;

    /* Filter unknowns from any/excluded. */
    uint16_t a2[64], e2[64]; uint16_t na2 = 0, ne2 = 0;
    for (size_t i = 0; i < na; i++)
        if (an[i] != MEMTAG_INVALID_TAG_ID) a2[na2++] = an[i];
    for (size_t i = 0; i < ne; i++)
        if (ex[i] != MEMTAG_INVALID_TAG_ID) e2[ne2++] = ex[i];

    r.count = MemTagBitmapQueryMixed(&g_bitmap_index,
                                      req, (uint16_t)nr,
                                      a2,  na2,
                                      e2,  ne2,
                                      r.region_ids,
                                      sizeof(r.region_ids)/sizeof(r.region_ids[0]));
    return r;
}

/* ─── PMM integration ──────────────────────────────────────────────── */

/* Pick the buddy zone bias from a "zone:*" tag string. Returns 0 if not a
 * zone tag (let buddy choose any zone). pmm_alloc fast-path is bit-flag
 * indexed via PHYS_TAG_DMA32 / USER / HIGH from pmm.h. */
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
    /* Walk pages; for each one that maps to a NON-zone-descriptor region,
     * destroy that region. Zone descriptors persist (boot-seeded). */
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
        if (r->flags & MEMTAG_REGION_FLAG_KERNEL) continue;  /* descriptor */
        MemRegionDestroy(rid);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Phase 2A — Enforcement infrastructure
 * ═══════════════════════════════════════════════════════════════════ */

/* Per-cabin active mask: 1024 bits = 16 words. tag_id N → word N/64 bit N%64. */
#define MEMTAG_MASK_WORDS  16
#define MEMTAG_MASK_BITS   (MEMTAG_MASK_WORDS * 64)

/* ─── Phase 2C enforcement buffer caps ─────────────────────────────── */

/* Max regions per single guard tag the enforcement walks see in one
 * EnforceForCabinTag call. Production-typical region count per tag is
 * <50 (Bay/Brook per cabin). Stack-budgeted ~1 KiB. Overflow drops the
 * tail — silent under load by design, but enforcement re-runs on every
 * Grant/Revoke/SetGuard so coverage is eventually-consistent. */
#define MEMTAG_ENFORCE_REGIONS_PER_CALL   256u

/* Max attach snapshots per region in one enforce iteration. Bay/Brook
 * typically have 1-2 attachments per region (cabin-pair max). 32 covers
 * any production-scale shared mapping with headroom. */
#define MEMTAG_ENFORCE_ATTACHES_PER_REGION  32u

/* Max guards per region the access-check considers. Region multi-guard
 * count is bounded by tag_count which is typically <8. 64 is the
 * Phase 1 query-array convention. */
#define MEMTAG_GUARDS_PER_REGION  64u

/* Max PIDs sampled in MemTagSweepGuard. Live process count in production
 * is typically <100. Stack-budgeted at 1 KiB (256 × 4 B). */
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

/* ─── Guard flag mutation ─────────────────────────────────────────── */

error_t MemTagSetGuard(const char *tag_str, bool guard) {
    if (!g_memtag_initialized || !tag_str) return ERR_INVALID_ARGUMENT;
    uint16_t tid = MemTagRegistryInternStr(&g_tag_registry, tag_str);
    if (tid == MEMTAG_INVALID_TAG_ID) return ERR_NO_MEMORY;

    bool was_guard = MemTagRegistryIsGuard(&g_tag_registry, tid);
    if (guard) MemTagRegistryMarkGuard(&g_tag_registry, tid);
    else       MemTagRegistryClearGuard(&g_tag_registry, tid);

    /* Phase 2C — if the guard state actually flipped, sweep across all
     * cabins. Idempotent no-op when the bit was already at the target.
     * NOTE: the sweep runs ONLY when policy changed; subsequent
     * SetGuard("x", on) when x is already a guard is free. */
    if (was_guard != guard) {
        (void)MemTagSweepGuard(tid, guard);
    }

    /* Touch publish so subscribers (security daemons, audit log) can
     * react to capability-policy changes. */
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

/* ─── Cabin grant / revoke ────────────────────────────────────────── */

/* Forward-declare to avoid pulling process.h into memtag.h. */
struct process_t;
typedef struct process_t process_t;
process_t *process_find_ref(uint32_t pid);  /* ref-counted lookup */
void       process_ref_dec(process_t *proc);

/* process_t accessors exposed by process.c. Caller knows ctx is opaque
 * — Phase 2C casts it back to vmm_context_t* for PTE walks. */
extern uint64_t *process_active_memtags(process_t *proc);
extern void     *process_get_cabin(process_t *proc);

/* Forward declarations for Phase 2C engine — defined later in the file
 * after MemRegionAccessAllowed / GuardsOnRegion, but called from Grant/
 * Revoke right below. */
static void   SnapshotMask(const uint64_t *mask, uint64_t *mask_snap);
static size_t EnforceForCabinTag(vmm_context_t *target_ctx,
                                  const uint64_t *mask,
                                  uint16_t tag_id, bool revoke);

/* Internal: copy cabin's mask into mask_snap (zeroed if mask is NULL).
 * Snapshot under the assumption caller holds proc ref so mask memory
 * stays alive. */
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
    if (tag_id >= MEMTAG_MASK_BITS) return ERR_NO_MEMORY;  /* over inline cap */

    process_t *proc = process_find_ref(pid);
    if (!proc) return ERR_OBJECT_NOT_FOUND;
    uint64_t *mask = process_active_memtags(proc);
    bool already_held = mask ? MaskHas(mask, tag_id) : false;
    if (mask && !already_held) MaskSetAtomic(mask, tag_id);

    /* Phase 2C — restore PTEs for previously revoked mappings now that
     * this cabin gained the guard. We hold the proc ref through enforcement
     * so the cabin's vmm_context_t stays alive across the PTE walk. */
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
    if (tag_id >= MEMTAG_MASK_BITS) return OK;  /* never could have been set */

    process_t *proc = process_find_ref(pid);
    if (!proc) return ERR_OBJECT_NOT_FOUND;
    uint64_t *mask = process_active_memtags(proc);
    bool was_held = mask ? MaskHas(mask, tag_id) : false;
    if (mask && was_held) MaskClearAtomic(mask, tag_id);

    /* Phase 2C — invalidate any live PTEs that were granted under
     * this capability. Hold proc ref through enforcement (same reason
     * as Grant). No-op if cabin never held it or tag not guard. */
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

/* ─── Access check ────────────────────────────────────────────────── */

/* Walk region's tag_ids[]; for each that is GUARD-flagged, verify cabin
 * holds it. Returns true on permit. Fail-closed for region/cabin
 * not-found.
 *
 * Hot-path properties:
 *   - region tag list snapshot via MemRegionRegistryListTags (bucket lock)
 *   - guard-flag check via tag registry single-bit read (registry lock)
 *   - mask read is atomic load, no lock
 * Total: ≤2 lock acquisitions per access. Acceptable for soft check;
 * Phase 2B will need a faster path via cached per-region guard summary. */
/* Helper: filter region's tag list to guard-flagged tags only. Returns
 * count of guards written to `out`. Doing this BEFORE the pid lookup
 * lets us answer "no guards → allow" without ever touching process_t,
 * which is essential for test_pid=0 (kernel sentinel has no process_t)
 * and any non-existent pid where the answer should still be "no guards
 * means no enforcement". */
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
    if (!g_memtag_initialized) return true;  /* fail-open if uninit */
    if (region_id == MEMTAG_INVALID_REGION_ID) return false;

    uint16_t guards[MEMTAG_GUARDS_PER_REGION];
    size_t ng = GuardsOnRegion(region_id, guards, MEMTAG_GUARDS_PER_REGION);
    if (ng == 0) return true;  /* no guards → no enforcement, allow */

    /* Region has guards — need the cabin's mask. Missing process → deny. */
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

/* ─── Phase 2B — enforcement + fault publish ─────────────────────── */

bool MemTagEnforce(uint32_t pid, uintptr_t va, uint32_t region_id) {
    /* Untracked / unknown region → nothing to enforce. Allow.
     * (Primitive MemRegionAccessAllowed is fail-closed on invalid id;
     * the enforcement wrapper is fail-open since "no region" means
     * "no policy applies", not "policy says deny".) */
    if (region_id == MEMTAG_INVALID_REGION_ID) return true;
    if (MemRegionAccessAllowed(pid, region_id)) return true;

    /* Deny path — publish payload for security daemons / audit log. */
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
    if (rid == MEMTAG_INVALID_REGION_ID) return true;  /* untracked → allow */
    return MemTagEnforce(pid, 0, rid);
}

uint16_t MemRegionFirstMissingGuard(uint32_t pid, uint32_t region_id) {
    if (!g_memtag_initialized) return MEMTAG_INVALID_TAG_ID;
    if (region_id == MEMTAG_INVALID_REGION_ID) return MEMTAG_INVALID_TAG_ID;

    uint16_t guards[MEMTAG_GUARDS_PER_REGION];
    size_t ng = GuardsOnRegion(region_id, guards, MEMTAG_GUARDS_PER_REGION);
    if (ng == 0) return MEMTAG_INVALID_TAG_ID;

    /* Region has guards. If pid invalid, the FIRST guard is implicitly
     * missing — report it. Real process: check actual mask. */
    process_t *proc = process_find_ref(pid);
    uint64_t *mask = proc ? process_active_memtags(proc) : NULL;

    uint16_t missing = MEMTAG_INVALID_TAG_ID;
    for (size_t i = 0; i < ng; i++) {
        if (!mask || !MaskHas(mask, guards[i])) { missing = guards[i]; break; }
    }
    if (proc) process_ref_dec(proc);
    return missing;
}

/* ─── Stats ─────────────────────────────────────────────────────────── */

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

/* ═══════════════════════════════════════════════════════════════════
 *  Phase 2C — Continuous PTE enforcement engine
 * ═══════════════════════════════════════════════════════════════════ */

/* ─── Attach / detach facades ─────────────────────────────────────── */

/* Forward decl — defined below under PTE manipulation primitives. */
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
        /* Phase 2D — stamp PTE bits 52-58 so vmm_handle_page_fault's
         * fast-path recovers region_id from the PTE itself (skips the
         * phys → id_by_page indirect load). Idempotent on re-attach
         * with the same region_id. */
        StampPteRegion((vmm_context_t *)ctx, va_base, pages,
                        region_id, page_class);
        /* Phase 2H+ — tag-driven PKU stamping. If region carries a
         * pku:N tag, stamp PTE bits 62:59 with that key value. No-op
         * when region has no pku tag (effective pkey = 0). Refuses
         * gracefully if proposed PTE would conflict with CET supv-SS
         * (bit 60) via vmm_pte_pkey_cet_conflict — attach itself still
         * succeeds, the PTE keeps its existing PKEY field. */
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

/* ─── PTE manipulation primitives ─────────────────────────────────── */

/* Phase 2D — stamp encoded region_id (region_id & 0x7F) into PTE bits
 * 52-58 of every leaf covering `[va_base, va_base + pages*4K)`. After
 * stamping, vmm_handle_page_fault recovers region_id directly from the
 * PTE without walking phys → id_by_page. CAS-loop tolerates concurrent
 * VMM ACCESSED/DIRTY updates. Idempotent — re-stamping with the same
 * region_id is a no-op. */
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
        /* Defensive: match page class so a 4K stamp never lands on a
         * 2 MiB leaf (or vice-versa) after a concurrent demote/promote. */
        if (is_2m  && level != 2) continue;
        if (!is_2m && level != 1) continue;

        pte_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
        for (;;) {
            pte_t new_val = (old & ~MEMTAG_PTE_REGION_MASK) | encoded;
            if (new_val == old) break;  /* already stamped */
            if (__atomic_compare_exchange_n(pte, &old, new_val, false,
                                             __ATOMIC_ACQ_REL,
                                             __ATOMIC_ACQUIRE)) {
                break;
            }
            /* old refreshed; loop. */
        }
    }
}

/* Atomically clear PTE.PRESENT on one leaf entry. Returns true if the
 * bit was actually toggled (false if it was already clear or PTE absent).
 * No TLB flush issued — caller batches shootdown across the full attach
 * span for efficiency.
 *
 * CAS loop tolerates concurrent VMM mutators (which set/clear ACCESSED
 * + DIRTY bits) on the same PTE — those don't conflict with our
 * targeted PRESENT-bit dance. Bits 52-58 (Phase 2D region encoding) are
 * untouched by this primitive — they survive revoke/grant cycles. */
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
        /* old now holds the latest value; retry if still PRESENT. */
    }
    return false;
}

/* Atomically restore PRESENT + original flags on one leaf entry.
 *
 * Preserves existing PHYS bits (under normal lifecycles ClearPresentLeaf
 * only touched the PRESENT bit, so phys stayed put). If a concurrent
 * vmm_unmap_page zeroed the entire PTE during the revoke window, the
 * phys==0 guard below refuses the restore — see body for why.
 *
 * Returns true on toggle. */
static bool RestorePresentLeaf(vmm_context_t *ctx, uintptr_t va,
                                uint64_t orig_flags, bool is_2m) {
    uint8_t level = 0;
    pte_t *pte = vmm_get_leaf_pte(ctx, va, &level);
    if (!pte) return false;
    /* Defense in depth: refuse to restore if leaf level doesn't match
     * the page class encoded by the caller. Prevents corrupting a 4 KiB
     * PTE that overlaps a former 2 MiB attach (page-table edits during
     * revoke window). */
    if (is_2m && level != 2) return false;
    if (!is_2m && level != 1) return false;

    /* The PHYS bits were never disturbed by our revoke — only the
     * lower-12 flag bits + NX. Rebuild the canonical PTE =
     *   phys | (orig_flags & flags_mask) | PRESENT [+ LARGE_PAGE if 2M].
     *
     * SAFETY: if PHYS bits are 0 (PTE was zeroed externally — e.g. a
     * concurrent vmm_unmap_page won the race against our attach detach),
     * refuse the restore. Setting PRESENT with phys=0 would map physical
     * address 0 (BIOS data / NULL trap reserved area) into userspace,
     * leaking that page or triggering a fresh #PF anyway. Returning
     * false here keeps the PTE in its unmapped state — caller will
     * observe `transitioned` not incrementing and Touch publishes
     * still happen at the layer above for the policy event. */
    pte_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    for (;;) {
        if (old & VMM_FLAG_PRESENT) return false;  /* already live */
        uintptr_t phys_bits = old & vmm_get_addr_mask();
        if (phys_bits == 0) return false;          /* externally cleared */
        /* Phase 2D — bits 52-58 carry the encoded region_id stamped at
         * attach time. ClearPresentLeaf never touched them, so `old`
         * still holds the encoding; rebuild new_val with that window
         * preserved. Without this OR we'd lose the cache on every
         * revoke/grant cycle and force #PF fast-path back to fallback. */
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

/* Walk every page covered by `att` and clear PRESENT. Returns true if
 * any PTE was toggled (used to gate the shootdown). */
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

/* Walk every page covered by `att` and restore PRESENT with orig flags. */
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

/* ─── Cabin-bound enforcement ─────────────────────────────────────── */

/* Helper: does a cabin (identified by its already-fetched mask + pid)
 * satisfy ALL guard tags on a region? Stand-alone path bypassing the
 * process_find_ref lookup so we don't ref-bounce inside enforcement
 * loops. */
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

/* Walk regions carrying `tag_id`, snapshot each region's attach list,
 * and for any attach matching `target_ctx`:
 *   - revoke=true  → clear PTE.P + shootdown + mark REVOKED
 *   - revoke=false → restore PTE.P only if cabin satisfies ALL guards
 *                    on the region → mark ACTIVE
 *
 * Returns count of attachments transitioned. `mask` is the cabin's
 * memtag mask snapshot (caller's lifetime). */
static size_t EnforceForCabinTag(vmm_context_t *target_ctx,
                                  const uint64_t *mask,
                                  uint16_t tag_id, bool revoke) {
    if (!target_ctx) return 0;

    /* Query regions carrying tag_id. Bounded by MEMTAG_ENFORCE_REGIONS_PER_CALL
     * (~1 KiB stack). SECURITY: if MORE regions carry this tag than the
     * buffer holds, the bitmap query writes the first N and silently
     * drops the rest. For a guard revoke this would leave PTEs live for
     * the dropped regions — defeating the capability. We log a kernel
     * warning so operators see when their tag-region density exceeds
     * the enforcement budget. Bumping the constant + recompile is the
     * production response (1 region_id = 4 B; even at 4096 we're 16 KiB
     * stack — manageable since enforcement is off-the-hot-path). */
    uint32_t region_ids[MEMTAG_ENFORCE_REGIONS_PER_CALL];
    size_t nr = MemTagBitmapQueryAnd(&g_bitmap_index, &tag_id, 1,
                                      region_ids,
                                      MEMTAG_ENFORCE_REGIONS_PER_CALL);
    if (nr == MEMTAG_ENFORCE_REGIONS_PER_CALL) {
        /* Best-effort warning — the underlying bitmap doesn't expose a
         * "total count" so we can't say "X dropped"; the hit-cap is the
         * signal that there MAY be more. */
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
                /* Optimisation: if cabin still satisfies all guards on
                 * THIS region (i.e. region has multiple guards and cabin
                 * holds the rest), skip the revoke. */
                if (CabinSatisfiesRegion(mask, rid)) continue;
                if (ClearAttachRange(att)) transitioned++;
                MemRegionRegistrySetAttachState(&g_region_registry, rid,
                                                 att->ctx, att->va_base,
                                                 MEMTAG_ATTACH_REVOKED);
            } else {
                if (att->state != MEMTAG_ATTACH_REVOKED) continue;
                /* Only restore if cabin now satisfies ALL guards on the
                 * region — restoring otherwise would create an inconsistent
                 * state. */
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

/* ─── Global sweep on SetGuard flip ───────────────────────────────── */

/* Iterate all processes (snapshot of PIDs) and for each call the
 * cabin-bound enforcement. Used by MemTagSweepGuard. */
extern uint32_t process_snapshot_pids(uint32_t *out, uint32_t max);

size_t MemTagSweepGuard(uint16_t tag_id, bool guard_on) {
    if (!g_memtag_initialized) return 0;
    if (tag_id == MEMTAG_INVALID_TAG_ID) return 0;

    /* Snapshot live PID set — stack-budgeted at MEMTAG_SWEEP_PIDS_PER_CALL
     * × 4 B = 1 KiB. SweepGuard fires only on policy flips, not in any hot
     * path; if production process counts approach the cap, switch to
     * kmalloc/kfree. */
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

/* ═══════════════════════════════════════════════════════════════════
 *  Phase 2H+ — Tag-driven PKU PTE stamping
 *
 *  See memtag.h "Phase 2H+" block for the spec + policy citations.
 *  Auto-stamp on attach + sweep on tag mutation. Refuses stamps that
 *  would collide with CET supv-SS (bit 60) per the policy decision.
 * ═══════════════════════════════════════════════════════════════════ */

/* RELAXED telemetry — never load-bearing, used by tests + dump only. */
static volatile uint64_t g_pku_stamp_count    = 0;
static volatile uint64_t g_pku_conflict_count = 0;
static volatile uint64_t g_pku_sweep_count    = 0;

static void ResolvePkuTagIdsIfNeeded(void) {
    if (g_pku_tag_ids_resolved) return;
    /* Iterate pku:0..pku:15. The strings are reserved in SeedReservedTags
     * so MemTagRegistryLookupStr returns a valid ID for each. */
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

/* tag_id → pkey value (0..15), or -1 if not a pku:N tag. O(16) — fits
 * in one cache line. Used by RegionEffectivePkey + apply/clear hook. */
static int PkuValueFromTagId(uint16_t tag_id) {
    if (tag_id == MEMTAG_INVALID_TAG_ID) return -1;
    ResolvePkuTagIdsIfNeeded();
    for (uint32_t i = 0; i < MEMTAG_PKU_MAX_KEYS; i++) {
        if (g_pku_tag_ids[i] == tag_id) return (int)i;
    }
    return -1;
}

/* Stamp PTE.PKEY bits 62:59 atomically. The CET conflict policy
 * (vmm_pte_pkey_cet_conflict) is meaningful only when CR4.CET=1, where
 * bit 60 reinterprets as the supervisor shadow-stack indicator. With
 * CR4.CET=0 (BoxOS today — Phase 2K is observe-only), bit 60 IS part
 * of the PKEY field, so stamping pkey ≥ 4 legitimately sets it. Read
 * CR4.CET once per call and gate the conflict refusal accordingly.
 *
 * Returns true if any bit was actually toggled (false on idempotent
 * stamp matching current value, on absent PTE, or on CET refusal). */
static bool StampPtePkeyLeaf(vmm_context_t *ctx, uintptr_t va,
                              uint8_t pkey, bool is_2m) {
    uint8_t level = 0;
    pte_t *pte = vmm_get_leaf_pte(ctx, va, &level);
    if (!pte) return false;
    if (is_2m  && level != 2) return false;
    if (!is_2m && level != 1) return false;

    /* Read CR4.CET once. CR4 bit 23 = CET enable. */
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    bool cet_active = (cr4 & (1ULL << 23)) != 0;

    uint64_t new_pkey_field = vmm_pte_encode_pkey(pkey);
    pte_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    for (;;) {
        pte_t new_val = (old & ~VMM_PTE_PKEY_MASK) | new_pkey_field;
        /* Conflict only matters under CR4.CET=1: bit 60 then encodes
         * the supv-SS indicator, NOT a PKEY bit. Without CET, the
         * policy helper would falsely refuse pkey ∈ {4..7,12..15}. */
        if (cet_active && vmm_pte_pkey_cet_conflict(new_val)) {
            __atomic_fetch_add(&g_pku_conflict_count, 1, __ATOMIC_RELAXED);
            return false;
        }
        if (new_val == old) return true;     /* already stamped */
        if (__atomic_compare_exchange_n(pte, &old, new_val, false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            __atomic_fetch_add(&g_pku_stamp_count, 1, __ATOMIC_RELAXED);
            return true;
        }
        /* old refreshed; loop. */
    }
}

/* Walk every leaf covering [va_base, va_base + pages*4K) and stamp
 * PKEY. Returns number of PTEs touched (toggle count, not idempotent
 * matches). 2 MiB attaches: walks in 2 MiB steps; each PD-leaf stamped
 * once. Used by AttachCabin auto-stamp + MemTagSweepPkey. */
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
    uint16_t buf[MEMTAG_ENFORCE_ATTACHES_PER_REGION];   /* reuse budget */
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

    /* Find + clear any existing pku:N tag first so the region carries
     * at most one. Walk region's tag list, drop any pku:* we find. */
    uint16_t buf[MEMTAG_ENFORCE_ATTACHES_PER_REGION];
    size_t   n = MemRegionRegistryListTags(&g_region_registry, region_id,
                                            buf, sizeof(buf)/sizeof(buf[0]));
    for (size_t i = 0; i < n; i++) {
        int v = PkuValueFromTagId(buf[i]);
        if (v < 0) continue;
        if (v == (int)pkey) {
            /* Already has the exact tag — sweep to enforce in case PTEs
             * drifted (idempotent), then bail. */
            (void)MemTagSweepPkey(region_id, pkey);
            return OK;
        }
        /* Different pku:N — clear the old one. The MemTagClear path
         * won't re-call sweep because we set the suppress flag via
         * the SetPkey policy: actual stamp happens at the end below. */
        (void)MemRegionRegistryRemoveTag(&g_region_registry, region_id, buf[i]);
        MemTagBitmapClear(&g_bitmap_index, buf[i], region_id);
        MemTagRegistryRefDec(&g_tag_registry, buf[i]);
        PublishTagEvent("memtag:tag:cleared", region_id, buf[i]);
    }

    /* pkey==0 means "no pku tag" — the clears above already removed any
     * prior pku tag; just sweep PTEs to clear PKEY bits. */
    if (pkey == 0) {
        (void)MemTagSweepPkey(region_id, 0);
        return OK;
    }

    /* Apply the new pku:N tag — uses canonical MemTagApply path so
     * tag-registry refcount + bitmap + Touch all stay coherent. */
    char tag_buf[8];
    /* "pku:" + 2-digit + NUL = 7 chars max */
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
    /* MemTagApply will trigger MemTagSweepPkey via the apply-hook (see
     * MemTagApply body below). Caller gets the up-to-date PTE state. */
    return OK;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Phase 2D — PTE bits 52-58 fast cache + AP-verify (M1 + M5)
 *
 *  See memtag.h "Phase 2D" block for the spec citations and encoding
 *  rationale. The fast-path replaces the two-step
 *      virt → phys → MemRegionFromPhys(phys)
 *  with the single-step
 *      pte_value → MemRegionFromPte(pte, phys)
 *  saving a dense-array load in the common case.
 * ═══════════════════════════════════════════════════════════════════ */

uint32_t MemRegionFromPte(uint64_t pte_value, uintptr_t phys) {
    if (!g_memtag_initialized) return MEMTAG_INVALID_REGION_ID;

    /* Extract the encoded region_id (PTE bits 52-58). Zero is a valid
     * encoded value (matches region_id 0 OR 128 OR 256…), distinguished
     * from "unstamped" only by the phys-overlap verification below. */
    uint32_t encoded = (uint32_t)((pte_value & MEMTAG_PTE_REGION_MASK)
                                  >> MEMTAG_PTE_REGION_SHIFT);

    MemRegion *r = MemRegionRegistrySlot(&g_region_registry, encoded);
    if (r && (r->flags & MEMTAG_REGION_FLAG_ACTIVE) && r->base_phys) {
        uintptr_t end = r->base_phys + (uintptr_t)r->pages * PMM_PAGE_SIZE;
        if (phys >= r->base_phys && phys < end) {
            return encoded;
        }
    }
    /* Cache miss: PTE wasn't stamped (e.g. boot-seeded zone region whose
     * pages were later sub-allocated; the per-page mapping doesn't know
     * which region_id covers that phys), or the encoded id collides
     * with a different active region (region_id ≥ 128 wraps modulo 128).
     * Fall back to the dense reverse-lookup index — still O(1). */
    return MemRegionRegistryFromPhys(&g_region_registry, phys);
}

/* M5 boot probe — verify PTE bits 52-58 are safely "Ignored" per the
 * spec citations above. Reads MAXPHYADDR + CR4.PKE/CR4.CET state on
 * the calling CPU and logs the result. Returns false (and logs ABORT)
 * only when a condition would invalidate the metadata-bit assumption;
 * on all production Intel/AMD x86_64 silicon today this returns true.
 *
 * Idempotent — safe to call from BSP after MemTagInit and from every
 * AP after cpu_intersect_features_ap (per_core_init_ap call site). */
bool MemTagVerifyPteMetadataBits(void) { return vmm_verify_pte_metadata_bits_52_58(); }

#if 0  /* dead — original impl moved to vmm.c (clean-slate audit) */
bool MemTagVerifyPteMetadataBits_OLD(void) {
    extern uint8_t vmm_maxphyaddr;

    /* Bits 52-58 are "Ignored" only when MAXPHYADDR ≤ 52. On any future
     * silicon that bumps MAXPHYADDR past 52, bits 52..(M-1) become real
     * phys-address bits and stamping them would corrupt mappings. */
    if (vmm_maxphyaddr > 52) {
        debug_printf("[MEMTAG] M5 ABORT: MAXPHYADDR=%u > 52 — "
                     "bits 52-58 are phys, NOT ignored\n",
                     (unsigned)vmm_maxphyaddr);
        return false;
    }

    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    bool cr4_pke = ((cr4 >> 22) & 1u) != 0;   /* Protection Keys for User */
    bool cr4_pks = ((cr4 >> 24) & 1u) != 0;   /* Protection Keys for Sup. */
    bool cr4_cet = ((cr4 >> 23) & 1u) != 0;   /* CET (shadow stack)        */

    /* All three features touch bits OUTSIDE 52-58:
     *   PKE / PKS → PKEY field at bits 62..59 (Intel SDM Vol 3A §4.6.2)
     *   CET       → supervisor-shadow-stack indicator at bit 60 (§17)
     * Logged for telemetry — never causes ABORT on current silicon. */
    debug_printf("[MEMTAG] M5 PTE-metadata probe: MAXPHYADDR=%u "
                 "CR4.PKE=%d CR4.PKS=%d CR4.CET=%d → bits 52-58 SAFE\n",
                 (unsigned)vmm_maxphyaddr,
                 (int)cr4_pke, (int)cr4_pks, (int)cr4_cet);
    return true;
}
#endif /* dead — moved to vmm.c */

/* Phase 2E originally implemented PAT MSR consistency probe + MTRR
 * audit here. The audit pass after Phase 2K moved them to vmm.c
 * (vmm_verify_pat_msr / vmm_dump_mtrr_layout) because they are pure
 * CPU MSR probes with no MemTag-specific state. The names below are
 * retained as thin wrappers for backward source compatibility — they
 * just forward to the VMM implementations. */
bool MemTagVerifyPatMsr(void)  { return vmm_verify_pat_msr(); }
void MemTagDumpMtrrLayout(void) { vmm_dump_mtrr_layout(); }

#if 0  /* dead — original impls moved to vmm.c */
bool MemTagVerifyPatMsr_OLD(void) {
    /* CPUID-gated. If PAT isn't supported (and so vmm_pat_init no-op'd),
     * the BSP snapshot stays 0 and RDMSR(0x277) would #GP. Skip
     * verification entirely — return true so non-PAT systems pass. */
    if (!g_cpu_caps.has_pat) {
        debug_printf("[MEMTAG] PAT MSR verify SKIP: CPU lacks PAT (has_pat=0)\n");
        return true;
    }

    uint64_t bsp = vmm_get_pat_msr_value();
    if (bsp == 0) {
        /* Probe ran before vmm_pat_init on BSP. Not a real failure;
         * just not yet ready. Returning true keeps the AP boot
         * non-fatal — the BSP probe (after vmm_pat_init) is the
         * authoritative check. */
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

/* Decode an MTRR memory type byte to a stable string. Returns NULL for
 * truly invalid encodings (per Intel SDM Vol 3A Table 11-8: types 2/3 are
 * RESERVED — never produced by valid firmware). */
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
        /* has_pat is the closest proxy we have for "real x86_64 with MSRs".
         * Skip on the (theoretical) Pentium-less CPU. */
        debug_printf("[MEMTAG] MTRR audit SKIP: CPU lacks PAT/MSRs\n");
        return;
    }

    uint64_t cap = MtagRdmsr(MEMTAG_MSR_IA32_MTRRCAP);
    uint64_t def = MtagRdmsr(MEMTAG_MSR_MTRR_DEF_TYPE);

    /* IA32_MTRRCAP layout (Intel SDM Vol 3A Table 11-3):
     *   bits 7:0 → VCNT  — number of variable-range MTRR register pairs
     *   bit 8    → FIX   — fixed-range MTRRs supported
     *   bit 10   → WC    — write-combining type supported
     *   bit 11   → SMRR  — System-Management-Range Registers supported
     *
     * IA32_MTRR_DEF_TYPE layout (Table 11-4):
     *   bits 7:0 → Type  — default memory type for unmatched ranges
     *   bit 10   → FE    — fixed-range MTRRs enabled
     *   bit 11   → E     — MTRRs globally enabled
     */
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
        /* WB is the production-typical default. Anything else means the
         * unmapped phys space is non-WB, which silently changes the
         * effective type of any region whose MTRR didn't explicitly
         * override. */
        debug_printf("[MEMTAG] MTRR audit WARN: DEF_TYPE=%s — non-WB "
                     "default means PAT cache:wb tags may be silently "
                     "demoted (Intel SDM §11.12.5 combination table).\n",
                     def_str ? def_str : "RESERVED");
    }

    /* Variable-range MTRRs (up to VCNT pairs). Each pair is one MTRR
     * region: PHYSBASE (type + base phys) + PHYSMASK (mask + valid).
     * Cap at 8 to bound the log; typical FW programs 2-4. */
    uint32_t walk_n = vcnt > 8 ? 8 : vcnt;
    for (uint32_t i = 0; i < walk_n; i++) {
        uint64_t base = MtagRdmsr(MEMTAG_MSR_MTRR_PHYSBASE0 + i * 2);
        uint64_t mask = MtagRdmsr(MEMTAG_MSR_MTRR_PHYSMASK0 + i * 2);
        if (!((mask >> 11) & 1u)) continue;  /* not valid */
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
#endif  /* dead — moved to vmm.c */
