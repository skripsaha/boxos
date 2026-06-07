/*
 * MCE Page Migration — implementation
 *
 * See mce_migrate.h for design + race analysis. This file contains:
 *   1. Static request ring + per-CPU nesting state.
 *   2. mce_migrate_request — IRQ-safe producer (called from #MC IST).
 *   3. mce_migrate_worker — K-Core consumer (called via irq_defer).
 *   4. mce_migrate_perform — the actual region lookup, copy, PTE swap.
 *   5. mce_safe_page_copy — 64 B chunked copy with abort flag.
 *   6. mce_swap_pte — atomic CAS on leaf PTE preserving metadata bits.
 *   7. mce_migrate_note_nested — called from outer mce_handle.
 */

#include "mce_migrate.h"
#include "mce.h"
#include "klib.h"
#include "amp.h"
#include "memtag.h"
#include "region_registry.h"
#include "vmm.h"
#include "pmm.h"
#include "touch.h"
#include "irq_defer.h"

/* ─── Request ring (static — no allocation in IRQ context) ──────── */

#define MCE_MIGRATE_RING_SIZE   32u
#define MCE_MIGRATE_RING_MASK   (MCE_MIGRATE_RING_SIZE - 1u)

typedef struct {
    uintptr_t       phys;
    uint64_t        status;
    uint16_t        severity;       /* mce_severity_t */
    uint16_t        rsv0;
    uint32_t        rsv1;
    volatile uint32_t in_use;       /* 0 = free, 1 = in-flight */
} MceMigrateSlot;

static MceMigrateSlot   g_ring[MCE_MIGRATE_RING_SIZE];
static volatile uint32_t g_ring_cursor = 0;

/* ─── Per-CPU nesting state (for safe-copy) ─────────────────────── */
/*
 * g_in_migration[cpu] holds the phys currently being copied on `cpu`,
 * or 0 when no migration is active on that CPU. mce_handle's outer
 * entry calls mce_migrate_note_nested which checks this; if it matches
 * the nested-#MC's phys, we set g_migration_aborted[cpu]=true.
 *
 * The copy loop checks the abort flag between 64 B chunks and zero-
 * fills the remainder on observed abort. We size the per-CPU arrays
 * by MAX_CORES (256, from amp.h) for full coverage without a per-CPU
 * heap allocation.
 */
#define MCE_MIGRATE_MAX_CORES   256u

static volatile uintptr_t g_in_migration[MCE_MIGRATE_MAX_CORES];
static volatile uint32_t  g_migration_aborted[MCE_MIGRATE_MAX_CORES];

/* ─── Telemetry (RELAXED counters — never load-bearing) ─────────── */

static volatile uint64_t g_stat_requests       = 0;
static volatile uint64_t g_stat_drops          = 0;
static volatile uint64_t g_stat_completed      = 0;
static volatile uint64_t g_stat_failed         = 0;
static volatile uint64_t g_stat_no_owner       = 0;
static volatile uint64_t g_stat_nested_aborts  = 0;
static volatile uint64_t g_stat_cabins_touched = 0;
static volatile uint64_t g_stat_pages_migrated = 0;
static volatile uint64_t g_stat_skipped_2m     = 0;

/* ─── Touch tag handles (resolved once at init) ─────────────────── */

static TouchTag g_tag_completed = TOUCH_TAG_INVALID;
static TouchTag g_tag_failed    = TOUCH_TAG_INVALID;
static TouchTag g_tag_unmapped  = TOUCH_TAG_INVALID;

static volatile bool g_initialized = false;

/* ─── Per-migration Touch payload (fits 64 B Pocket envelope) ───── */

typedef struct {
    uint64_t old_phys;          /* 8 */
    uint64_t new_phys;          /* 8 */
    uint64_t status;            /* 8 — MCx_STATUS for forensics */
    uint64_t pages_migrated;    /* 8 — always 1 in Phase 1 */
    uint32_t cabins_touched;    /* 4 */
    uint32_t region_id;         /* 4 */
    uint8_t  severity;          /* 1 */
    uint8_t  copy_clean;        /* 1 — 0 if nested-#MC aborted partial copy */
    uint8_t  pages_skipped_2m;  /* 1 */
    uint8_t  pad8;              /* 1 */
    uint8_t  pad[20];           /* 20 → total = 64 B */
} mce_migrate_payload_t;

_Static_assert(sizeof(mce_migrate_payload_t) == 64,
               "mce_migrate_payload must fit 64 B Pocket envelope");

/* ─── Bounded snapshot buffer for attach chain ──────────────────── */

#define MCE_MIGRATE_ATTACHES_PER_CALL  32u

