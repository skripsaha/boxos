/*
 * MemTag — Region Registry
 *
 * Mirror of TagFS file_table conceptually, but for RAM areas instead of disk
 * files. Each region is a contiguous (base_phys, pages) range with optional
 * (base_virt, ctx) for cabin-owned regions. Multi-tag via sorted tag_ids[]
 * array (TagFS-style).
 *
 * O(1) lookups via dense indirection:
 *   - region_id → MemRegion*           : slot array indexed by region_id
 *   - phys_addr → region_id            : id_by_page dense array (4B per 4KB)
 *   - virt_addr (current cabin) → id   : vmm walk + phys lookup
 *
 * Dynamic growth: slots/free-stack grow on demand (start 1024, double).
 * id_by_page is sized to PMM mem_end at boot and immutable thereafter.
 *
 * AMP-safe: per-bucket spinlocks (region_id % MEMTAG_REGION_BUCKETS) for
 * mutation; reads of base/pages are lock-free when region is active (see
 * MemRegionInfo snapshot pattern).
 */

#ifndef MEMTAG_REGION_REGISTRY_H
#define MEMTAG_REGION_REGISTRY_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"
#include "tag_registry.h"

#define MEMTAG_INVALID_REGION_ID    0xFFFFFFFFu
#define MEMTAG_REGION_INITIAL_CAP   1024u
#define MEMTAG_REGION_MAX_CAP       (1u << 20)   /* 1M regions hard ceiling */
#define MEMTAG_REGION_BUCKETS       256u
#define MEMTAG_REGION_TAGS_INITIAL  4u           /* per-region tag_ids[] start */

#define MEMTAG_REGION_FLAG_ACTIVE       0x0001
#define MEMTAG_REGION_FLAG_PINNED       0x0002   /* DMA / page-table buffers  */
#define MEMTAG_REGION_FLAG_KERNEL       0x0004   /* kernel-owned region       */
#define MEMTAG_REGION_FLAG_CABIN        0x0008   /* user-cabin owned region   */
#define MEMTAG_REGION_FLAG_PHYSICAL     0x0010   /* base_phys is authoritative */
#define MEMTAG_REGION_FLAG_VIRTUAL      0x0020   /* base_virt is authoritative */

/* Per-attach mapping state. Phase 2C uses this to decide whether to
 * restore PTE.P=1 on grant (must be REVOKED to need restore) and whether
 * the attach is already inert (DETACHED — caller already unmapped). */
#define MEMTAG_ATTACH_ACTIVE     0u   /* PTE PRESENT, cabin can access      */
#define MEMTAG_ATTACH_REVOKED    1u   /* PTE PRESENT cleared by enforcement */

/* Page class for attach — selects PT (level 1, 4 KiB) vs PD leaf (level 2,
 * 2 MiB) walking strategy. 1 GiB (level-3 PDPT leaf) intentionally absent;
 * BoxOS does not currently map 1 GiB user pages. */
#define MEMTAG_ATTACH_CLASS_4K   0u
#define MEMTAG_ATTACH_CLASS_2M   1u

/* MemRegionAttach — bookkeeping for "this region is mapped into cabin
 * `ctx` at VA `va_base` for `pages` 4 KiB units". Phase 2C revoke/grant
 * walks this chain to find PTE locations to toggle.
 *
 * Stored as a singly-linked list hanging off MemRegion.attach_head,
 * mutated under the per-region bucket lock. Allocated from kmalloc;
 * freed on detach or region destroy.
 *
 * orig_flags captures the VMM PTE-flag pattern from the original
 * vmm_map call so the restore path can reconstruct the same PTE on
 * grant. We save the FULL VMM_PTE_FLAGS_MASK (low 12 bits + NX bit 63)
 * since all pages in an attach share the same flags by construction. */
