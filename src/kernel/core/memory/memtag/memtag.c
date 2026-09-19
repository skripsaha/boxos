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
     * mce:poisoned is NOT applied by the #MC handler: that handler may
     * take no lock while MCIP is set, and the MemTag registry is a
     * lock-taker. It goes on the page named by IA32_MC<i>_ADDR from a
     * K-Core, off the record slot's own baton (mce_note_worker), or —
     * on a fatal kernel-mode verdict, where no core lives to pump that
     * baton — from the dying core itself. APEI GHES applies it too, for
     * a memory error the firmware reports. PMM tracks the same page in
     * pmm_poisoned_bitmap from the handler, so the allocator refuses to
     * hand it back out even if no tag ever lands. */
    MarkInternReserved("mce:poisoned");
    MarkInternReserved("mce:corrected");
    MarkInternReserved("mce:fault:detected");
    MarkInternReserved("mce:fault:recovered");
    MarkInternReserved("mce:fault:fatal");
    /* MCE page migration (mce_migrate.c) — phys → MemRegion attach
     * chain → per-cabin PTE swap with TLB shootdown. Published from
     * K-Core context (the worker runs off the slot's baton, not on the IST
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
    /* Runtime TME pool lifecycle (tme.c). pool:ready fires after BSP
     * SET_KEY_RANDOM sweep; rekey:failed flags a hardware-rejected
     * PCONFIG on free (slot is removed from the pool). */
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
    /* Phase 2K — CET (Control-flow Enforcement) namespace.
     * cet:ready signals probe completion; cet:shstk:supervisor /
     * cet:shstk:user name a phys range the CET lifecycle allocated as
     * shadow stack (the PTE bits 60 / 61 it sets there are protection-key
     * bits, not a shadow-stack attribute — see vmm.h's CET block);
     * cet:fault:cp captures #CP (vector 21) Control-Protection
     * Exception events for telemetry. */
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

    /* Resolve at the ACTUAL leaf level. vmm_virt_to_phys only finds 4 KiB PT
     * leaves, but user heap and Bay are mapped with implicit 2 MiB pages — a
     * pointer into any >=2 MiB allocation would otherwise miss. vmm_get_leaf_pte
     * reports the leaf level (1/2/3 = 4K/2M/1G); mask the phys base per level
     * and add the in-page offset so MemRegionFromPhys hits the right page. */
    uint8_t level = 0;
    pte_t *leaf = vmm_get_leaf_pte(vctx, virt, &level);
    if (!leaf) return MEMTAG_INVALID_REGION_ID;
    pte_t entry = __atomic_load_n(leaf, __ATOMIC_ACQUIRE);
    if (!(entry & VMM_FLAG_PRESENT)) return MEMTAG_INVALID_REGION_ID;

    uintptr_t page_size = (level == 3) ? (1ULL << 30)   /* 1 GiB PDPT leaf */
                        : (level == 2) ? (1ULL << 21)   /* 2 MiB PD leaf   */
                                       : (1ULL << 12);  /* 4 KiB PT leaf   */
    uintptr_t phys = (vmm_pte_to_phys(entry) & ~(page_size - 1)) + (virt & (page_size - 1));
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

/* ─── Out-pointer query variants ────────────────────────────────────
 *
 * The return-by-value variants below spill the 2056-byte MemTagResult
 * to a stack temporary at every call site. Code that calls them
 * repeatedly (memtag_test.c MemTagStressTest is the worst offender)
 * accumulates the temporaries into a >8 KiB frame that exceeds
 * -Wstack-usage=8192 and competes with nested IRQ frames on the
 * 16 KiB per-cpu kernel stacks.
 *
 * The _Into variants write into a caller-owned out-buffer. The
 * existing return-by-value functions become thin wrappers for API
 * compatibility — they're still safe to call from cold paths, but
 * hot paths should switch to _Into. */

