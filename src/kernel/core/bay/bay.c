
#include "bay.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "klib.h"
#include "tagfs.h"
#include "tag_registry.h"
#include "memtag.h"
#include "tme.h"
#include "atomics.h"
#include "error.h"

#define BAY_PAGE_CLASS_4K   12u
#define BAY_PAGE_CLASS_2M   21u

#define BAY_HUGE_THRESHOLD  VMM_LARGE_PAGE_2M_SIZE
#define BAY_HUGE_SIZE       VMM_LARGE_PAGE_2M_SIZE
#define BAY_HUGE_PAGES      VMM_LARGE_PAGE_2M_PAGES

#define BAY_BUCKETS         256u
#define BAY_BUCKET_MASK     (BAY_BUCKETS - 1)

typedef struct BayBucket {
    spinlock_t  lock;
    BayObject  *head;
} BayBucket;

static BayBucket g_bay_buckets[BAY_BUCKETS];

static volatile uint64_t g_stat_objects;
static volatile uint64_t g_stat_claims;
static volatile uint64_t g_stat_pages;
static volatile uint64_t g_stat_releases;

static inline uint32_t bay_bucket_index(uint16_t tag_id)
{
    return ((uint32_t)tag_id ^ ((uint32_t)tag_id >> 4)) & BAY_BUCKET_MASK;
}

void BayInit(void)
{
    for (uint32_t i = 0; i < BAY_BUCKETS; i++) {
        spinlock_init(&g_bay_buckets[i].lock);
        g_bay_buckets[i].head = NULL;
    }
    debug_printf("[Bay] init: %u buckets, 2 MB huge-page threshold\n",
                 (unsigned)BAY_BUCKETS);
}

static uint16_t bay_resolve_tag(const char *tag, bool intern_if_missing)
{
    if (!tag || tag[0] == '\0') return TAGFS_INVALID_TAG_ID;
    return intern_if_missing ? tagfs_tag_intern(tag) : tagfs_tag_lookup(tag);
}

static error_t bay_alloc_chunks_internal(uint64_t total_size,
                                          bool force_4k,
                                          uint16_t *out_class,
                                          uint64_t *out_chunk_size,
                                          uint32_t *out_chunk_count,
                                          uint64_t **out_chunks)
{
    if (total_size == 0) return ERR_INVALID_ARGUMENT;

    uint16_t cls;
    uint64_t cs;
    if (!force_4k && total_size >= BAY_HUGE_THRESHOLD) {
        cls = BAY_PAGE_CLASS_2M;
        cs  = BAY_HUGE_SIZE;
    } else {
        cls = BAY_PAGE_CLASS_4K;
        cs  = PMM_PAGE_SIZE;
    }
    uint32_t cc = (uint32_t)((total_size + cs - 1) / cs);

    uint64_t *chunks = (uint64_t *)kmalloc(cc * sizeof(uint64_t));
    if (!chunks) return ERR_NO_MEMORY;
    memset(chunks, 0, cc * sizeof(uint64_t));

    for (uint32_t i = 0; i < cc; i++) {
        size_t pages = (cls == BAY_PAGE_CLASS_2M) ? BAY_HUGE_PAGES : 1;
        void *p = pmm_alloc_zero(pages);
        if (!p && cls == BAY_PAGE_CLASS_2M) {
            for (uint32_t j = 0; j < i; j++) {
                if (chunks[j]) pmm_free((void *)chunks[j], BAY_HUGE_PAGES);
            }
            kfree(chunks);

            cls = BAY_PAGE_CLASS_4K;
            cs  = PMM_PAGE_SIZE;
            cc  = (uint32_t)((total_size + cs - 1) / cs);
            chunks = (uint64_t *)kmalloc(cc * sizeof(uint64_t));
            if (!chunks) return ERR_NO_MEMORY;
            memset(chunks, 0, cc * sizeof(uint64_t));
            i = 0;
            continue;
        }
        if (!p) {
            for (uint32_t j = 0; j < i; j++) {
                if (chunks[j]) pmm_free((void *)chunks[j], 1);
            }
            kfree(chunks);
            return ERR_NO_MEMORY;
        }
        chunks[i] = (uint64_t)p;
    }

    *out_class       = cls;
    *out_chunk_size  = cs;
    *out_chunk_count = cc;
    *out_chunks      = chunks;
    return OK;
}

