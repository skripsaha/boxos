/*
 * MemTag — Region Registry Implementation
 *
 * Region slot array + free-id stack + dense phys→region_id index.
 * Per-bucket spinlocks for region mutation; one global lock for slot growth
 * and free-stack pop/push (rare paths).
 *
 * id_by_page is allocated directly from PMM (not kmalloc) because it can be
 * MB-sized on large RAM (4 B per 4 KiB page = 4 MB per 4 GB RAM). Backed via
 * Pull Map (vmm_phys_to_virt) for kernel access.
 */

#include "region_registry.h"
#include "pmm.h"
#include "vmm.h"

/* ─── Internal helpers ────────────────────────────────────────────────── */

static inline spinlock_t *BucketLock(MemRegionRegistry *reg, uint32_t id) {
    return &reg->bucket_locks[id % MEMTAG_REGION_BUCKETS];
}

static error_t EnsureSlotCapacity(MemRegionRegistry *reg, uint32_t needed_id) {
    /* Caller must hold reg->lock. Grows slots[] / free_stack[] together. */
    if (needed_id < reg->slot_cap) return OK;
    if (needed_id >= MEMTAG_REGION_MAX_CAP) {
        debug_printf("[MEMTAG/RGN] cap exhausted (max=%u)\n", MEMTAG_REGION_MAX_CAP);
        return ERR_NO_MEMORY;
    }

    uint32_t new_cap = reg->slot_cap * 2;
    if (new_cap <= needed_id) new_cap = needed_id + 1;
    if (new_cap > MEMTAG_REGION_MAX_CAP) new_cap = MEMTAG_REGION_MAX_CAP;

    MemRegion *new_slots = (MemRegion *)kmalloc(sizeof(MemRegion) * new_cap);
    if (!new_slots) return ERR_NO_MEMORY;
    memcpy(new_slots, reg->slots, sizeof(MemRegion) * reg->slot_cap);
    memset(new_slots + reg->slot_cap, 0,
           sizeof(MemRegion) * (new_cap - reg->slot_cap));

    uint32_t *new_free = (uint32_t *)kmalloc(sizeof(uint32_t) * new_cap);
    if (!new_free) {
        kfree(new_slots);
        return ERR_NO_MEMORY;
    }
    memcpy(new_free, reg->free_stack, sizeof(uint32_t) * reg->free_top);

    kfree(reg->slots);
    kfree(reg->free_stack);
    reg->slots      = new_slots;
    reg->free_stack = new_free;
    reg->slot_cap   = new_cap;
    reg->free_cap   = new_cap;
    return OK;
}

/* Walk id_by_page[first..last) and write `val`. Bounded by registry's
 * page_count — out-of-range pages (e.g. firmware MMIO above mem_end) are
 * silently skipped. */
static void IdByPageFill(MemRegionRegistry *reg,
                          uintptr_t base_phys, size_t pages, uint32_t val) {
    if (!reg->id_by_page) return;
    size_t first = base_phys / PMM_PAGE_SIZE;
    size_t last  = first + pages;
    if (first >= reg->page_count) return;
    if (last > reg->page_count) last = reg->page_count;
    for (size_t p = first; p < last; p++) {
        reg->id_by_page[p] = val;
    }
}

/* Only clear pages still mapped to THIS region (don't clobber overlapping
 * later registration). */
static void IdByPageClearMatching(MemRegionRegistry *reg,
                                   uintptr_t base_phys, size_t pages,
                                   uint32_t expect_id) {
    if (!reg->id_by_page) return;
    size_t first = base_phys / PMM_PAGE_SIZE;
    size_t last  = first + pages;
    if (first >= reg->page_count) return;
    if (last > reg->page_count) last = reg->page_count;
    for (size_t p = first; p < last; p++) {
        if (reg->id_by_page[p] == expect_id)
            reg->id_by_page[p] = MEMTAG_INVALID_REGION_ID;
    }
}

/* Binary search for tag_id in sorted tag_ids[]. Returns insert position
 * (== count if larger than all) AND sets *found. */