void MemTagQueryAndInto_(const char *const *tag_strs, MemTagResult *out) {
    if (!out) return;
    out->count = 0;
    out->total = 0;
    if (!g_memtag_initialized) return;

    uint16_t ids[64];
    size_t n = ResolveTagStrs(tag_strs, ids, 64);
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == MEMTAG_INVALID_TAG_ID) return;
    }
    out->count = MemTagBitmapQueryAnd(&g_bitmap_index, ids, (uint16_t)n,
                                       out->region_ids,
                                       MEMTAG_QUERY_MAX_IDS, &out->total);
}

void MemTagQueryOrInto_(const char *const *tag_strs, MemTagResult *out) {
    if (!out) return;
    out->count = 0;
    out->total = 0;
    if (!g_memtag_initialized) return;

    uint16_t ids[64];
    size_t n = ResolveTagStrs(tag_strs, ids, 64);
    uint16_t known[64]; uint16_t kn = 0;
    for (size_t i = 0; i < n; i++)
        if (ids[i] != MEMTAG_INVALID_TAG_ID) known[kn++] = ids[i];
    if (kn == 0) return;
    out->count = MemTagBitmapQueryOr(&g_bitmap_index, known, kn,
                                      out->region_ids,
                                      MEMTAG_QUERY_MAX_IDS, &out->total);
}

void MemTagQueryMixedInto_(const char *const *required,
                            const char *const *any,
                            const char *const *excluded,
                            MemTagResult *out) {
    if (!out) return;
    out->count = 0;
    out->total = 0;
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
                                         MEMTAG_QUERY_MAX_IDS, &out->total);
}

/* Return-by-value compat shims — kept for source compat with callers
 * that already use the MemTagAnd / MemTagOr macros. New code prefers
 * the _Into variants above. */
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

/* ─── Phase 2C enforcement walk batches ────────────────────────────── */

/* These are BATCH sizes for streaming cursors, not ceilings on the sets
 * being walked. The enforcement walks below take one batch under the
 * index (or bucket) lock, release it, do the PTE work with no memtag lock
 * held, and come back for the next batch keyed by the last value they
 * saw. Nothing is dropped for being past a limit; the limits are only how
 * much stack one hop is allowed to borrow, which matters because the
 * build runs -Wstack-usage=8192 and enforcement can run on the 16 KiB
 * per-core kernel stack under a nested IRQ frame. */

/* One 64-bit bitmap word carries exactly 64 region_ids, so a 64-id batch
 * is the widest run MemTagBitmapWalkTag can lift out of a single word
 * without re-reading it. 64 × 4 B = 256 B. */
#define MEMTAG_WALK_REGIONS_BATCH   64u

/* sizeof(MemRegionAttach) is 48 B, so 8 per batch is 384 B — one batch of
 * regions and one of attaches together stay under half a kilobyte of the
 * enforcement frame. */
#define MEMTAG_WALK_ATTACHES_BATCH   8u

/* Sizes the per-region TAG-LIST snapshots: the guard filter in
 * GuardsOnRegion and the pku:N scans. A region's tag_ids[] is a sorted
 * set the region itself owns, and 64 matches the width every other
 * tag-array in this file uses (ResolveTagStrs, the query key buffers). */
#define MEMTAG_GUARDS_PER_REGION  64u

/* ─── The held-tag set a cabin carries ─────────────────────────────────
 *
 * memtag.h carries the shape and the geometry's derivation. What matters
 * here: the table is a plain uint64_t[MEMTAG_HELD_L1_ENTRIES] living in
 * cabin_t whose slots hold leaf ADDRESSES, because process.c's
 * process_active_memtags — the only handle this file has on a cabin's
 * set — hands the table out as uint64_t*. The two casts below are the
 * whole cost of that, and they are exact on LP64 x86_64.
 *
 * The bound on a tag id is the id space's own: tag_id is uint16_t and
 * MEMTAG_INVALID_TAG_ID (0xFFFF) is the one value above MEMTAG_MAX_TAG_ID,
 * so rejecting the invalid id is rejecting everything out of range. There
 * is no second, smaller ceiling any more.
 * ──────────────────────────────────────────────────────────────────── */