static error_t bay_alloc_chunks(uint64_t total_size,
                                uint16_t *out_class,
                                uint64_t *out_chunk_size,
                                uint32_t *out_chunk_count,
                                uint64_t **out_chunks)
{
    return bay_alloc_chunks_internal(total_size, false,
                                      out_class, out_chunk_size,
                                      out_chunk_count, out_chunks);
}

error_t bay_alloc_chunks_4k(uint64_t total_size,
                            uint16_t *out_class,
                            uint64_t *out_chunk_size,
                            uint32_t *out_chunk_count,
                            uint64_t **out_chunks)
{
    return bay_alloc_chunks_internal(total_size, true,
                                      out_class, out_chunk_size,
                                      out_chunk_count, out_chunks);
}

static error_t bay_map_into_cabin(cabin_t *cabin,
                                  uint64_t user_va_base,
                                  uint64_t *chunks,
                                  uint32_t chunk_count,
                                  uint64_t chunk_size,
                                  uint32_t flags,
                                  uint16_t tme_keyid)
{
    if (!cabin || !cabin->vmm) return ERR_INVALID_ARGUMENT;

    uint64_t vmm_flags = VMM_FLAGS_USER_RW;
    if (flags & BAY_RO) {
        vmm_flags = VMM_FLAGS_USER_RO;
    }
    vmm_flags |= VMM_FLAG_NO_EXECUTE;

    const bool encrypted = (tme_keyid != 0);

    for (uint32_t i = 0; i < chunk_count; i++) {
        uint64_t va = user_va_base + (uint64_t)i * chunk_size;
        uint64_t pa = chunks[i];

        bool ok;
        if (chunk_size == BAY_HUGE_SIZE) {
            if (encrypted) {
                uint64_t pa_with_keyid = tme_phys_with_keyid(pa, tme_keyid);
                ok = vmm_map_huge_2m_with_keyid(cabin->vmm, va,
                                                 pa_with_keyid, vmm_flags);
            } else {
                ok = vmm_map_huge_2m(cabin->vmm, va, pa, vmm_flags);
            }
        } else if (encrypted) {
            uint64_t pa_with_keyid = tme_phys_with_keyid(pa, tme_keyid);
            vmm_map_result_t r = vmm_map_page_with_keyid(cabin->vmm, va,
                                                         pa_with_keyid, vmm_flags);
            ok = r.success;
        } else {
            vmm_map_result_t r = vmm_map_page(cabin->vmm, va, pa, vmm_flags);
            ok = r.success;
        }

        if (!ok) {
            for (uint32_t j = 0; j < i; j++) {
                uint64_t uva = user_va_base + (uint64_t)j * chunk_size;
                if (chunk_size == BAY_HUGE_SIZE) {
                    vmm_unmap_huge_2m(cabin->vmm, uva);
                } else {
                    vmm_unmap_page(cabin->vmm, uva);
                }
            }
            return ERR_NO_MEMORY;
        }
    }
    return OK;
}

static void bay_memtag_attach_all(cabin_t *cabin,
                                   uint64_t *chunks,
                                   uint32_t chunk_count,
                                   uint64_t va_base,
                                   uint64_t chunk_size,
                                   uint32_t bay_flags)
{
    uint64_t att_flags = (bay_flags & BAY_RO) ? VMM_FLAGS_USER_RO
                                              : VMM_FLAGS_USER_RW;
    att_flags |= VMM_FLAG_NO_EXECUTE;
    uint8_t  page_class = (chunk_size == BAY_HUGE_SIZE)
                          ? MEMTAG_ATTACH_CLASS_2M
                          : MEMTAG_ATTACH_CLASS_4K;
    uint32_t pages_per_chunk = (chunk_size == BAY_HUGE_SIZE)
                               ? (uint32_t)BAY_HUGE_PAGES : 1u;
    for (uint32_t i = 0; i < chunk_count; i++) {
        if (chunks[i] == 0) continue;
        uint64_t att_va = va_base + (uint64_t)i * chunk_size;
        uint32_t rid    = MemRegionFromPhys((uintptr_t)chunks[i]);
        if (rid != MEMTAG_INVALID_REGION_ID) {
            MemRegionAttachCabin(rid, (void *)cabin->vmm, att_va,
                                  pages_per_chunk, page_class, att_flags);
        }
    }
}