/* ─── Init ──────────────────────────────────────────────────────── */

bool mce_migrate_is_initialized(void) {
    return g_initialized;
}

void mce_migrate_init(void) {
    if (g_initialized) return;

    for (uint32_t i = 0; i < MCE_MIGRATE_RING_SIZE; i++) {
        __atomic_store_n(&g_ring[i].in_use, 0, __ATOMIC_RELAXED);
    }
    for (uint32_t c = 0; c < MCE_MIGRATE_MAX_CORES; c++) {
        __atomic_store_n(&g_in_migration[c],      0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_migration_aborted[c], 0, __ATOMIC_RELAXED);
    }

    /* Resolve Touch tag handles upfront. TouchTagIntern is OK in normal
     * kernel context but not in IRQ — we cache here so the deferred
     * worker can publish without a registry lookup, matching the Phase
     * 2F pattern used by mce.c for the IRQ-side tags. */
    g_tag_completed = TouchTagIntern("mce:migration:completed");
    g_tag_failed    = TouchTagIntern("mce:migration:failed");
    g_tag_unmapped  = TouchTagIntern("mce:migration:unmapped");

    debug_printf("[MCE] migrate init: ring=%u tags{completed=0x%x failed=0x%x "
                 "unmapped=0x%x}\n",
                 (unsigned)MCE_MIGRATE_RING_SIZE,
                 (unsigned)g_tag_completed,
                 (unsigned)g_tag_failed,
                 (unsigned)g_tag_unmapped);

    g_initialized = true;
}

/* ─── Atomic PTE swap ────────────────────────────────────────────── */
/*
 * Replace PHYS bits of the leaf PTE at (ctx, va) with `new_phys`,
 * preserving everything else: low-12 flag bits, NX (bit 63), Phase 2D
 * region_id (bits 52-58), Phase 2H PKEY (bits 62:59), Phase 2K CET
 * supv-SS (bit 60 when CR4.CET=1). Returns true on swap, false if
 * the PTE no longer exists (vmm_unmap raced) or is a 2 MiB leaf
 * (Phase 1 migration is 4 KiB-only — log via stats and let caller
 * decide).
 */