static inline uint32_t HeldWindow(uint16_t tag_id) {
    return (uint32_t)tag_id / MEMTAG_HELD_LEAF_IDS;
}

static inline uint32_t HeldWord(uint16_t tag_id) {
    return ((uint32_t)tag_id % MEMTAG_HELD_LEAF_IDS) >> 6;
}

/* One ACQUIRE load. NULL means the window has no leaf, which is the
 * truthful answer "this cabin holds no tag in it" — never a reason to
 * allocate, so every reader below is allocation-free by construction. */
static inline uint64_t *HeldLeaf(const uint64_t *held_l1, uint16_t tag_id) {
    return (uint64_t *)(uintptr_t)__atomic_load_n(&held_l1[HeldWindow(tag_id)],
                                                   __ATOMIC_ACQUIRE);
}

static inline bool HeldHas(const uint64_t *held_l1, uint16_t tag_id) {
    if (!held_l1 || tag_id == MEMTAG_INVALID_TAG_ID) return false;
    const uint64_t *leaf = HeldLeaf(held_l1, tag_id);
    if (!leaf) return false;
    uint64_t w = __atomic_load_n(&leaf[HeldWord(tag_id)], __ATOMIC_ACQUIRE);
    return (w >> (tag_id & 63)) & 1ULL;
}

/* Grant path only. CAS-install with loser-frees, the pattern
 * touch_bucket_get_or_create uses over this same id space: two strands
 * granting different ids in the same window race on one word and the
 * loser hands its leaf back, so no lock is introduced over the table and
 * no new nesting with it. Returns the installed leaf, or NULL only when
 * the kernel heap is out. */