static void bay_memtag_detach_all(cabin_t *cabin,
                                   uint64_t *chunks,
                                   uint32_t chunk_count,
                                   uint64_t va_base,
                                   uint64_t chunk_size)
{
    for (uint32_t i = 0; i < chunk_count; i++) {
        if (chunks[i] == 0) continue;
        uint64_t att_va = va_base + (uint64_t)i * chunk_size;
        uint32_t rid    = MemRegionFromPhys((uintptr_t)chunks[i]);
        if (rid != MEMTAG_INVALID_REGION_ID) {
            MemRegionDetachCabin(rid, (void *)cabin->vmm, att_va);
        }
    }
}

static void bay_unmap_from_cabin(cabin_t *cabin,
                                 uint64_t user_va_base,
                                 uint32_t chunk_count,
                                 uint64_t chunk_size)
{
    if (!cabin || !cabin->vmm) return;
    for (uint32_t i = 0; i < chunk_count; i++) {
        uint64_t uva = user_va_base + (uint64_t)i * chunk_size;
        if (chunk_size == BAY_HUGE_SIZE) {
            vmm_unmap_huge_2m(cabin->vmm, uva);
        } else {
            vmm_unmap_page(cabin->vmm, uva);
        }
    }
}

static void bay_free_chunks(BayObject *bay)
{
    if (!bay || !bay->chunks) return;
    size_t pages_per_chunk = (bay->page_class == BAY_PAGE_CLASS_2M)
                             ? BAY_HUGE_PAGES : 1;
    for (uint32_t i = 0; i < bay->chunk_count; i++) {
        if (bay->chunks[i] == 0) continue;
        pmm_free((void *)bay->chunks[i], pages_per_chunk);
    }
    kfree(bay->chunks);
    bay->chunks = NULL;
}

static uint64_t bay_reserve_user_va(cabin_t *cabin,
                                    uint64_t size,
                                    uint64_t page_align)
{
    if (page_align < PMM_PAGE_SIZE) page_align = PMM_PAGE_SIZE;

    uint64_t base;
    spin_lock(&cabin->bay_lock);
    uint64_t cur = (cabin->bay_va_next + page_align - 1) & ~(page_align - 1);
    if (size > CABIN_BAY_END - cur) {
        spin_unlock(&cabin->bay_lock);
        return 0;
    }
    base = cur;
    cabin->bay_va_next = cur + size;
    spin_unlock(&cabin->bay_lock);
    return base;
}

static void bay_link_claim(cabin_t *cabin, BayClaim *claim)
{
    spin_lock(&cabin->bay_lock);
    claim->cabin_next = (BayClaim *)cabin->bay_claims_head;
    cabin->bay_claims_head = claim;
    spin_unlock(&cabin->bay_lock);
}

static BayClaim *bay_unlink_claim_by_va(cabin_t *cabin, uint64_t user_va)
{
    spin_lock(&cabin->bay_lock);
    BayClaim **p = (BayClaim **)&cabin->bay_claims_head;
    while (*p) {
        if ((*p)->user_va_base == user_va) {
            BayClaim *hit = *p;
            *p = hit->cabin_next;
            spin_unlock(&cabin->bay_lock);
            return hit;
        }
        p = &(*p)->cabin_next;
    }
    spin_unlock(&cabin->bay_lock);
    return NULL;
}

static BayClaim *bay_find_claim_by_va(cabin_t *cabin, uint64_t user_va)
{
    spin_lock(&cabin->bay_lock);
    BayClaim *c = (BayClaim *)cabin->bay_claims_head;
    while (c && c->user_va_base != user_va) c = c->cabin_next;
    spin_unlock(&cabin->bay_lock);
    return c;
}

static BayObject *bay_bucket_find_locked(BayBucket *b, uint16_t tag_id)
{
    for (BayObject *o = b->head; o; o = o->bucket_next) {
        if (o->tag_id == tag_id) return o;
    }
    return NULL;
}

static void bay_bucket_unlink_locked(BayBucket *b, BayObject *obj)
{
    BayObject **p = &b->head;
    while (*p && *p != obj) p = &(*p)->bucket_next;
    if (*p) *p = obj->bucket_next;
    obj->bucket_next = NULL;
}

static inline uint64_t bay_total_pages(const BayObject *bay)
{
    uint64_t pages_per_chunk = (bay->chunk_size == BAY_HUGE_SIZE)
                               ? BAY_HUGE_PAGES : 1;
    return (uint64_t)bay->chunk_count * pages_per_chunk;
}