typedef struct MemRegionAttach {
    void                     *ctx;            /* vmm_context_t* — cabin owner */
    uintptr_t                 va_base;        /* virtual base in that cabin   */
    uint32_t                  region_id;      /* back-pointer for iteration   */
    uint32_t                  pages;          /* mapping length in 4 KiB pages */
    uint64_t                  orig_flags;     /* VMM_PTE_FLAGS_MASK at attach */
    uint8_t                   page_class;     /* MEMTAG_ATTACH_CLASS_*        */
    uint8_t                   state;          /* MEMTAG_ATTACH_*              */
    uint16_t                  reserved16;
    uint32_t                  reserved32;
    struct MemRegionAttach   *next;
} MemRegionAttach;

typedef struct MemRegion {
    uintptr_t        base_phys;      /* physical base                       */
    uintptr_t        base_virt;      /* virtual base (0 if pure physical)   */
    void            *ctx;            /* vmm_context_t* — cabin owner; NULL=kernel/global */
    size_t           pages;          /* length in 4 KiB pages               */
    uint16_t        *tag_ids;        /* SORTED ascending → O(log K) lookup  */
    uint16_t         tag_count;
    uint16_t         tag_cap;        /* grows as needed                     */
    uint16_t         flags;
    uint32_t         generation;     /* bumped on tag/state change          */
    MemRegionAttach *attach_head;    /* Phase 2C: cabin mapping chain       */
} MemRegion;

typedef struct MemRegionRegistry {
    MemRegion       *slots;           /* dense; index == region_id          */
    uint32_t         slot_cap;
    uint32_t         slot_count;      /* high-water (next_id slot)          */

    uint32_t        *free_stack;      /* reusable region_ids                */
    uint32_t         free_top;
    uint32_t         free_cap;

    /* Dense phys-page → region_id index (O(1) reverse lookup).
     * Allocated from PMM at init (4 B per 4 KiB page). MEMTAG_INVALID_REGION_ID
     * = "no region covers this page". Sized to pmm_get_mem_end().
     *
     * NOTE: pages may be covered by MULTIPLE regions (overlap). The dense
     * array stores the MOST RECENTLY APPLIED region_id; the bitmap inverted
     * index is authoritative for full overlap enumeration. The dense array
     * is the fast-path for the common case (#PF tag-check in Phase 2). */
    uint32_t        *id_by_page;
    size_t           page_count;

    volatile uint64_t generation;     /* bumped on create/destroy           */

    spinlock_t       lock;            /* protects slots / slot_count / free */
    spinlock_t       bucket_locks[MEMTAG_REGION_BUCKETS]; /* per-region mutex */
} MemRegionRegistry;

/* ─── Lifecycle ─────────────────────────────────────────────────────────── */

error_t          MemRegionRegistryInit(MemRegionRegistry *reg, size_t mem_end);
void             MemRegionRegistryShutdown(MemRegionRegistry *reg);

/* ─── Region create / destroy ──────────────────────────────────────────── */

/* Allocate a region_id and populate. Returns INVALID on OOM/cap. */
uint32_t         MemRegionRegistryCreate(MemRegionRegistry *reg,
                                          uintptr_t base_phys,
                                          uintptr_t base_virt,
                                          void *ctx,
                                          size_t pages,
                                          uint16_t flags);

/* Free a region. Releases tag_ids[] and clears id_by_page entries that
 * still point to this region. Caller is responsible for separately removing
 * the region from the tag_bitmap inverted index (see MemTag* public API
 * which does both atomically). */
void             MemRegionRegistryDestroy(MemRegionRegistry *reg, uint32_t region_id);

/* ─── Lookup ──────────────────────────────────────────────────────────── */

bool             MemRegionRegistryIsActive(MemRegionRegistry *reg, uint32_t region_id);

/* O(1) phys → region_id. Returns INVALID if unmapped. */
uint32_t         MemRegionRegistryFromPhys(MemRegionRegistry *reg, uintptr_t phys);

/* Direct slot access — caller must verify active first. Returns NULL on
 * out-of-range region_id. Pointer is stable across non-destroy operations
 * (the slot doesn't move; only its contents may change under bucket lock). */
MemRegion       *MemRegionRegistrySlot(MemRegionRegistry *reg, uint32_t region_id);