static bool mce_swap_pte(void *ctx_ptr, uintptr_t va, uintptr_t new_phys,
                          bool *out_was_2m) {
    vmm_context_t *ctx = (vmm_context_t *)ctx_ptr;
    if (!ctx) return false;
    if (out_was_2m) *out_was_2m = false;

    uint8_t level = 0;
    pte_t *pte = vmm_get_leaf_pte(ctx, va, &level);
    if (!pte) return false;

    if (level != 1) {
        /* 2 MiB / 1 GiB leaf. Splitting a huge page to migrate one 4 KiB
         * subrange is out of scope for Phase 1 — the caller logs this
         * via stats and leaves the original PTE alone (process will #PF
         * if it touches the poisoned subrange and be killed cleanly). */
        if (out_was_2m) *out_was_2m = true;
        return false;
    }

    const uint64_t addr_mask = vmm_get_addr_mask();
    pte_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    for (;;) {
        if (!(old & VMM_FLAG_PRESENT)) return false;
        pte_t new_val = (old & ~addr_mask) | (new_phys & addr_mask);
        if (__atomic_compare_exchange_n(pte, &old, new_val, false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            return true;
        }
        /* CAS failed; `old` now reflects the latest PTE — retry. */
    }
}

/* ─── Safe-copy with nested-#MC detection ───────────────────────── */
/*
 * Copy PMM_PAGE_SIZE bytes from old_phys to new_phys in 64-byte chunks.
 * Before starting, publish g_in_migration[cpu] = old_phys so a nested
 * #MC hitting the bad cache line can flag the copy as aborted. On
 * abort, zero-fill remaining bytes and return false; caller still
 * installs the new PTE (process keeps running with partial-zero data).
 */
static bool mce_safe_page_copy(uintptr_t old_phys, uintptr_t new_phys) {
    void *old_va = vmm_phys_to_virt(old_phys);
    void *new_va = vmm_phys_to_virt(new_phys);
    if (!old_va || !new_va) return false;

    uint8_t cpu = amp_get_core_index();
    if (cpu >= MCE_MIGRATE_MAX_CORES) return false;

    __atomic_store_n(&g_migration_aborted[cpu], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_in_migration[cpu], old_phys, __ATOMIC_RELEASE);

    bool clean = true;
    const size_t LINE = 64u;
    uint8_t *src = (uint8_t *)old_va;
    uint8_t *dst = (uint8_t *)new_va;

    for (size_t off = 0; off < PMM_PAGE_SIZE; off += LINE) {
        if (__atomic_load_n(&g_migration_aborted[cpu], __ATOMIC_ACQUIRE)) {
            /* Nested #MC observed — bail and zero the rest. */
            __builtin_memset(dst + off, 0, PMM_PAGE_SIZE - off);
            clean = false;
            __atomic_fetch_add(&g_stat_nested_aborts, 1, __ATOMIC_RELAXED);
            break;
        }
        __builtin_memcpy(dst + off, src + off, LINE);
    }

    __atomic_store_n(&g_in_migration[cpu], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_migration_aborted[cpu], 0, __ATOMIC_RELEASE);
    return clean;
}

/* ─── Core migration routine (runs in K-Core context) ───────────── */

static uint32_t mce_migrate_perform(uintptr_t phys, mce_severity_t sev,
                                     uint64_t status) {
    if (!g_initialized) return 0;
    if (phys == 0 || (phys & (PMM_PAGE_SIZE - 1)) != 0) {
        __atomic_fetch_add(&g_stat_failed, 1, __ATOMIC_RELAXED);
        return 0;
    }

    /* 1. Reverse-map phys → region_id (O(1) via id_by_page). */
    uint32_t region_id = MemRegionFromPhys(phys);
    if (region_id == MEMTAG_INVALID_REGION_ID) {
        __atomic_fetch_add(&g_stat_no_owner, 1, __ATOMIC_RELAXED);
        mce_migrate_payload_t ev = {0};
        ev.old_phys = phys;
        ev.status   = status;
        ev.severity = (uint8_t)sev;
        if (g_tag_unmapped != TOUCH_TAG_INVALID) {
            TouchPublishId(g_tag_unmapped, &ev, sizeof(ev), 0u, 0u);
        }
        return 0;
    }

    /* 2. Snapshot region descriptor + attach chain. */
    MemRegionRegistry *reg = MemTagGetRegionRegistry();
    if (!reg) {
        __atomic_fetch_add(&g_stat_failed, 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (!MemRegionRegistryIsActive(reg, region_id)) {
        __atomic_fetch_add(&g_stat_no_owner, 1, __ATOMIC_RELAXED);
        return 0;
    }

    MemRegion *region_slot = MemRegionRegistrySlot(reg, region_id);
    if (!region_slot) {
        __atomic_fetch_add(&g_stat_no_owner, 1, __ATOMIC_RELAXED);
        return 0;
    }
    uintptr_t region_base   = region_slot->base_phys;
    size_t    region_pages  = region_slot->pages;
    uintptr_t region_end    = region_base + region_pages * PMM_PAGE_SIZE;

    if (phys < region_base || phys >= region_end) {
        /* Stale id_by_page entry — region was destroyed + recycled
         * between MemRegionFromPhys and now. Bail cleanly. */
        __atomic_fetch_add(&g_stat_no_owner, 1, __ATOMIC_RELAXED);
        return 0;
    }

    MemRegionAttach snap[MCE_MIGRATE_ATTACHES_PER_CALL];
    size_t n_attach = MemRegionRegistrySnapshotAttachs(reg, region_id, snap,
                                                       MCE_MIGRATE_ATTACHES_PER_CALL);
    if (n_attach == 0) {
        /* Region exists but no cabin currently maps it (kernel-only
         * buffer / future-Bay-pre-attach). Nothing to migrate — PMM
         * bitmap already prevents future allocs hitting this page. */
        mce_migrate_payload_t ev = {0};
        ev.old_phys  = phys;
        ev.status    = status;
        ev.region_id = region_id;
        ev.severity  = (uint8_t)sev;
        if (g_tag_unmapped != TOUCH_TAG_INVALID) {
            TouchPublishId(g_tag_unmapped, &ev, sizeof(ev), 0u, 0u);
        }
        return 0;
    }

    /* 3. Allocate ONE replacement phys page. Same page for every
     * attached cabin — preserves Bay shared semantics by construction.
     * We use PHYS_TAG_USER (mid zone) which is where typical Bay/Brook
     * buffers live. Falling back to any-zone alloc would still work but
     * crosses NUMA domains. */
    void *new_phys_ptr = pmm_alloc(1, PHYS_TAG_USER);
    if (!new_phys_ptr) {
        /* Try any zone before giving up. */
        new_phys_ptr = pmm_alloc(1);
    }
    if (!new_phys_ptr) {
        __atomic_fetch_add(&g_stat_failed, 1, __ATOMIC_RELAXED);
        mce_migrate_payload_t ev = {0};
        ev.old_phys  = phys;
        ev.status    = status;
        ev.region_id = region_id;
        ev.severity  = (uint8_t)sev;
        if (g_tag_failed != TOUCH_TAG_INVALID) {
            TouchPublishId(g_tag_failed, &ev, sizeof(ev), 0u, 0u);
        }
        return 0;
    }
    uintptr_t new_phys = (uintptr_t)new_phys_ptr;

    /* 4. Safe-copy old → new with nested-#MC abort. */
    bool copy_clean = mce_safe_page_copy(phys, new_phys);

    /* 5. Walk attachments, swap PTEs in-place. Same new_phys for all
     * (Bay-correct). Per-attach state machine: 2 MiB leaves are
     * counted but skipped (Phase 1 only handles 4 KiB). */
    size_t page_index = (phys - region_base) / PMM_PAGE_SIZE;
    uint32_t cabins_touched = 0;
    uint32_t skipped_2m = 0;

    for (size_t i = 0; i < n_attach; i++) {
        MemRegionAttach *att = &snap[i];
        if (!att->ctx) continue;

        /* Phase 1 only migrates 4 KiB attaches. 2 MiB leaves require
         * page-table splitting (split 2 MiB PD-leaf into 512×4 KiB PTs,
         * then swap one), which is non-trivial and out-of-scope. */
        if (att->page_class == MEMTAG_ATTACH_CLASS_2M) {
            skipped_2m++;
            continue;
        }
        if (att->state != MEMTAG_ATTACH_ACTIVE) continue;
        if (page_index >= att->pages) continue;       /* not in this attach */

        uintptr_t target_va = att->va_base + page_index * PMM_PAGE_SIZE;

        bool was_2m = false;
        if (mce_swap_pte(att->ctx, target_va, new_phys, &was_2m)) {
            vmm_shootdown_pages((vmm_context_t *)att->ctx, target_va, 1);
            cabins_touched++;
        } else if (was_2m) {
            skipped_2m++;
        }
    }

    if (skipped_2m) {
        __atomic_fetch_add(&g_stat_skipped_2m, skipped_2m, __ATOMIC_RELAXED);
    }

    if (cabins_touched == 0) {
        /* Nothing swapped — return the unused replacement. */
        pmm_free(new_phys_ptr, 1);
        __atomic_fetch_add(&g_stat_failed, 1, __ATOMIC_RELAXED);
        mce_migrate_payload_t ev = {0};
        ev.old_phys         = phys;
        ev.new_phys         = 0;
        ev.status           = status;
        ev.region_id        = region_id;
        ev.severity         = (uint8_t)sev;
        ev.copy_clean       = copy_clean ? 1 : 0;
        ev.pages_skipped_2m = (uint8_t)(skipped_2m > 255 ? 255 : skipped_2m);
        if (g_tag_failed != TOUCH_TAG_INVALID) {
            TouchPublishId(g_tag_failed, &ev, sizeof(ev), 0u, 0u);
        }
        return 0;
    }

    /* 6. Publish completed event. */
    __atomic_fetch_add(&g_stat_completed,      1,              __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_stat_pages_migrated, 1,              __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_stat_cabins_touched, cabins_touched, __ATOMIC_RELAXED);

    mce_migrate_payload_t ev = {0};
    ev.old_phys         = phys;
    ev.new_phys         = new_phys;
    ev.status           = status;
    ev.pages_migrated   = 1;
    ev.cabins_touched   = cabins_touched;
    ev.region_id        = region_id;
    ev.severity         = (uint8_t)sev;
    ev.copy_clean       = copy_clean ? 1 : 0;
    ev.pages_skipped_2m = (uint8_t)(skipped_2m > 255 ? 255 : skipped_2m);
    if (g_tag_completed != TOUCH_TAG_INVALID) {
        TouchPublishId(g_tag_completed, &ev, sizeof(ev), 0u, 0u);
    }

    debug_printf("[MCE] migrated phys=0x%lx → 0x%lx region=%u cabins=%u "
                 "copy_clean=%d skipped_2m=%u\n",
                 (unsigned long)phys, (unsigned long)new_phys,
                 (unsigned)region_id, (unsigned)cabins_touched,
                 (int)copy_clean, (unsigned)skipped_2m);

    return cabins_touched;
}

/* ─── irq_defer worker (runs in K-Core context) ─────────────────── */

static void mce_migrate_worker(void *slot_ptr) {
    MceMigrateSlot *slot = (MceMigrateSlot *)slot_ptr;

    /* Copy parameters before releasing the slot so back-to-back IRQ
     * producers can reuse it immediately. */
    uintptr_t phys     = slot->phys;
    uint64_t  status   = slot->status;
    uint16_t  severity = slot->severity;

    __atomic_store_n(&slot->in_use, 0, __ATOMIC_RELEASE);

    (void)mce_migrate_perform(phys, (mce_severity_t)severity, status);
}

/* ─── Public API ────────────────────────────────────────────────── */

bool mce_migrate_request(uintptr_t phys, mce_severity_t sev, uint64_t status) {
    if (!g_initialized) return false;
    if (phys == 0) return false;

    /* Claim a slot from the static ring. Producer rolls a bounded
     * fetch_add cursor and tries to CAS in_use from 0 → 1. On contention
     * (ring saturation) we walk a bounded number of slots before giving
     * up — never block, never panic. The drop is counted in stats so
     * operators see when MCE event rate exceeds the slot budget. */
    uint32_t start = __atomic_fetch_add(&g_ring_cursor, 1, __ATOMIC_RELAXED);
    for (uint32_t k = 0; k < MCE_MIGRATE_RING_SIZE; k++) {
        uint32_t idx = (start + k) & MCE_MIGRATE_RING_MASK;
        MceMigrateSlot *slot = &g_ring[idx];
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&slot->in_use, &expected, 1, false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_RELAXED)) {
            slot->phys     = phys;
            slot->status   = status;
            slot->severity = (uint16_t)sev;
            slot->rsv0     = 0;
            slot->rsv1     = 0;

            irq_defer(mce_migrate_worker, slot);
            __atomic_fetch_add(&g_stat_requests, 1, __ATOMIC_RELAXED);
            return true;
        }
    }

    /* Ring saturation — drop the request. Phase 2F already poisoned the
     * page so future allocations skip it; the process that owns the
     * page will eventually #PF on access and be killed cleanly. */
    __atomic_fetch_add(&g_stat_drops, 1, __ATOMIC_RELAXED);
    return false;
}

bool mce_migrate_note_nested(uintptr_t phys) {
    if (!g_initialized) return false;
    uint8_t cpu = amp_get_core_index();
    if (cpu >= MCE_MIGRATE_MAX_CORES) return false;
    uintptr_t in_flight = __atomic_load_n(&g_in_migration[cpu], __ATOMIC_ACQUIRE);
    if (in_flight == 0) return false;
    if ((in_flight & ~(uintptr_t)(PMM_PAGE_SIZE - 1)) !=
        (phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1))) return false;
    __atomic_store_n(&g_migration_aborted[cpu], 1, __ATOMIC_RELEASE);
    return true;
}