static uint16_t TagSearch(const uint16_t *ids, uint16_t count,
                           uint16_t tag_id, bool *found) {
    *found = false;
    uint16_t lo = 0, hi = count;
    while (lo < hi) {
        uint16_t mid = (lo + hi) / 2;
        if (ids[mid] < tag_id)      lo = mid + 1;
        else if (ids[mid] > tag_id) hi = mid;
        else { *found = true; return mid; }
    }
    return lo;
}

/* Grow tag_ids[] geometrically. Returns 0 on success. */
static int TagsGrow(MemRegion *r) {
    uint16_t new_cap = (r->tag_cap == 0) ? MEMTAG_REGION_TAGS_INITIAL
                                          : (uint16_t)(r->tag_cap * 2);
    if (new_cap <= r->tag_cap) return -1;  /* overflow */
    uint16_t *new_ids = (uint16_t *)kmalloc(sizeof(uint16_t) * new_cap);
    if (!new_ids) return -1;
    if (r->tag_count > 0)
        memcpy(new_ids, r->tag_ids, sizeof(uint16_t) * r->tag_count);
    if (r->tag_ids) kfree(r->tag_ids);
    r->tag_ids = new_ids;
    r->tag_cap = new_cap;
    return 0;
}

/* ─── Public API ──────────────────────────────────────────────────────── */