/* Snapshot copy (lock-protected). Safe for cross-cabin read. */
typedef struct {
    uintptr_t   base_phys;
    uintptr_t   base_virt;
    size_t      pages;
    uint16_t    tag_count;
    uint16_t    flags;
    uint32_t    generation;
} MemRegionSnapshot;

error_t          MemRegionRegistrySnapshot(MemRegionRegistry *reg,
                                            uint32_t region_id,
                                            MemRegionSnapshot *out);

/* ─── Tag-list mutation (called from memtag.c; bumps region.generation) ─ */

error_t          MemRegionRegistryAddTag(MemRegionRegistry *reg,
                                          uint32_t region_id, uint16_t tag_id);
error_t          MemRegionRegistryRemoveTag(MemRegionRegistry *reg,
                                             uint32_t region_id, uint16_t tag_id);
bool             MemRegionRegistryHasTag(MemRegionRegistry *reg,
                                          uint32_t region_id, uint16_t tag_id);

/* Returns tag_ids[] copy via snapshot (caller-supplied buffer). */
size_t           MemRegionRegistryListTags(MemRegionRegistry *reg,
                                            uint32_t region_id,
                                            uint16_t *out, size_t max);

/* ─── Per-attach mapping registry (Phase 2C enforcement bookkeeping) ─── */

/* Attach a cabin mapping record to `region_id`. Allocates one
 * MemRegionAttach node, links it under bucket lock. Returns OK on success
 * or ERR_NO_MEMORY / ERR_OBJECT_NOT_FOUND. Caller (Bay/Brook map paths)
 * must call this AFTER vmm_map succeeded so the attach reflects a real
 * PTE-present mapping. Multiple attaches per region are allowed (Bay
 * shared across cabins). Duplicate (ctx, va_base) is treated as update
 * — orig_flags refreshed, state reset to ACTIVE. */
error_t          MemRegionRegistryAttach(MemRegionRegistry *reg,
                                          uint32_t region_id,
                                          void *ctx,
                                          uintptr_t va_base,
                                          uint32_t pages,
                                          uint8_t page_class,
                                          uint64_t orig_flags);

/* Remove the attach matching (ctx, va_base). Idempotent: returns OK
 * even if no match exists. Frees the node. Called from Bay/Brook
 * unmap paths just BEFORE vmm_unmap. */
error_t          MemRegionRegistryDetach(MemRegionRegistry *reg,
                                          uint32_t region_id,
                                          void *ctx,
                                          uintptr_t va_base);

/* Snapshot all attaches of `region_id` into a caller buffer. Lock-protected
 * shallow copy of the linked list. Returns the actual count (capped by
 * `max`). Used by enforcement walks that need to drop the bucket lock
 * before doing PTE manipulation + cross-core TLB shootdowns. */
size_t           MemRegionRegistrySnapshotAttachs(MemRegionRegistry *reg,
                                                   uint32_t region_id,
                                                   MemRegionAttach *out,
                                                   size_t max);

/* Update state of a specific attach matching (ctx, va_base). No-op if
 * attach not found (race with detach). Idempotent. */
error_t          MemRegionRegistrySetAttachState(MemRegionRegistry *reg,
                                                  uint32_t region_id,
                                                  void *ctx,
                                                  uintptr_t va_base,
                                                  uint8_t new_state);

/* Detach all entries matching `ctx` regardless of va_base or region.
 * Used by process_destroy to scrub stale attach pointers in case
 * Bay/Brook cleanup somehow missed them. O(R × A_avg). */
size_t           MemRegionRegistryDetachAllForCtx(MemRegionRegistry *reg,
                                                   void *ctx);

/* ─── Statistics ──────────────────────────────────────────────────────── */

uint32_t         MemRegionRegistryActiveCount(MemRegionRegistry *reg);
uint64_t         MemRegionRegistryGeneration(MemRegionRegistry *reg);
void             MemRegionRegistryDump(MemRegionRegistry *reg,
                                        MemTagRegistry *tag_reg);

#endif /* MEMTAG_REGION_REGISTRY_H */