uint32_t mce_migrate_run_sync(uintptr_t phys) {
    /* Tests bypass the irq_defer hop. Same perform() — different entry. */
    return mce_migrate_perform(phys, MCE_SEV_UCR, 0ULL);
}

/* ─── Telemetry ─────────────────────────────────────────────────── */

void mce_migrate_get_stats(mce_migrate_stats_t *out) {
    if (!out) return;
    out->requests       = __atomic_load_n(&g_stat_requests,       __ATOMIC_RELAXED);
    out->drops          = __atomic_load_n(&g_stat_drops,          __ATOMIC_RELAXED);
    out->completed      = __atomic_load_n(&g_stat_completed,      __ATOMIC_RELAXED);
    out->failed         = __atomic_load_n(&g_stat_failed,         __ATOMIC_RELAXED);
    out->no_owner       = __atomic_load_n(&g_stat_no_owner,       __ATOMIC_RELAXED);
    out->nested_aborts  = __atomic_load_n(&g_stat_nested_aborts,  __ATOMIC_RELAXED);
    out->cabins_touched = __atomic_load_n(&g_stat_cabins_touched, __ATOMIC_RELAXED);
    out->pages_migrated = __atomic_load_n(&g_stat_pages_migrated, __ATOMIC_RELAXED);
    out->skipped_2m     = __atomic_load_n(&g_stat_skipped_2m,     __ATOMIC_RELAXED);
}

void mce_migrate_dump(void) {
    mce_migrate_stats_t s;
    mce_migrate_get_stats(&s);
    debug_printf("[MCE] migrate stats: req=%lu drop=%lu done=%lu fail=%lu "
                 "no_owner=%lu nested=%lu cabins=%lu pages=%lu skip2m=%lu\n",
                 (unsigned long)s.requests,
                 (unsigned long)s.drops,
                 (unsigned long)s.completed,
                 (unsigned long)s.failed,
                 (unsigned long)s.no_owner,
                 (unsigned long)s.nested_aborts,
                 (unsigned long)s.cabins_touched,
                 (unsigned long)s.pages_migrated,
                 (unsigned long)s.skipped_2m);
}