static void bay_drop_ref_locked(BayBucket *b, BayObject *bay)
{
    bay->ref_count--;
    if (bay->ref_count == 0) {
        uint16_t released_keyid = bay->tme_keyid;
        uint32_t creator_pid    = (uint32_t)bay->create_pid;

        bay_bucket_unlink_locked(b, bay);
        spin_unlock(&b->lock);

        atomic_fetch_sub_u64(&g_stat_objects, 1);
        atomic_fetch_sub_u64(&g_stat_pages, bay_total_pages(bay));
        bay_free_chunks(bay);
        kfree(bay);

        if (released_keyid != 0) {
            (void)tme_keyid_free(released_keyid);
            process_t *creator = process_find(creator_pid);
            if (creator && creator->cabin) {
                spin_lock(&creator->cabin->bay_lock);
                if (creator->cabin->tme_keyids_held > 0)
                    creator->cabin->tme_keyids_held--;
                spin_unlock(&creator->cabin->bay_lock);
            }
        }
        return;
    }
    spin_unlock(&b->lock);
}

error_t BayOpenInternal(struct process_t *proc,
                        const char *tag,
                        uint64_t requested_size,
                        uint32_t flags,
                        uint64_t *out_user_va,
                        uint64_t *out_actual_size)
{
    if (!proc || !proc->cabin || !tag || !out_user_va || !out_actual_size)
        return ERR_INVALID_ARGUMENT;

    cabin_t *cabin = proc->cabin;

    if (flags & ~BAY_FLAGS_MASK) return ERR_INVALID_ARGUMENT;

    bool wants_create = (flags & BAY_CREATE) != 0;
    uint16_t tag_id = bay_resolve_tag(tag, wants_create);
    if (tag_id == TAGFS_INVALID_TAG_ID) return ERR_TAG_NOT_FOUND;

    BayBucket *b = &g_bay_buckets[bay_bucket_index(tag_id)];

    spin_lock(&b->lock);
    BayObject *bay = bay_bucket_find_locked(b, tag_id);
    if (bay) {
        bool caller_wants_enc  = (flags & BAY_ENCRYPTED) != 0;
        bool bay_is_encrypted  = (bay->tme_keyid != 0);
        if (caller_wants_enc != bay_is_encrypted) {
            spin_unlock(&b->lock);
            return ERR_INVALID_ARGUMENT;
        }
    }
    if (!bay) {
        if (!wants_create) {
            spin_unlock(&b->lock);
            return ERR_TAG_NOT_FOUND;
        }
        if (requested_size == 0) {
            spin_unlock(&b->lock);
            return ERR_INVALID_ARGUMENT;
        }
        spin_unlock(&b->lock);

        uint16_t alloc_keyid = 0;
        if (flags & BAY_ENCRYPTED) {
            if (!g_tme.mk_active) return ERR_UNSUPPORTED;
            spin_lock(&cabin->bay_lock);
            if (cabin->tme_keyids_held >= TME_QUOTA_PER_PROC) {
                spin_unlock(&cabin->bay_lock);
                return ERR_QUOTA_EXCEEDED;
            }
            cabin->tme_keyids_held++;
            spin_unlock(&cabin->bay_lock);

            error_t krc = tme_keyid_alloc(&alloc_keyid);
            if (krc != OK) {
                spin_lock(&cabin->bay_lock);
                if (cabin->tme_keyids_held > 0) cabin->tme_keyids_held--;
                spin_unlock(&cabin->bay_lock);
                return krc;
            }
        }

        uint16_t  cls;
        uint64_t  cs;
        uint32_t  cc;
        uint64_t *chunks;
        error_t   rc;
        rc = bay_alloc_chunks(requested_size, &cls, &cs, &cc, &chunks);
        if (rc != OK) {
            if (alloc_keyid) {
                (void)tme_keyid_free(alloc_keyid);
                spin_lock(&cabin->bay_lock);
                if (cabin->tme_keyids_held > 0) cabin->tme_keyids_held--;
                spin_unlock(&cabin->bay_lock);
            }
            return rc;
        }

        if (alloc_keyid) {
            bool huge = (cls == BAY_PAGE_CLASS_2M);
            for (uint32_t ci = 0; ci < cc; ci++) {
                if (chunks[ci] == 0) continue;
                error_t zrc = tme_zero_pages_with_keyid(
                    (uintptr_t)chunks[ci],
                    huge ? BAY_HUGE_PAGES : 1u,
                    alloc_keyid, huge);
                if (zrc != OK) {
                    for (uint32_t k = 0; k < cc; k++) {
                        if (chunks[k]) pmm_free((void *)chunks[k],
                                                 huge ? BAY_HUGE_PAGES : 1);
                    }
                    kfree(chunks);
                    (void)tme_keyid_free(alloc_keyid);
                    spin_lock(&cabin->bay_lock);
                    if (cabin->tme_keyids_held > 0) cabin->tme_keyids_held--;
                    spin_unlock(&cabin->bay_lock);
                    return zrc;
                }
            }
        }

        BayObject *fresh = (BayObject *)kmalloc(sizeof(BayObject));
        if (!fresh) {
            for (uint32_t i = 0; i < cc; i++) {
                if (chunks[i]) pmm_free((void *)chunks[i],
                                         (cls == BAY_PAGE_CLASS_2M) ? BAY_HUGE_PAGES : 1);
            }
            kfree(chunks);
            if (alloc_keyid) {
                (void)tme_keyid_free(alloc_keyid);
                spin_lock(&cabin->bay_lock);
                if (cabin->tme_keyids_held > 0) cabin->tme_keyids_held--;
                spin_unlock(&cabin->bay_lock);
            }
            return ERR_NO_MEMORY;
        }
        memset(fresh, 0, sizeof(*fresh));
        fresh->tag_id      = tag_id;
        fresh->page_class  = cls;
        fresh->tme_keyid   = alloc_keyid;
        fresh->total_size  = (uint64_t)cc * cs;
        fresh->user_size   = requested_size;
        fresh->chunk_size  = cs;
        fresh->chunk_count = cc;
        fresh->chunks      = chunks;
        fresh->flags       = flags & ~BAY_RO;
        fresh->ref_count   = 0;
        fresh->create_pid  = proc->pid;
        fresh->create_tsc  = rdtsc();

        spin_lock(&b->lock);
        BayObject *winner = bay_bucket_find_locked(b, tag_id);
        if (winner) {
            spin_unlock(&b->lock);
            bay_free_chunks(fresh);
            kfree(fresh);
            if (alloc_keyid) {
                (void)tme_keyid_free(alloc_keyid);
                spin_lock(&cabin->bay_lock);
                if (cabin->tme_keyids_held > 0) cabin->tme_keyids_held--;
                spin_unlock(&cabin->bay_lock);
                alloc_keyid = 0;
            }
            spin_lock(&b->lock);
            bay = bay_bucket_find_locked(b, tag_id);
            if (!bay) {
                spin_unlock(&b->lock);
                return ERR_TAG_NOT_FOUND;
            }
            bool caller_wants_enc  = (flags & BAY_ENCRYPTED) != 0;
            bool bay_is_encrypted  = (bay->tme_keyid != 0);
            if (caller_wants_enc != bay_is_encrypted) {
                spin_unlock(&b->lock);
                return ERR_INVALID_ARGUMENT;
            }
        } else {
            fresh->bucket_next = b->head;
            b->head            = fresh;
            bay                = fresh;
            atomic_fetch_add_u64(&g_stat_objects, 1);
            atomic_fetch_add_u64(&g_stat_pages,
                                 (uint64_t)cc * ((cs == BAY_HUGE_SIZE) ? BAY_HUGE_PAGES : 1));

            char tag_buf[128];
            if (tagfs_tag_text(tag_id, tag_buf, sizeof(tag_buf))) {
                size_t pages_per_chunk =
                    (cs == BAY_HUGE_SIZE) ? BAY_HUGE_PAGES : 1;
                for (uint32_t ci = 0; ci < cc; ci++) {
                    MemTagApplyByPhys((uintptr_t)chunks[ci], pages_per_chunk, tag_buf);
                    MemTagApplyByPhys((uintptr_t)chunks[ci], pages_per_chunk, "purpose:bay");
                }
            }
        }
    }

    bay->ref_count++;
    uint64_t total_size = bay->total_size;
    uint64_t user_size  = bay->user_size;
    uint32_t chunk_count = bay->chunk_count;
    uint64_t chunk_size  = bay->chunk_size;
    spin_unlock(&b->lock);

    uint64_t va = bay_reserve_user_va(cabin, total_size, chunk_size);
    if (va == 0) {
        spin_lock(&b->lock);
        bay_drop_ref_locked(b, bay);
        return ERR_NO_MEMORY;
    }

    BayClaim *claim = (BayClaim *)kmalloc(sizeof(BayClaim));
    if (!claim) {
        spin_lock(&b->lock);
        bay_drop_ref_locked(b, bay);
        return ERR_NO_MEMORY;
    }
    memset(claim, 0, sizeof(*claim));
    claim->bay          = bay;
    claim->user_va_base = va;
    claim->flags        = flags;

    if (bay->chunks && chunk_count > 0) {
        if (!MemTagEnforcePhys(proc->pid, (uintptr_t)bay->chunks[0])) {
            kfree(claim);
            spin_lock(&b->lock);
            bay_drop_ref_locked(b, bay);
            return ERR_PERMISSION_DENIED;
        }
    }

    error_t rc = bay_map_into_cabin(cabin, va, bay->chunks,
                                    chunk_count, chunk_size, flags,
                                    bay->tme_keyid);
    if (rc != OK) {
        kfree(claim);
        spin_lock(&b->lock);
        bay_drop_ref_locked(b, bay);
        return rc;
    }

    bay_memtag_attach_all(cabin, bay->chunks, chunk_count, va, chunk_size, flags);

    bay_link_claim(cabin, claim);
    atomic_fetch_add_u64(&g_stat_claims, 1);

    *out_user_va     = va;
    *out_actual_size = user_size;
    return OK;
}