error_t MemRegionRegistryInit(MemRegionRegistry *reg, size_t mem_end) {
    if (!reg || !mem_end) return ERR_INVALID_ARGUMENT;

    reg->slots = (MemRegion *)kmalloc(sizeof(MemRegion) * MEMTAG_REGION_INITIAL_CAP);
    if (!reg->slots) return ERR_NO_MEMORY;
    memset(reg->slots, 0, sizeof(MemRegion) * MEMTAG_REGION_INITIAL_CAP);

    reg->free_stack = (uint32_t *)kmalloc(sizeof(uint32_t) * MEMTAG_REGION_INITIAL_CAP);
    if (!reg->free_stack) {
        kfree(reg->slots);
        return ERR_NO_MEMORY;
    }

    /* Dense id_by_page allocated from PMM (can be MB-sized on large RAM). */
    size_t pages_in_ram = mem_end / PMM_PAGE_SIZE;
    size_t idbp_bytes   = pages_in_ram * sizeof(uint32_t);
    size_t idbp_pages   = (idbp_bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    uintptr_t idbp_phys = (uintptr_t)pmm_alloc(idbp_pages);
    if (!idbp_phys) {
        kfree(reg->free_stack);
        kfree(reg->slots);
        return ERR_NO_MEMORY;
    }
    uint32_t *idbp = (uint32_t *)vmm_phys_to_virt(idbp_phys);
    /* memset writes 0xFF bytes — MEMTAG_INVALID_REGION_ID is 0xFFFFFFFF. */
    memset(idbp, 0xFF, idbp_bytes);

    reg->id_by_page = idbp;
    reg->page_count = pages_in_ram;

    reg->slot_cap   = MEMTAG_REGION_INITIAL_CAP;
    reg->slot_count = 0;
    reg->free_top   = 0;
    reg->free_cap   = MEMTAG_REGION_INITIAL_CAP;
    reg->generation = 0;

    spinlock_init(&reg->lock);
    for (uint32_t i = 0; i < MEMTAG_REGION_BUCKETS; i++)
        spinlock_init(&reg->bucket_locks[i]);

    debug_printf("[MEMTAG/RGN] Init: slots=%u  id_by_page=%zuKB (%zu pages)\n",
                 MEMTAG_REGION_INITIAL_CAP, idbp_bytes / 1024, pages_in_ram);
    return OK;
}

void MemRegionRegistryShutdown(MemRegionRegistry *reg) {
    if (!reg) return;
    for (uint32_t i = 0; i < reg->slot_count; i++) {
        MemRegion *r = &reg->slots[i];
        if (r->tag_ids) kfree(r->tag_ids);
        MemRegionAttach *a = r->attach_head;
        while (a) {
            MemRegionAttach *next = a->next;
            kfree(a);
            a = next;
        }
        r->attach_head = NULL;
    }
    if (reg->slots)      kfree(reg->slots);
    if (reg->free_stack) kfree(reg->free_stack);
    /* id_by_page is from PMM — released only at full subsystem teardown
     * (which is boot-only). Leaving it freed via pmm_free would require
     * tracking the original allocation pointer. For Phase 1 we treat
     * shutdown as never-called in production. */
    reg->slots      = NULL;
    reg->free_stack = NULL;
    reg->id_by_page = NULL;
}

uint32_t MemRegionRegistryCreate(MemRegionRegistry *reg,
                                  uintptr_t base_phys,
                                  uintptr_t base_virt,
                                  void *ctx,
                                  size_t pages,
                                  uint16_t flags) {
    if (!reg || !pages) return MEMTAG_INVALID_REGION_ID;

    spin_lock(&reg->lock);

    uint32_t id;
    if (reg->free_top > 0) {
        id = reg->free_stack[--reg->free_top];
    } else {
        id = reg->slot_count;
        if (EnsureSlotCapacity(reg, id) != OK) {
            spin_unlock(&reg->lock);
            return MEMTAG_INVALID_REGION_ID;
        }
        reg->slot_count++;
    }

    MemRegion *r = &reg->slots[id];
    spin_unlock(&reg->lock);

    /* Per-bucket lock for content init — other buckets can run in parallel. */
    spin_lock(BucketLock(reg, id));
    r->base_phys   = base_phys;
    r->base_virt   = base_virt;
    r->ctx         = ctx;
    r->pages       = pages;
    r->tag_ids     = NULL;
    r->tag_count   = 0;
    r->tag_cap     = 0;
    r->flags       = (uint16_t)(flags | MEMTAG_REGION_FLAG_ACTIVE);
    if (base_phys && !base_virt) r->flags |= MEMTAG_REGION_FLAG_PHYSICAL;
    if (base_virt)               r->flags |= MEMTAG_REGION_FLAG_VIRTUAL;
    r->generation  = 1;
    r->attach_head = NULL;
    spin_unlock(BucketLock(reg, id));

    /* Index by phys page (last-writer-wins on overlap; bitmap is authoritative). */
    if (base_phys) IdByPageFill(reg, base_phys, pages, id);

    __atomic_add_fetch(&reg->generation, 1, __ATOMIC_RELEASE);
    return id;
}

void MemRegionRegistryDestroy(MemRegionRegistry *reg, uint32_t id) {
    if (!reg || id == MEMTAG_INVALID_REGION_ID) return;

    spin_lock(&reg->lock);
    if (id >= reg->slot_count) {
        spin_unlock(&reg->lock);
        return;
    }
    spin_unlock(&reg->lock);

    spin_lock(BucketLock(reg, id));
    MemRegion *r = &reg->slots[id];
    if (!(r->flags & MEMTAG_REGION_FLAG_ACTIVE)) {
        spin_unlock(BucketLock(reg, id));
        return;
    }

    uintptr_t base = r->base_phys;
    size_t    pgs  = r->pages;

    /* Free attach chain — Phase 2C bookkeeping must not outlive the
     * region. Bay/Brook should have detached every attach by now, but
     * leftover entries (after process_destroy without explicit detach)
     * are reaped here to avoid leaks. */
    MemRegionAttach *att = r->attach_head;
    r->attach_head = NULL;
    while (att) {
        MemRegionAttach *next = att->next;
        kfree(att);
        att = next;
    }

    if (r->tag_ids) kfree(r->tag_ids);
    r->tag_ids   = NULL;
    r->tag_count = 0;
    r->tag_cap   = 0;
    r->flags     = 0;
    r->base_phys = 0;
    r->base_virt = 0;
    r->pages     = 0;
    r->ctx       = NULL;
    r->generation++;
    spin_unlock(BucketLock(reg, id));

    if (base) IdByPageClearMatching(reg, base, pgs, id);

    spin_lock(&reg->lock);
    if (reg->free_top < reg->free_cap) {
        reg->free_stack[reg->free_top++] = id;
    }
    spin_unlock(&reg->lock);

    __atomic_add_fetch(&reg->generation, 1, __ATOMIC_RELEASE);
}

bool MemRegionRegistryIsActive(MemRegionRegistry *reg, uint32_t id) {
    if (!reg || id == MEMTAG_INVALID_REGION_ID) return false;
    if (id >= reg->slot_count) return false;
    /* Lock-free read of flag; ACTIVE bit is set/cleared atomically by C
     * (uint16_t aligned on x86-64). Safe for snapshot. */
    return (reg->slots[id].flags & MEMTAG_REGION_FLAG_ACTIVE) != 0;
}

uint32_t MemRegionRegistryFromPhys(MemRegionRegistry *reg, uintptr_t phys) {
    if (!reg || !reg->id_by_page) return MEMTAG_INVALID_REGION_ID;
    size_t page = phys / PMM_PAGE_SIZE;
    if (page >= reg->page_count) return MEMTAG_INVALID_REGION_ID;
    uint32_t id = reg->id_by_page[page];
    if (id == MEMTAG_INVALID_REGION_ID) return MEMTAG_INVALID_REGION_ID;
    if (!MemRegionRegistryIsActive(reg, id)) return MEMTAG_INVALID_REGION_ID;
    return id;
}

MemRegion *MemRegionRegistrySlot(MemRegionRegistry *reg, uint32_t id) {
    if (!reg || id == MEMTAG_INVALID_REGION_ID) return NULL;
    if (id >= reg->slot_count) return NULL;
    return &reg->slots[id];
}

error_t MemRegionRegistrySnapshot(MemRegionRegistry *reg, uint32_t id,
                                   MemRegionSnapshot *out) {
    if (!reg || !out || id == MEMTAG_INVALID_REGION_ID) return ERR_INVALID_ARGUMENT;
    if (id >= reg->slot_count) return ERR_OBJECT_NOT_FOUND;

    spin_lock(BucketLock(reg, id));
    MemRegion *r = &reg->slots[id];
    if (!(r->flags & MEMTAG_REGION_FLAG_ACTIVE)) {
        spin_unlock(BucketLock(reg, id));
        return ERR_OBJECT_NOT_FOUND;
    }
    out->base_phys  = r->base_phys;
    out->base_virt  = r->base_virt;
    out->pages      = r->pages;
    out->tag_count  = r->tag_count;
    out->flags      = r->flags;
    out->generation = r->generation;
    spin_unlock(BucketLock(reg, id));
    return OK;
}

error_t MemRegionRegistryAddTag(MemRegionRegistry *reg,
                                 uint32_t id, uint16_t tag_id) {
    if (!reg || id == MEMTAG_INVALID_REGION_ID || tag_id == MEMTAG_INVALID_TAG_ID)
        return ERR_INVALID_ARGUMENT;
    if (id >= reg->slot_count) return ERR_OBJECT_NOT_FOUND;

    spin_lock(BucketLock(reg, id));
    MemRegion *r = &reg->slots[id];
    if (!(r->flags & MEMTAG_REGION_FLAG_ACTIVE)) {
        spin_unlock(BucketLock(reg, id));
        return ERR_OBJECT_NOT_FOUND;
    }

    bool found;
    uint16_t pos = TagSearch(r->tag_ids, r->tag_count, tag_id, &found);
    if (found) {
        spin_unlock(BucketLock(reg, id));
        return OK;  /* idempotent */
    }

    if (r->tag_count >= r->tag_cap) {
        if (TagsGrow(r) != 0) {
            spin_unlock(BucketLock(reg, id));
            return ERR_NO_MEMORY;
        }
    }
    /* Shift right to make room at pos */
    if (pos < r->tag_count) {
        memmove(&r->tag_ids[pos + 1], &r->tag_ids[pos],
                sizeof(uint16_t) * (r->tag_count - pos));
    }
    r->tag_ids[pos] = tag_id;
    r->tag_count++;
    r->generation++;
    spin_unlock(BucketLock(reg, id));
    return OK;
}

error_t MemRegionRegistryRemoveTag(MemRegionRegistry *reg,
                                    uint32_t id, uint16_t tag_id) {
    if (!reg || id == MEMTAG_INVALID_REGION_ID || tag_id == MEMTAG_INVALID_TAG_ID)
        return ERR_INVALID_ARGUMENT;
    if (id >= reg->slot_count) return ERR_OBJECT_NOT_FOUND;

    spin_lock(BucketLock(reg, id));
    MemRegion *r = &reg->slots[id];
    if (!(r->flags & MEMTAG_REGION_FLAG_ACTIVE)) {
        spin_unlock(BucketLock(reg, id));
        return ERR_OBJECT_NOT_FOUND;
    }
    bool found;
    uint16_t pos = TagSearch(r->tag_ids, r->tag_count, tag_id, &found);
    if (!found) {
        spin_unlock(BucketLock(reg, id));
        return OK;  /* idempotent */
    }
    /* Shift left over removed entry */
    if (pos + 1 < r->tag_count) {
        memmove(&r->tag_ids[pos], &r->tag_ids[pos + 1],
                sizeof(uint16_t) * (r->tag_count - pos - 1));
    }
    r->tag_count--;
    r->generation++;
    spin_unlock(BucketLock(reg, id));
    return OK;
}

bool MemRegionRegistryHasTag(MemRegionRegistry *reg,
                              uint32_t id, uint16_t tag_id) {
    if (!reg || id == MEMTAG_INVALID_REGION_ID || tag_id == MEMTAG_INVALID_TAG_ID)
        return false;
    if (id >= reg->slot_count) return false;

    spin_lock(BucketLock(reg, id));
    MemRegion *r = &reg->slots[id];
    bool result = false;
    if (r->flags & MEMTAG_REGION_FLAG_ACTIVE) {
        bool found;
        TagSearch(r->tag_ids, r->tag_count, tag_id, &found);
        result = found;
    }
    spin_unlock(BucketLock(reg, id));
    return result;
}

size_t MemRegionRegistryListTags(MemRegionRegistry *reg, uint32_t id,
                                  uint16_t *out, size_t max) {
    if (!reg || id == MEMTAG_INVALID_REGION_ID || !out) return 0;
    if (id >= reg->slot_count) return 0;

    spin_lock(BucketLock(reg, id));
    MemRegion *r = &reg->slots[id];
    size_t n = 0;
    if (r->flags & MEMTAG_REGION_FLAG_ACTIVE) {
        n = (r->tag_count < max) ? r->tag_count : max;
        memcpy(out, r->tag_ids, n * sizeof(uint16_t));
    }
    spin_unlock(BucketLock(reg, id));
    return n;
}

/* ─── Phase 2C — per-attach mapping registry ────────────────────────── */

/* Internal: find attach matching (ctx, va_base). Caller must hold bucket
 * lock. Returns NULL if not found. */
static MemRegionAttach *AttachLookup(MemRegion *r, void *ctx, uintptr_t va) {
    for (MemRegionAttach *a = r->attach_head; a; a = a->next) {
        if (a->ctx == ctx && a->va_base == va) return a;
    }
    return NULL;
}

error_t MemRegionRegistryAttach(MemRegionRegistry *reg,
                                 uint32_t region_id,
                                 void *ctx,
                                 uintptr_t va_base,
                                 uint32_t pages,
                                 uint8_t page_class,
                                 uint64_t orig_flags) {
    if (!reg || region_id == MEMTAG_INVALID_REGION_ID || !ctx || !pages)
        return ERR_INVALID_ARGUMENT;
    if (region_id >= reg->slot_count) return ERR_OBJECT_NOT_FOUND;
    if (page_class != MEMTAG_ATTACH_CLASS_4K && page_class != MEMTAG_ATTACH_CLASS_2M)
        return ERR_INVALID_ARGUMENT;

    /* Allocate the node OUTSIDE the bucket lock — kmalloc can take a
     * long time on first slab allocation. */
    MemRegionAttach *fresh = (MemRegionAttach *)kmalloc(sizeof(MemRegionAttach));
    if (!fresh) return ERR_NO_MEMORY;
    fresh->ctx         = ctx;
    fresh->va_base     = va_base;
    fresh->region_id   = region_id;
    fresh->pages       = pages;
    fresh->orig_flags  = orig_flags;
    fresh->page_class  = page_class;
    fresh->state       = MEMTAG_ATTACH_ACTIVE;
    fresh->reserved16  = 0;
    fresh->reserved32  = 0;
    fresh->next        = NULL;

    spin_lock(BucketLock(reg, region_id));
    MemRegion *r = &reg->slots[region_id];
    if (!(r->flags & MEMTAG_REGION_FLAG_ACTIVE)) {
        spin_unlock(BucketLock(reg, region_id));
        kfree(fresh);
        return ERR_OBJECT_NOT_FOUND;
    }

    /* Idempotent: if (ctx, va_base) already present, refresh in place
     * and discard the fresh node. Refreshes orig_flags + pages + state
     * so subsequent grant/revoke operates on current mapping state. */
    MemRegionAttach *existing = AttachLookup(r, ctx, va_base);
    if (existing) {
        existing->pages      = pages;
        existing->orig_flags = orig_flags;
        existing->page_class = page_class;
        existing->state      = MEMTAG_ATTACH_ACTIVE;
        spin_unlock(BucketLock(reg, region_id));
        kfree(fresh);
        return OK;
    }

    /* Link at head — O(1) and order doesn't matter for enforcement walks. */
    fresh->next     = r->attach_head;
    r->attach_head  = fresh;
    r->generation++;
    spin_unlock(BucketLock(reg, region_id));
    return OK;
}

error_t MemRegionRegistryDetach(MemRegionRegistry *reg,
                                 uint32_t region_id,
                                 void *ctx,
                                 uintptr_t va_base) {
    if (!reg || region_id == MEMTAG_INVALID_REGION_ID || !ctx)
        return ERR_INVALID_ARGUMENT;
    if (region_id >= reg->slot_count) return ERR_OBJECT_NOT_FOUND;

    spin_lock(BucketLock(reg, region_id));
    MemRegion *r = &reg->slots[region_id];
    if (!(r->flags & MEMTAG_REGION_FLAG_ACTIVE)) {
        spin_unlock(BucketLock(reg, region_id));
        return OK;  /* race with destroy — idempotent */
    }

    MemRegionAttach **p = &r->attach_head;
    while (*p) {
        MemRegionAttach *cur = *p;
        if (cur->ctx == ctx && cur->va_base == va_base) {
            *p = cur->next;
            r->generation++;
            spin_unlock(BucketLock(reg, region_id));
            kfree(cur);
            return OK;
        }
        p = &cur->next;
    }
    spin_unlock(BucketLock(reg, region_id));
    return OK;  /* not found — idempotent */
}

size_t MemRegionRegistrySnapshotAttachs(MemRegionRegistry *reg,
                                         uint32_t region_id,
                                         MemRegionAttach *out,
                                         size_t max) {
    if (!reg || region_id == MEMTAG_INVALID_REGION_ID || !out || !max) return 0;
    if (region_id >= reg->slot_count) return 0;

    spin_lock(BucketLock(reg, region_id));
    MemRegion *r = &reg->slots[region_id];
    size_t n = 0;
    if (r->flags & MEMTAG_REGION_FLAG_ACTIVE) {
        for (MemRegionAttach *a = r->attach_head; a && n < max; a = a->next) {
            /* Snapshot copy: clear `next` so caller can't accidentally
             * chase a stale pointer into registry-owned memory after the
             * bucket lock drops. All consumed fields are by-value. */
            out[n]          = *a;
            out[n].next     = NULL;
            n++;
        }
    }
    spin_unlock(BucketLock(reg, region_id));
    return n;
}

error_t MemRegionRegistrySetAttachState(MemRegionRegistry *reg,
                                         uint32_t region_id,
                                         void *ctx,
                                         uintptr_t va_base,
                                         uint8_t new_state) {
    if (!reg || region_id == MEMTAG_INVALID_REGION_ID || !ctx)
        return ERR_INVALID_ARGUMENT;
    if (region_id >= reg->slot_count) return ERR_OBJECT_NOT_FOUND;

    spin_lock(BucketLock(reg, region_id));
    MemRegion *r = &reg->slots[region_id];
    if (!(r->flags & MEMTAG_REGION_FLAG_ACTIVE)) {
        spin_unlock(BucketLock(reg, region_id));
        return OK;  /* race with destroy — silent */
    }

    MemRegionAttach *a = AttachLookup(r, ctx, va_base);
    if (a) {
        a->state = new_state;
        r->generation++;
    }
    spin_unlock(BucketLock(reg, region_id));
    return OK;
}

size_t MemRegionRegistryDetachAllForCtx(MemRegionRegistry *reg, void *ctx) {
    if (!reg || !ctx) return 0;
    size_t removed = 0;
    /* Walk every active slot under its bucket lock. Heavy on huge
     * region counts but only runs during process_destroy cleanup paths
     * where Bay/Brook missed an explicit detach. */
    for (uint32_t id = 0; id < reg->slot_count; id++) {
        spin_lock(BucketLock(reg, id));
        MemRegion *r = &reg->slots[id];
        if (!(r->flags & MEMTAG_REGION_FLAG_ACTIVE)) {
            spin_unlock(BucketLock(reg, id));
            continue;
        }
        MemRegionAttach **p = &r->attach_head;
        bool changed = false;
        while (*p) {
            if ((*p)->ctx == ctx) {
                MemRegionAttach *gone = *p;
                *p = gone->next;
                kfree(gone);
                removed++;
                changed = true;
            } else {
                p = &(*p)->next;
            }
        }
        if (changed) r->generation++;
        spin_unlock(BucketLock(reg, id));
    }
    if (removed) __atomic_add_fetch(&reg->generation, 1, __ATOMIC_RELEASE);
    return removed;
}

uint32_t MemRegionRegistryActiveCount(MemRegionRegistry *reg) {
    if (!reg) return 0;
    uint32_t active = 0;
    /* Approximation under heavy mutation; exact reads would need to hold
     * every bucket lock which is wasteful. */
    for (uint32_t i = 0; i < reg->slot_count; i++) {
        if (reg->slots[i].flags & MEMTAG_REGION_FLAG_ACTIVE) active++;
    }
    return active;
}

uint64_t MemRegionRegistryGeneration(MemRegionRegistry *reg) {
    if (!reg) return 0;
    return __atomic_load_n(&reg->generation, __ATOMIC_ACQUIRE);
}

void MemRegionRegistryDump(MemRegionRegistry *reg, MemTagRegistry *tag_reg) {
    if (!reg) return;
    uint32_t active = MemRegionRegistryActiveCount(reg);
    debug_printf("[MEMTAG/RGN] slots=%u/%u  active=%u  gen=%lu\n",
                 reg->slot_count, reg->slot_cap, active,
                 (unsigned long)reg->generation);

    for (uint32_t i = 0; i < reg->slot_count; i++) {
        MemRegion *r = &reg->slots[i];
        if (!(r->flags & MEMTAG_REGION_FLAG_ACTIVE)) continue;
        debug_printf("[MEMTAG/RGN]   [%5u] phys=0x%016lx virt=0x%016lx pages=%zu flags=0x%04x tags=%u:",
                     i, r->base_phys, r->base_virt, r->pages, r->flags, r->tag_count);
        for (uint16_t t = 0; t < r->tag_count; t++) {
            uint16_t tid = r->tag_ids[t];
            const char *k = tag_reg ? MemTagRegistryKey(tag_reg, tid)   : "?";
            const char *v = tag_reg ? MemTagRegistryValue(tag_reg, tid) : NULL;
            debug_printf(" %s%s%s", k ? k : "?", v ? ":" : "", v ? v : "");
        }
        debug_printf("\n");
    }
}
