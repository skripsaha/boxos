
#include "mce_migrate.h"
#include "mce.h"
#include "klib.h"
#include "amp.h"
#include "memtag.h"
#include "region_registry.h"
#include "vmm.h"
#include "pmm.h"
#include "touch.h"
#include "logbook.h"
#include "baton.h"


#define MCE_MIGRATE_RING_SIZE   32u
#define MCE_MIGRATE_RING_MASK   (MCE_MIGRATE_RING_SIZE - 1u)

typedef struct {
    uintptr_t       phys;
    uint64_t        status;
    uint16_t        severity;
    uint16_t        rsv0;
    uint32_t        rsv1;
    volatile uint32_t in_use;
    Baton           baton;
} MceMigrateSlot;

static MceMigrateSlot   g_ring[MCE_MIGRATE_RING_SIZE];
static volatile uint32_t g_ring_cursor = 0;

#define MCE_MIGRATE_MAX_CORES   256u

static volatile uintptr_t g_in_migration[MCE_MIGRATE_MAX_CORES];
static volatile uint32_t  g_migration_aborted[MCE_MIGRATE_MAX_CORES];


static volatile uint64_t g_stat_requests       = 0;
static volatile uint64_t g_stat_drops          = 0;
static volatile uint64_t g_stat_completed      = 0;
static volatile uint64_t g_stat_failed         = 0;
static volatile uint64_t g_stat_no_owner       = 0;
static volatile uint64_t g_stat_nested_aborts  = 0;
static volatile uint64_t g_stat_cabins_touched = 0;
static volatile uint64_t g_stat_pages_migrated = 0;
static volatile uint64_t g_stat_skipped_2m     = 0;


static TouchTag g_tag_completed = TOUCH_TAG_INVALID;
static TouchTag g_tag_failed    = TOUCH_TAG_INVALID;
static TouchTag g_tag_unmapped  = TOUCH_TAG_INVALID;

static volatile bool g_initialized = false;


typedef struct {
    uint64_t old_phys;
    uint64_t new_phys;
    uint64_t status;
    uint64_t pages_migrated;
    uint32_t cabins_touched;
    uint32_t region_id;
    uint8_t  severity;
    uint8_t  copy_clean;
    uint8_t  pages_skipped_2m;
    uint8_t  pad8;
    uint8_t  pad[20];
} mce_migrate_payload_t;

_Static_assert(sizeof(mce_migrate_payload_t) == 64,
               "mce_migrate_payload must fit 64 B Pocket envelope");


#define MCE_MIGRATE_ATTACHES_PER_CALL  32u


bool mce_migrate_is_initialized(void) {
    return g_initialized;
}

static void mce_migrate_worker(void *slot_ptr);

void mce_migrate_init(void) {
    if (g_initialized) return;

    for (uint32_t i = 0; i < MCE_MIGRATE_RING_SIZE; i++) {
        __atomic_store_n(&g_ring[i].in_use, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_ring[i].baton.next, NULL, __ATOMIC_RELAXED);
        g_ring[i].baton.run = mce_migrate_worker;
        g_ring[i].baton.ctx = &g_ring[i];
    }
    for (uint32_t c = 0; c < MCE_MIGRATE_MAX_CORES; c++) {
        __atomic_store_n(&g_in_migration[c],      0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_migration_aborted[c], 0, __ATOMIC_RELAXED);
    }

    g_tag_completed = TouchLogbookIntern("mce:migration:completed");
    g_tag_failed    = TouchLogbookIntern("mce:migration:failed");
    g_tag_unmapped  = TouchLogbookIntern("mce:migration:unmapped");

    debug_printf("[MCE] migrate init: ring=%u tags{completed=0x%x failed=0x%x "
                 "unmapped=0x%x}\n",
                 (unsigned)MCE_MIGRATE_RING_SIZE,
                 (unsigned)g_tag_completed,
                 (unsigned)g_tag_failed,
                 (unsigned)g_tag_unmapped);

    g_initialized = true;
}

static bool mce_swap_pte(void *ctx_ptr, uintptr_t va, uintptr_t new_phys,
                          bool *out_was_2m) {
    vmm_context_t *ctx = (vmm_context_t *)ctx_ptr;
    if (!ctx) return false;
    if (out_was_2m) *out_was_2m = false;

    uint8_t level = 0;
    pte_t *pte = vmm_get_leaf_pte(ctx, va, &level);
    if (!pte) return false;

    if (level != 1) {
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
    }
}

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


static uint32_t mce_migrate_perform(uintptr_t phys, mce_severity_t sev,
                                     uint64_t status) {
    if (!g_initialized) return 0;
    if (phys == 0 || (phys & (PMM_PAGE_SIZE - 1)) != 0) {
        __atomic_fetch_add(&g_stat_failed, 1, __ATOMIC_RELAXED);
        return 0;
    }

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
        __atomic_fetch_add(&g_stat_no_owner, 1, __ATOMIC_RELAXED);
        return 0;
    }

    MemRegionAttach snap[MCE_MIGRATE_ATTACHES_PER_CALL];
    size_t n_attach = MemRegionRegistrySnapshotAttachs(reg, region_id, snap,
                                                       MCE_MIGRATE_ATTACHES_PER_CALL);
    if (n_attach == 0) {
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

    void *new_phys_ptr = pmm_alloc(1, PHYS_TAG_USER);
    if (!new_phys_ptr) {
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

    bool copy_clean = mce_safe_page_copy(phys, new_phys);

    size_t page_index = (phys - region_base) / PMM_PAGE_SIZE;
    uint32_t cabins_touched = 0;
    uint32_t skipped_2m = 0;

    for (size_t i = 0; i < n_attach; i++) {
        MemRegionAttach *att = &snap[i];
        if (!att->ctx) continue;

        if (att->page_class == MEMTAG_ATTACH_CLASS_2M) {
            skipped_2m++;
            continue;
        }
        if (att->state != MEMTAG_ATTACH_ACTIVE) continue;
        if (page_index >= att->pages) continue;

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


static uint64_t g_stat_drops_said = 0;

static void mce_migrate_worker(void *slot_ptr) {
    MceMigrateSlot *slot = (MceMigrateSlot *)slot_ptr;

    uint64_t drops = __atomic_load_n(&g_stat_drops, __ATOMIC_RELAXED);
    if (drops != g_stat_drops_said) {
        kprintf("[MCE] ERROR: %lu page migration request(s) found no free slot — "
                "the machine's errors outran the K-Core by a whole pool of %u; "
                "those pages stay poisoned and unmigrated, and their owners "
                "will fault on them\n",
                (unsigned long)(drops - g_stat_drops_said),
                (unsigned)MCE_MIGRATE_RING_SIZE);
        g_stat_drops_said = drops;
    }

    uintptr_t phys     = slot->phys;
    uint64_t  status   = slot->status;
    uint16_t  severity = slot->severity;

    __atomic_store_n(&slot->in_use, 0, __ATOMIC_RELEASE);

    (void)mce_migrate_perform(phys, (mce_severity_t)severity, status);
}


bool mce_migrate_request(uintptr_t phys, mce_severity_t sev, uint64_t status) {
    if (!g_initialized) return false;
    if (phys == 0) return false;

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

            BatonPass(&slot->baton);
            __atomic_fetch_add(&g_stat_requests, 1, __ATOMIC_RELAXED);
            return true;
        }
    }

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
    return mce_migrate_perform(phys, MCE_SEV_UCR, 0ULL);
}


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