error_t BayReleaseInternal(struct process_t *proc, uint64_t user_va)
{
    if (!proc || !proc->cabin || user_va == 0) return ERR_INVALID_ARGUMENT;

    cabin_t *cabin = proc->cabin;

    BayClaim *claim = bay_unlink_claim_by_va(cabin, user_va);
    if (!claim) return ERR_TAG_NOT_FOUND;

    BayObject *bay = claim->bay;
    if (!bay) {
        kfree(claim);
        return ERR_INVALID_STATE;
    }

    bay_memtag_detach_all(cabin, bay->chunks, bay->chunk_count,
                          claim->user_va_base, bay->chunk_size);

    bay_unmap_from_cabin(cabin, claim->user_va_base,
                         bay->chunk_count, bay->chunk_size);

    BayBucket *b = &g_bay_buckets[bay_bucket_index(bay->tag_id)];
    spin_lock(&b->lock);
    bay_drop_ref_locked(b, bay);

    kfree(claim);
    if (g_stat_claims > 0) atomic_fetch_sub_u64(&g_stat_claims, 1);
    atomic_fetch_add_u64(&g_stat_releases, 1);
    return OK;
}

uint64_t BaySizeInternal(struct process_t *proc, uint64_t user_va)
{
    if (!proc || !proc->cabin) return 0;
    BayClaim *c = bay_find_claim_by_va(proc->cabin, user_va);
    return c ? c->bay->user_size : 0;
}