static uint64_t *HeldLeafInstall(uint64_t *held_l1, uint16_t tag_id) {
    uint32_t  win  = HeldWindow(tag_id);
    uint64_t *leaf = (uint64_t *)(uintptr_t)__atomic_load_n(&held_l1[win],
                                                            __ATOMIC_ACQUIRE);
    if (leaf) return leaf;

    uint64_t *fresh = (uint64_t *)kmalloc(MEMTAG_HELD_LEAF_WORDS * sizeof(uint64_t));
    if (!fresh) return NULL;
    memset(fresh, 0, MEMTAG_HELD_LEAF_WORDS * sizeof(uint64_t));

    uint64_t expected = 0;
    if (!__atomic_compare_exchange_n(&held_l1[win], &expected,
                                     (uint64_t)(uintptr_t)fresh, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        kfree(fresh);
        return (uint64_t *)(uintptr_t)expected;
    }
    return fresh;
}

/* false only when the leaf could not be installed — the cabin's set is
 * unchanged in that case, so the caller may report the failure honestly
 * rather than leave a half-granted capability behind. */
static bool HeldSet(uint64_t *held_l1, uint16_t tag_id) {
    if (!held_l1 || tag_id == MEMTAG_INVALID_TAG_ID) return false;
    uint64_t *leaf = HeldLeafInstall(held_l1, tag_id);
    if (!leaf) return false;
    __atomic_or_fetch(&leaf[HeldWord(tag_id)], (1ULL << (tag_id & 63)),
                      __ATOMIC_ACQ_REL);
    return true;
}

/* Never installs: a window with no leaf holds no tag, so a clear there
 * has nothing to clear. That keeps revoke allocation-free too, which
 * matters because revoke must not be able to fail for memory. */
static void HeldClear(uint64_t *held_l1, uint16_t tag_id) {
    if (!held_l1 || tag_id == MEMTAG_INVALID_TAG_ID) return;
    uint64_t *leaf = HeldLeaf(held_l1, tag_id);
    if (!leaf) return;
    __atomic_and_fetch(&leaf[HeldWord(tag_id)], ~(1ULL << (tag_id & 63)),
                       __ATOMIC_ACQ_REL);
}

void MemTagHeldSetRelease(uint64_t *held_l1) {
    if (!held_l1) return;
    for (uint32_t i = 0; i < MEMTAG_HELD_L1_ENTRIES; i++) {
        /* Exchange rather than load-then-store: the slot is emptied and
         * the leaf taken in one step, so this is idempotent and a second
         * call frees nothing. */
        uint64_t *leaf = (uint64_t *)(uintptr_t)__atomic_exchange_n(&held_l1[i], 0,
                                                                     __ATOMIC_ACQ_REL);
        if (leaf) kfree(leaf);
    }
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

/* Forward declaration for the Phase 2C engine — defined later in the file
 * after MemRegionAccessAllowed / GuardsOnRegion, but called from Grant/
 * Revoke right below. */
static size_t EnforceForCabinTag(vmm_context_t *target_ctx,
                                  const uint64_t *held_l1,
                                  uint16_t tag_id, bool revoke);

error_t MemCabinGrant(uint32_t pid, uint16_t tag_id) {
    if (!g_memtag_initialized || tag_id == MEMTAG_INVALID_TAG_ID)
        return ERR_INVALID_ARGUMENT;

    process_t *proc = process_find_ref(pid);
    if (!proc) return ERR_OBJECT_NOT_FOUND;
    uint64_t *held = process_active_memtags(proc);
    bool already_held = HeldHas(held, tag_id);
    if (held && !already_held && !HeldSet(held, tag_id)) {
        /* The window's leaf could not be installed. The set is untouched,
         * so say so rather than publish a grant that did not happen. */
        process_ref_dec(proc);
        return ERR_NO_MEMORY;
    }

    /* Phase 2C — restore PTEs for previously revoked mappings now that
     * this cabin gained the guard. We hold the proc ref through enforcement
     * so the cabin's vmm_context_t stays alive across the PTE walk. */
    if (!already_held && MemTagRegistryIsGuard(&g_tag_registry, tag_id)) {
        vmm_context_t *ctx = (vmm_context_t *)process_get_cabin(proc);
        if (ctx) (void)EnforceForCabinTag(ctx, held, tag_id, false);
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

    process_t *proc = process_find_ref(pid);
    if (!proc) return ERR_OBJECT_NOT_FOUND;
    uint64_t *held = process_active_memtags(proc);
    bool was_held = HeldHas(held, tag_id);
    if (was_held) HeldClear(held, tag_id);

    /* Phase 2C — invalidate any live PTEs that were granted under
     * this capability. Hold proc ref through enforcement (same reason
     * as Grant). No-op if cabin never held it or tag not guard. */
    if (was_held && MemTagRegistryIsGuard(&g_tag_registry, tag_id)) {
        vmm_context_t *ctx = (vmm_context_t *)process_get_cabin(proc);
        if (ctx) (void)EnforceForCabinTag(ctx, held, tag_id, true);
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
    process_t *proc = process_find_ref(pid);
    if (!proc) return false;
    bool h = HeldHas(process_active_memtags(proc), tag_id);
    process_ref_dec(proc);
    return h;
}

size_t MemCabinListTags(uint32_t pid, uint16_t *out, size_t max) {
    if (!g_memtag_initialized || !out || max == 0) return 0;
    process_t *proc = process_find_ref(pid);
    if (!proc) return 0;
    uint64_t *held = process_active_memtags(proc);
    size_t n = 0;
    if (held) {
        /* Walks what the cabin HAS, not the id space it could have used:
         * an uninstalled window costs one load and a zero word costs one
         * load, and the set bits inside a word are lifted by ctz rather
         * than probed one id at a time. The old flat loop asked the mask
         * once per id and so cost 1024 loads to report a single tag; over
         * the full 16-bit space that shape would have cost 65535. */
        for (uint32_t win = 0; win < MEMTAG_HELD_L1_ENTRIES && n < max; win++) {
            uint64_t *leaf = (uint64_t *)(uintptr_t)__atomic_load_n(&held[win],
                                                                    __ATOMIC_ACQUIRE);
            if (!leaf) continue;
            for (uint32_t w = 0; w < MEMTAG_HELD_LEAF_WORDS && n < max; w++) {
                uint64_t bits = __atomic_load_n(&leaf[w], __ATOMIC_ACQUIRE);
                while (bits && n < max) {
                    uint32_t b = (uint32_t)__builtin_ctzll(bits);
                    bits &= bits - 1;
                    out[n++] = (uint16_t)(win * MEMTAG_HELD_LEAF_IDS + w * 64u + b);
                }
            }
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
 *   - held-set read is two dependent atomic loads (L1 slot, then leaf
 *     word), no lock — and no allocation, since an empty slot already
 *     answers "not held"
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

    /* Region has guards — need the cabin's held set. Missing process → deny. */
    process_t *proc = process_find_ref(pid);
    if (!proc) return false;
    uint64_t *held = process_active_memtags(proc);

    bool allowed = true;
    for (size_t i = 0; i < ng; i++) {
        if (!HeldHas(held, guards[i])) { allowed = false; break; }
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
     * missing — report it. Real process: check the actual held set. */
    process_t *proc = process_find_ref(pid);
    uint64_t *held = proc ? process_active_memtags(proc) : NULL;

    uint16_t missing = MEMTAG_INVALID_TAG_ID;
    for (size_t i = 0; i < ng; i++) {
        if (!HeldHas(held, guards[i])) { missing = guards[i]; break; }
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
         * when region has no pku tag (effective pkey = 0). Every key
         * 0..15 stamps; the field is four plain bits and no value of
         * it is reserved for anything else. */
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

/* Helper: does a cabin (identified by its already-fetched held set)
 * satisfy ALL guard tags on a region? Stand-alone path bypassing the
 * process_find_ref lookup so we don't ref-bounce inside enforcement
 * loops. */
static bool CabinSatisfiesRegion(const uint64_t *held_l1, uint32_t region_id) {
    uint16_t guards[MEMTAG_GUARDS_PER_REGION];
    size_t ng = GuardsOnRegion(region_id, guards, MEMTAG_GUARDS_PER_REGION);
    if (ng == 0) return true;
    if (!held_l1) return false;
    for (size_t i = 0; i < ng; i++) {
        if (!HeldHas(held_l1, guards[i])) return false;
    }
    return true;
}

/* ─── Lock order the enforcement walks below rely on ───────────────────
 *
 * Outermost to innermost, and every one of them is taken AND released
 * inside the leaf call that needs it:
 *
 *   process_lock            (process_find_ref)
 *   g_bitmap_index.lock     (MemTagBitmapWalkTag)
 *   region bucket lock      (MemRegionRegistryWalkAttachs / ListTags /
 *                            SetAttachState)
 *   tag registry lock       (MemTagRegistryIsGuard)
 *   vmm_context_t.lock + the shootdown IPI  (vmm_get_leaf_pte,
 *                                            vmm_shootdown_pages)
 *
 * The enforcement engine holds NONE of them across a call into another
 * subsystem, which is the whole reason the walks are resumable cursors
 * instead of callbacks. A callback fired from inside the bitmap index
 * would put the VMM context lock and the TLB shootdown IPI underneath
 * g_bitmap_index.lock — a nesting that exists nowhere in this tree today
 * and would have to be proved deadlock-free against every other path
 * that takes those two in the other order. Taking a batch and letting go
 * introduces no edge at all: the order above is exactly the order that
 * was already there before this file learned to stream.
 * ──────────────────────────────────────────────────────────────────── */

/* Apply one revoke/restore decision to every attach of `rid` owned by
 * `target_ctx`, streaming the attach chain so a long chain is walked to
 * its end rather than cut at a buffer's edge.
 *
 * The guard decision is taken ONCE per region: every attach of this
 * region into this cabin answers CabinSatisfiesRegion identically (same
 * held set, same region), and asking per attach only re-takes the bucket
 * and registry locks for an answer that cannot differ. Asking first also
 * means a region with nothing to do costs no chain walk at all. */
static size_t EnforceRegionAttachesForCabin(uint32_t rid,
                                             vmm_context_t *target_ctx,
                                             const uint64_t *held_l1,
                                             bool revoke) {
    bool satisfies = CabinSatisfiesRegion(held_l1, rid);
    /* Revoking a region the cabin still fully satisfies (it holds the
     * region's other guards) would tear down a mapping policy still
     * permits; restoring one it does not satisfy would hand back a
     * mapping policy still denies. Either way: nothing to do. */
    if (revoke == satisfies) return 0;

    size_t transitioned = 0;
    MemRegionAttach batch[MEMTAG_WALK_ATTACHES_BATCH];
    uintptr_t   cur_va  = 0;
    const void *cur_ctx = NULL;
    for (;;) {
        size_t na = MemRegionRegistryWalkAttachs(&g_region_registry, rid,
                                                  cur_va, cur_ctx, batch,
                                                  MEMTAG_WALK_ATTACHES_BATCH);
        if (na == 0) break;
        for (size_t j = 0; j < na; j++) {
            MemRegionAttach *att = &batch[j];
            if ((vmm_context_t *)att->ctx != target_ctx) continue;

            if (revoke) {
                if (att->state != MEMTAG_ATTACH_ACTIVE) continue;
                if (ClearAttachRange(att)) transitioned++;
                MemRegionRegistrySetAttachState(&g_region_registry, rid,
                                                 att->ctx, att->va_base,
                                                 MEMTAG_ATTACH_REVOKED);
            } else {
                if (att->state != MEMTAG_ATTACH_REVOKED) continue;
                if (RestoreAttachRange(att)) transitioned++;
                MemRegionRegistrySetAttachState(&g_region_registry, rid,
                                                 att->ctx, att->va_base,
                                                 MEMTAG_ATTACH_ACTIVE);
            }
        }
        cur_va  = batch[na - 1].va_base;
        cur_ctx = batch[na - 1].ctx;
    }
    return transitioned;
}

/* Walk EVERY region carrying `tag_id` and, for each attach belonging to
 * `target_ctx`:
 *   - revoke=true  → clear PTE.P + shootdown + mark REVOKED
 *   - revoke=false → restore PTE.P if the cabin satisfies ALL guards on
 *                    the region → mark ACTIVE
 *
 * Streams the region set a batch at a time and resumes by region id, so
 * the number of regions carrying a guard tag is not a quantity this
 * function has an opinion about. What happens when the set changes
 * between batches is spelled out at MemTagBitmapWalkTag.
 *
 * Returns count of attachments transitioned.
 *
 * `held_l1` is the cabin's LIVE held-tag set, not a copy of it. Every
 * caller used to hand down a sixteen-word copy taken before the walk;
 * a sparse set has no fixed width to copy into, and the copy was never
 * worth its cost anyway. It was sixteen independent ACQUIRE loads with
 * no generation or seqlock around them, so it was never a consistent
 * view of the set — only a stale one. Reading live is strictly fresher,
 * and safe on both counts that matter: the caller holds a process ref,
 * so the cabin (and therefore the table) outlives the walk; and a leaf
 * is install-once, freed only by MemTagHeldSetRelease at cabin teardown,
 * so a pointer read out of the table here can never be freed under this
 * walk. A grant or revoke landing mid-walk runs its own enforcement pass
 * for its own tag, which is what makes either interleaving converge on
 * the same PTE state. */
static size_t EnforceForCabinTag(vmm_context_t *target_ctx,
                                  const uint64_t *held_l1,
                                  uint16_t tag_id, bool revoke) {
    if (!target_ctx) return 0;

    size_t   transitioned = 0;
    uint32_t batch[MEMTAG_WALK_REGIONS_BATCH];
    uint32_t cursor = 0;
    for (;;) {
        size_t nr = MemTagBitmapWalkTag(&g_bitmap_index, tag_id, cursor,
                                         batch, MEMTAG_WALK_REGIONS_BATCH);
        if (nr == 0) break;
        for (size_t i = 0; i < nr; i++) {
            transitioned += EnforceRegionAttachesForCabin(batch[i], target_ctx,
                                                           held_l1, revoke);
        }
        /* MEMTAG_REGION_MAX_CAP holds ids under 2^20, so the increment
         * cannot wrap; the guard is here so raising that cap can never
         * silently turn an overflow into an endless walk. */
        if (batch[nr - 1] == UINT32_MAX) break;
        cursor = batch[nr - 1] + 1u;
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
    size_t n = ctx ? EnforceForCabinTag(ctx, process_active_memtags(proc),
                                        tag_id, true) : 0;
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
    size_t n = ctx ? EnforceForCabinTag(ctx, process_active_memtags(proc),
                                        tag_id, false) : 0;
    process_ref_dec(proc);
    return n;
}

/* ─── Global sweep on SetGuard flip ───────────────────────────────── */

size_t MemTagSweepGuard(uint16_t tag_id, bool guard_on) {
    if (!g_memtag_initialized) return 0;
    if (tag_id == MEMTAG_INVALID_TAG_ID) return 0;

    /* Walk the pid SPACE, not a sample of the pid set.
     *
     * pid_alloc hands out index+1 over exactly MAX_PROCESSES slots
     * (allocator/pid_allocator.h), so 1..MAX_PROCESSES is every pid that
     * can exist — the shelf itself. Walking it needs no buffer, so there
     * is no cap to be enforced-on-a-sample behind, and no pid is ever
     * carried across a gap: each one is resolved to a ref'd process_t at
     * the moment it is used. That closes the re-let race a pid LIST has —
     * a snapshotted pid whose process dies and whose slot is re-issued
     * before the walk reaches it would otherwise silently redirect one
     * cabin's enforcement at another cabin's address space. Here the
     * held set and the context always come from the same process_t under
     * the same ref, so the pair can never be mismatched.
     *
     * Liveness is process_find_ref plus a non-NULL cabin — the same
     * definition MemCabinGrant / MemCabinRevoke / MemRegionAccessAllowed
     * already enforce on. Empty slots cost one hash miss each, and this
     * runs only when MemTagSetGuard actually flips a tag's guard bit. */
    size_t total = 0;
    for (uint32_t pid = 1; pid <= MAX_PROCESSES; pid++) {
        process_t *proc = process_find_ref(pid);
        if (!proc) continue;
        vmm_context_t *ctx = (vmm_context_t *)process_get_cabin(proc);
        if (ctx) {
            total += EnforceForCabinTag(ctx, process_active_memtags(proc),
                                        tag_id, guard_on);
        }
        process_ref_dec(proc);
    }
    return total;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Phase 2H+ — Tag-driven PKU PTE stamping
 *
 *  See memtag.h "Phase 2H+" block for the spec + policy citations.
 *  Auto-stamp on attach + sweep on tag mutation. Every key 0..15 is
 *  stampable — the PTE field is four plain bits (Vol 3A Table 4-19).
 * ═══════════════════════════════════════════════════════════════════ */

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

/* Stamp PTE.PKEY bits 62:59 atomically. All four bits are the key, on
 * every machine, whatever CR4.CET says (Intel SDM Vol 3A Table 4-19 —
 * the field is a plain 4-bit protection key; the bit-60 shadow-stack
 * meaning belongs to EPT entries, Vol 3C §29.3.2). No key value is
 * refused: keys 2, 3, 6, 7, 10, 11, 14, 15 set bit 60 because that is
 * what those keys ARE.
 *
 * Returns true if any bit was actually toggled (false on idempotent
 * stamp matching current value, or on absent PTE). */
static bool StampPtePkeyLeaf(vmm_context_t *ctx, uintptr_t va,
                              uint8_t pkey, bool is_2m) {
    uint8_t level = 0;
    pte_t *pte = vmm_get_leaf_pte(ctx, va, &level);
    if (!pte) return false;
    if (is_2m  && level != 2) return false;
    if (!is_2m && level != 1) return false;

    uint64_t new_pkey_field = vmm_pte_encode_pkey(pkey);
    pte_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    for (;;) {
        pte_t new_val = (old & ~VMM_PTE_PKEY_MASK) | new_pkey_field;
        if (new_val == old) return true;     /* already stamped */
        if (__atomic_compare_exchange_n(pte, &old, new_val, false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
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
    uint16_t buf[MEMTAG_GUARDS_PER_REGION];
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

    /* Streams the attach chain for the same reason the guard enforcement
     * does: a key stamped on some of a region's mappings and not the
     * others is a region whose pages disagree about which PKRU pair
     * gates them. Every attach gets the stamp or none does. The PTE work
     * and the shootdown run with the bucket lock released. */
    size_t total = 0;
    MemRegionAttach batch[MEMTAG_WALK_ATTACHES_BATCH];
    uintptr_t   cur_va  = 0;
    const void *cur_ctx = NULL;
    for (;;) {
        size_t n = MemRegionRegistryWalkAttachs(&g_region_registry, region_id,
                                                 cur_va, cur_ctx, batch,
                                                 MEMTAG_WALK_ATTACHES_BATCH);
        if (n == 0) break;
        for (size_t i = 0; i < n; i++) {
            MemRegionAttach *att = &batch[i];
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
        cur_va  = batch[n - 1].va_base;
        cur_ctx = batch[n - 1].ctx;
    }
    return total;
}

error_t MemTagApplyPkey(uint32_t region_id, uint8_t pkey) {
    if (!g_memtag_initialized) return ERR_NOT_INITIALIZED;
    if (region_id == MEMTAG_INVALID_REGION_ID) return ERR_INVALID_ARGUMENT;
    if (pkey >= MEMTAG_PKU_MAX_KEYS) return ERR_INVALID_ARGUMENT;

    /* Find + clear any existing pku:N tag first so the region carries
     * at most one. Walk region's tag list, drop any pku:* we find. */
    uint16_t buf[MEMTAG_GUARDS_PER_REGION];
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

/* Phase 2E originally implemented PAT MSR consistency probe + MTRR
 * audit here. The audit pass after Phase 2K moved them to vmm.c
 * (vmm_verify_pat_msr / vmm_dump_mtrr_layout) because they are pure
 * CPU MSR probes with no MemTag-specific state. The names below are
 * retained as thin wrappers for backward source compatibility — they
 * just forward to the VMM implementations.
 *
 * The two verdicts that matter — MTRRs disabled, or a non-WB default
 * type — now speak through kprintf inside vmm_dump_mtrr_layout, not
 * debug_printf. Effective memory type is MTRR combined with PAT (Intel SDM
 * Vol 3A §11.5.2.2, Table 11-7 — not §11.12.5, which is PAT compatibility
 * with earlier processors), and for a PAT write-back page that combination
 * is the MTRR type itself, so either condition silently demotes every
 * cache:wb tag this module hands out — while a cache:wc tag survives both,
 * WC being the one column the table gives under every MTRR type. A build
 * that ships compiles debug_printf into nothing. */
bool MemTagVerifyPatMsr(void)  { return vmm_verify_pat_msr(); }
void MemTagDumpMtrrLayout(void) { vmm_dump_mtrr_layout(); }