void BayCleanupCabin(struct cabin_t *cabin)
{
    if (!cabin) return;

    spin_lock(&cabin->bay_lock);
    BayClaim *to_free = (BayClaim *)cabin->bay_claims_head;
    cabin->bay_claims_head = NULL;
    spin_unlock(&cabin->bay_lock);

    while (to_free) {
        BayClaim *next = to_free->cabin_next;
        BayObject *bay = to_free->bay;

        if (bay) {
            bay_memtag_detach_all(cabin, bay->chunks, bay->chunk_count,
                                   to_free->user_va_base, bay->chunk_size);
            bay_unmap_from_cabin(cabin, to_free->user_va_base,
                                 bay->chunk_count, bay->chunk_size);

            BayBucket *b = &g_bay_buckets[bay_bucket_index(bay->tag_id)];
            spin_lock(&b->lock);
            bay_drop_ref_locked(b, bay);

            if (g_stat_claims > 0) atomic_fetch_sub_u64(&g_stat_claims, 1);
            atomic_fetch_add_u64(&g_stat_releases, 1);
        }

        kfree(to_free);
        to_free = next;
    }
}

void BayStatsSnapshot(uint64_t out[4])
{
    out[0] = atomic_load_u64(&g_stat_objects);
    out[1] = atomic_load_u64(&g_stat_claims);
    out[2] = atomic_load_u64(&g_stat_pages);
    out[3] = atomic_load_u64(&g_stat_releases);
}