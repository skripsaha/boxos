/*
 * Bay — cross-cabin shared memory, tag-driven, refcounted.
 *
 * Implementation rationale: see header (bay.h). This file is the engine
 * that turns a tag string + size into a per-cabin user-VA mapped over
 * physical pages shared with every other cabin that opened the same
 * tag.
 *
 * Ownership — a claim belongs to the CABIN, never to the strand that
 * opened it. The mapping a claim carries lives in the cabin's address
 * space, which every strand of that cabin runs in and can already hold a
 * pointer into; tearing it down because one strand finished pulls the
 * floor out from under siblings that did nothing wrong. The key to the
 * shared room is issued to the flat, not to one occupant: when one of
 * them moves out, the door is not bricked up. Exactly two doors lead out
 * of a claim and there is no third — an explicit BayReleaseInternal, and
 * the cabin's own teardown (BayCleanupCabin).
 *
 * The rejected alternative was the one that used to stand here: record
 * the opening strand on the claim and, at strand death, splice out just
 * its rows. It reads like tidy bookkeeping and it is an unmap of live
 * memory. The console found it the hard way — one cabin-wide pointer to
 * the shared print order, opened by whichever strand happened to print
 * first, vanished under all its siblings the moment that strand finished,
 * and the next print faulted in code that had been working perfectly.
 * Since the opener is never consulted again, the claim does not carry it.
 *
 * Concurrency model:
 *   - bay_buckets[]: hash table keyed by tag_id. Each bucket has its
 *     own spinlock; cross-tag operations never serialize.
 *   - BayObject.ref_count: atomic. Drop-to-zero is detected inside the
 *     bucket lock so revive-during-destroy races are impossible (the
 *     destroyer holds the lock that any opener must take).
 *   - Per-cabin claim list: protected by cabin->bay_lock. Walks are
 *     O(N_claims_in_this_cabin) which is small (single-digit typical).
 *   - Lock order is cabin->bay_lock and bucket->lock, never nested: the
 *     list surgery runs under bay_lock, the heavy release outside it.
 *
 * Page-class policy:
 *   - size >= 2 MB: pmm_alloc(512) per chunk, vmm_map_huge_2m
 *   - else: pmm_alloc(N) for the whole thing, vmm_map_pages
 *
 * Falls back to 4 KB pages when buddy can't deliver a 2 MB chunk.
 * Memory ordering: we lean on the bucket lock as the sequentially-
 * consistent boundary; atomic ref_count uses ACQUIRE/RELEASE only to
 * publish the bucket's "I'm dying" decision.
 */

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

/* ─────────────────────────────────────────────────────────────────────
 * Page class. We deal in two classes only: 4 KiB (PMM_PAGE_SIZE) and
 * 2 MiB (VMM_LARGE_PAGE_2M_SIZE). 1 GiB would require BUDDY_MAX_ORDER
 * to be bumped from 14 to 18 — out of scope for v1.
 *
 * BAY_HUGE_THRESHOLD is the cut-off at which we try 2 MiB chunks; it
 * equals the chunk size by design so a single chunk is always at least
 * one PDE leaf.
 * ───────────────────────────────────────────────────────────────────── */
#define BAY_PAGE_CLASS_4K   12u
#define BAY_PAGE_CLASS_2M   21u

#define BAY_HUGE_THRESHOLD  VMM_LARGE_PAGE_2M_SIZE
#define BAY_HUGE_SIZE       VMM_LARGE_PAGE_2M_SIZE
#define BAY_HUGE_PAGES      VMM_LARGE_PAGE_2M_PAGES

/* ─────────────────────────────────────────────────────────────────────
 * Hash table.
 * ───────────────────────────────────────────────────────────────────── */
#define BAY_BUCKETS         256u
#define BAY_BUCKET_MASK     (BAY_BUCKETS - 1)

typedef struct BayBucket {
    spinlock_t  lock;
    BayObject  *head;
} BayBucket;

static BayBucket g_bay_buckets[BAY_BUCKETS];

/* Global stats (snapshot-only — informational). */
static volatile uint64_t g_stat_objects;
static volatile uint64_t g_stat_claims;
static volatile uint64_t g_stat_pages;
static volatile uint64_t g_stat_releases;

static inline uint32_t bay_bucket_index(uint16_t tag_id)
{
    /* tag_id is 16 bits and TagFS interns roughly densely from 0 up,
     * so a low-order mask gives good spread. xor-fold the high half so
     * any hot 256-id range still distributes. */
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

/* ─────────────────────────────────────────────────────────────────────
 * Tag resolution. Identical pattern to TouchTagResolve but returns a
 * single id (Bay does not need wildcard semantics — every Bay is a
 * concrete (key:value) blob). If no value present, the bare key is
 * used; bay_open("video", ...) is a valid tag.
 * ───────────────────────────────────────────────────────────────────── */
static uint16_t bay_resolve_tag(const char *tag, bool intern_if_missing)
{
    if (!intern_if_missing) return tagfs_tag_lookup(tag);
    uint16_t id;
    return tagfs_tag_intern(tag, &id) == OK ? id : TAGFS_INVALID_TAG_ID;
}

/* ─────────────────────────────────────────────────────────────────────
 * Physical-page allocation. Returns 0 on failure.
 *
 * Chooses between 2 MB chunks and 4 KB chunks based on requested_size.
 * Falls back to 4 KB transparently if the buddy can't deliver a 2 MB
 * chunk (e.g. fragmentation, RAM smaller than 2 MB, etc.).
 *
 * Sets *out_class to the chosen page class and fills *out_chunk_count
 * + *out_chunk_size + the chunks[] array (allocated via kmalloc).
 * Caller owns chunks[] and must kfree it (and pmm_free each entry)
 * on tear-down.
 *
 * Note: chunks[] is sized to chunk_count; the caller must not assume
 * any larger capacity.
 * ───────────────────────────────────────────────────────────────────── */
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
            /* PMM can't deliver a 2 MiB chunk — fall back to 4 KiB for
             * the WHOLE Bay (mixed-class Bays would force per-chunk
             * size tracking in BayObject, not worth the complexity for
             * a rare fragmentation case). Free what we already grabbed
             * and rebuild chunks[] at 4 KiB granularity. The retry is
             * bounded — buddy_alloc(1) is O(log MAX_ORDER) and we issue
             * at most total_size / PMM_PAGE_SIZE of them. */
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
            /* 4 KiB also failed mid-loop — unwind everything. */
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

/* Default policy — 4 KiB or 2 MiB depending on size, fallback to 4 KiB
 * on huge-page exhaustion. */
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

/* Force 4 KiB chunks — used by encrypted (TME-MK) Bays until
 * vmm_map_huge_2m supports KeyID-bearing PTEs. Exported by name so
 * BayOpenInternal can request the 4 KiB-only allocation path
 * explicitly. */
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

/* Map every chunk into the cabin starting at user_va_base. Returns OK on
 * success (any partial progress is rolled back internally before
 * returning). The cabin, not a strand, is what these take: the page
 * tables being written are the cabin's and outlive any one strand. */
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
    /* NX on all user mappings — Bay is data, not code. */
    vmm_flags |= VMM_FLAG_NO_EXECUTE;

    /* For encrypted Bays, the per-cabin VA gets PTEs with the BAY's
     * KeyID embedded in the upper phys bits. The CPU encrypts every
     * store and decrypts every load using the key associated with that
     * KeyID; two cabins opening the SAME encrypted Bay both map with
     * the same KeyID and observe the same plaintext. */
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
            /* Unwind */
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

/* Phase 2C — register MemRegionAttach bookkeeping for every chunk this
 * cabin just mapped. Mirror flag set used in bay_map_into_cabin so the
 * stored orig_flags exactly match the live PTE flags. Calls are best-
 * effort: any per-chunk failure is silently skipped (region creation
 * race only — extremely rare; future enforcement walks will simply miss
 * that one chunk, never corrupt). */
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

/* Inverse of bay_memtag_attach_all — detach every chunk's attach record.
 * Idempotent; safe to call when no attach was registered for a chunk. */
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

/* Free every backing chunk via PMM. */
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

/* ─────────────────────────────────────────────────────────────────────
 * Per-cabin VA reservation. Bump-allocated; never reused. The 32 TiB
 * Bay window is large enough that fragmentation is irrelevant for any
 * realistic workload (release leaves "holes" but the bump cursor only
 * goes up, so we waste at most ~32 TiB across the lifetime of a cabin
 * which is fine — actual physical pages are properly freed and
 * available for the NEXT Bay).
 *
 * Each allocation aligns up to the page class so 2 MB chunks land on
 * 2 MB-aligned VAs (mandatory for vmm_map_huge_2m).
 * ───────────────────────────────────────────────────────────────────── */
static uint64_t bay_reserve_user_va(cabin_t *cabin,
                                    uint64_t size,
                                    uint64_t page_align)
{
    if (page_align < PMM_PAGE_SIZE) page_align = PMM_PAGE_SIZE;

    uint64_t base;
    /* atomic_fetch_add with alignment requires explicit serialization;
     * the bay_lock is enough since reservations are off the hot path. */
    spin_lock(&cabin->bay_lock);
    uint64_t cur = (cabin->bay_va_next + page_align - 1) & ~(page_align - 1);
    /* Wrap-safe bounds check: `cur + size` could overflow if size is
     * close to UINT64_MAX (the boxlib boundary already caps at
     * BAY_MAX_OPEN_SIZE but a kernel-internal caller might not). */
    if (size > CABIN_BAY_END - cur) {
        spin_unlock(&cabin->bay_lock);
        return 0;
    }
    base = cur;
    cabin->bay_va_next = cur + size;
    spin_unlock(&cabin->bay_lock);
    return base;
}

/* ─────────────────────────────────────────────────────────────────────
 * Claim list helpers — singly-linked off cabin->bay_claims_head.
 * ───────────────────────────────────────────────────────────────────── */
/* Linking cannot fail. It used to: a claim opened while its strand was
 * being destroyed had to be refused, because the strand-death drain had
 * either already run or was about to, and a row linked past it would
 * leak. With the drain moved to cabin teardown that window is gone — the
 * only drain left runs when the cabin's last strand is already reaped,
 * and a strand executing this call is itself the proof that the cabin
 * still has one. */
static void bay_link_claim(cabin_t *cabin, BayClaim *claim)
{
    spin_lock(&cabin->bay_lock);
    claim->cabin_next = (BayClaim *)cabin->bay_claims_head;
    cabin->bay_claims_head = claim;
    spin_unlock(&cabin->bay_lock);
}

/* Unlink claim with matching user_va_base. Returns the claim or NULL
 * if not found. Caller is responsible for kfree.
 *
 * The VA alone decides, and that is deliberate: any strand of the cabin
 * may hand back a window any strand of the cabin opened, because the
 * window is the cabin's. Matching an opener here would re-introduce, in
 * the release path, exactly the strand ownership this file rejects. */
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

/* ─────────────────────────────────────────────────────────────────────
 * Bucket lookup — caller must hold bucket->lock. Returns BayObject or NULL.
 * ───────────────────────────────────────────────────────────────────── */
static BayObject *bay_bucket_find_locked(BayBucket *b, uint16_t tag_id)
{
    for (BayObject *o = b->head; o; o = o->bucket_next) {
        if (o->tag_id == tag_id) return o;
    }
    return NULL;
}

/* Unlink obj from b->head. Caller must hold b->lock. */
static void bay_bucket_unlink_locked(BayBucket *b, BayObject *obj)
{
    BayObject **p = &b->head;
    while (*p && *p != obj) p = &(*p)->bucket_next;
    if (*p) *p = obj->bucket_next;
    obj->bucket_next = NULL;
}

/* Total 4 KiB-page count backing a BayObject. Used by stats and by the
 * destroy path's PMM bookkeeping. Holding the bucket lock is enough —
 * chunk_count and chunk_size are immutable after BayObject creation. */
static inline uint64_t bay_total_pages(const BayObject *bay)
{
    uint64_t pages_per_chunk = (bay->chunk_size == BAY_HUGE_SIZE)
                               ? BAY_HUGE_PAGES : 1;
    return (uint64_t)bay->chunk_count * pages_per_chunk;
}

/* Drop one ref. Caller MUST hold b->lock on entry; this function
 * always RELEASES the lock before returning. If ref_count hits zero
 * the BayObject is unlinked, its physical chunks return to PMM, and
 * the struct is freed. The bucket lock is the serialization point
 * that makes the "ref-hit-zero" decision race-free: any concurrent
 * opener would block on the same lock.
 *
 * This used to be inlined four times across BayOpenInternal's failure
 * paths, BayReleaseInternal, and BayCleanupCabin — extracting it
 * killed 60+ lines of cleanup duplication. */
static void bay_drop_ref_locked(BayBucket *b, BayObject *bay)
{
    bay->ref_count--;
    if (bay->ref_count == 0) {
        /* Snapshot keyid + creator pid before unlinking — bay is freed
         * below and we need to call tme_keyid_free + adjust the
         * creator's quota counter after releasing all Bay state.
         * tme_keyid_free issues PCONFIG SET_KEY_RANDOM to re-key the
         * slot before returning it to the pool, so a future Bay that
         * reuses this KeyID slot CANNOT decrypt the old ciphertext. */
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
            /* Return the quota slot to the creator process IF still
             * alive. process_find returns NULL when the creator is
             * gone; in that case the quota counter is irrelevant
             * (the proc is destroyed, future allocs go through a
             * fresh proc with its own counter). PID-reuse race
             * accepted as a minor accounting drift. */
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

/* ─────────────────────────────────────────────────────────────────────
 * Public API — see header for semantics.
 * ───────────────────────────────────────────────────────────────────── */
error_t BayOpenInternal(struct process_t *proc,
                        const char *tag,
                        uint64_t requested_size,
                        uint32_t flags,
                        uint64_t *out_user_va,
                        uint64_t *out_actual_size)
{
    if (!proc || !proc->cabin || !tag || !out_user_va || !out_actual_size)
        return ERR_INVALID_ARGUMENT;

    /* Everything below the tag lookup is the cabin's — the VA window, the
     * page tables, the claim row, the encryption quota. The strand is kept
     * only for the two things that are genuinely its own: the pid the
     * MemTag gate judges, and the pid stamped on a freshly created Bay. */
    cabin_t *cabin = proc->cabin;

    /* Reject unknown flag bits early — they almost certainly indicate a
     * caller bug or an ABI mismatch with userspace, and silently
     * accepting them now would make future BAY_* additions ambiguous. */
    if (flags & ~BAY_FLAGS_MASK) return ERR_INVALID_ARGUMENT;

    bool wants_create = (flags & BAY_CREATE) != 0;
    uint16_t tag_id = bay_resolve_tag(tag, wants_create);
    if (tag_id == TAGFS_INVALID_TAG_ID) return ERR_TAG_NOT_FOUND;

    BayBucket *b = &g_bay_buckets[bay_bucket_index(tag_id)];

    /* Phase 1: see if the Bay already exists. */
    spin_lock(&b->lock);
    BayObject *bay = bay_bucket_find_locked(b, tag_id);
    if (bay) {
        /* Encryption capability must match between caller and existing
         * Bay — mismatched maps would decrypt with the wrong key and
         * silently return garbage. Gate explicitly. */
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

        /* TME-MK encryption requested? Enforce per-process quota
         * before touching any global state so a misbehaving cabin
         * can't drain the global KeyID pool. The KeyID reservation
         * + quota-counter increment happen as a single atomic step
         * under cabin->bay_lock so the quota is observed correctly
         * under concurrent BayOpen calls from the same cabin. */
        uint16_t alloc_keyid = 0;
        if (flags & BAY_ENCRYPTED) {
            if (!g_tme.mk_active) return ERR_UNSUPPORTED;
            spin_lock(&cabin->bay_lock);
            if (cabin->tme_keyids_held >= TME_QUOTA_PER_PROC) {
                spin_unlock(&cabin->bay_lock);
                return ERR_QUOTA_EXCEEDED;
            }
            /* Take a quota slot tentatively. If KeyID alloc fails
             * below we roll back. */
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

        /* Drop the lock while allocating phys pages — pmm_alloc may
         * take its own locks and we don't want to hold the bucket
         * lock across that. We retry under the bucket lock to handle
         * a concurrent creator winning the race. */
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

        /* For encrypted Bays, re-zero the pages via a kernel mapping
         * that bears the chosen KeyID. pmm_alloc_zero zeroed via the
         * identity map (KeyID 0); the user mapping uses KeyID N, so
         * without this re-zero the user would observe garbage on
         * first read (ciphertext_K0(zeros) decrypted with K_N).
         * tme_zero_pages_with_keyid serialises through one kernel VA
         * slot; the cost is N * (map + memset + unmap) which is cold-
         * path and bounded by chunk_count. */
        if (alloc_keyid) {
            bool huge = (cls == BAY_PAGE_CLASS_2M);
            for (uint32_t ci = 0; ci < cc; ci++) {
                if (chunks[ci] == 0) continue;
                error_t zrc = tme_zero_pages_with_keyid(
                    (uintptr_t)chunks[ci],
                    huge ? BAY_HUGE_PAGES : 1u,
                    alloc_keyid, huge);
                if (zrc != OK) {
                    /* Zero-fill failed mid-way — unwind allocated
                     * chunks and roll back KeyID + quota. */
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
        fresh->flags       = flags & ~BAY_RO;   /* Bay capability: RW by default */
        fresh->ref_count   = 0;
        fresh->create_pid  = proc->pid;
        fresh->create_tsc  = rdtsc();

        spin_lock(&b->lock);
        BayObject *winner = bay_bucket_find_locked(b, tag_id);
        if (winner) {
            /* Lost the race — discard our fresh copy, use the winner's.
             * Our alloc_keyid was reserved tentatively; release it and
             * give our quota slot back since the winner owns its own
             * KeyID (or none, if the winner isn't encrypted). */
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
                /* Winner vanished between unlock and re-lock — extremely
                 * unlikely (would require ref_count==0 within a few
                 * cycles), but bail safely. */
                spin_unlock(&b->lock);
                return ERR_TAG_NOT_FOUND;
            }
            /* Re-validate encryption capability against the winner
             * Bay we're now reusing. The earlier check ran against a
             * Bay that no longer exists; encryption-mismatch must
             * still be rejected on the actual winner. */
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

            /* MemTag integration: tag each chunk with the Bay's
             * key:value AND "purpose:bay" so the region surface is
             * queryable from outside the Bay subsystem. Chunks are
             * destroyed via pmm_free in bay teardown which triggers
             * MemTagPmmFreed → region cleanup. The key:value goes whole,
             * however long: measured first, then copied at its length. */
            size_t tag_len  = tagfs_tag_text(tag_id, NULL, 0);
            char  *tag_text = tag_len ? kmalloc(tag_len + 1) : NULL;
            if (tag_text && tagfs_tag_text(tag_id, tag_text, tag_len + 1) == tag_len) {
                size_t pages_per_chunk =
                    (cs == BAY_HUGE_SIZE) ? BAY_HUGE_PAGES : 1;
                for (uint32_t ci = 0; ci < cc; ci++) {
                    MemTagApplyByPhys((uintptr_t)chunks[ci], pages_per_chunk, tag_text);
                    MemTagApplyByPhys((uintptr_t)chunks[ci], pages_per_chunk, "purpose:bay");
                }
            }
            kfree(tag_text);
        }
    }

    /* At this point we hold b->lock and `bay` points to a live BayObject. */
    bay->ref_count++;
    uint64_t total_size = bay->total_size;
    uint64_t user_size  = bay->user_size;
    uint32_t chunk_count = bay->chunk_count;
    uint64_t chunk_size  = bay->chunk_size;
    spin_unlock(&b->lock);

    /* Reserve per-cabin VA and map. */
    uint64_t va = bay_reserve_user_va(cabin, total_size, chunk_size);
    if (va == 0) {
        spin_lock(&b->lock);
        bay_drop_ref_locked(b, bay);
        return ERR_NO_MEMORY;
    }

    /* Allocate BayClaim and link before mapping so a fault mid-map can
     * unwind cleanly. We keep the claim populated only after success. */
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

    /* Phase 2B M2 — enforce MemTag capabilities BEFORE mapping pages
     * into the cabin. Any guard tag on the Bay's region(s) that this
     * cabin doesn't hold → deny mapping + publish memtag:fault:denied. */
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

    /* Phase 2C — register attachment bookkeeping per chunk so future
     * revoke/grant sweeps can locate this cabin's PTEs without scanning
     * the page table. */
    bay_memtag_attach_all(cabin, bay->chunks, chunk_count, va, chunk_size, flags);

    /* Final link, and it cannot fail — see bay_link_claim. The window it
     * used to guard belonged to a drain that no longer happens here. */
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
        /* should not happen — defensive */
        kfree(claim);
        return ERR_INVALID_STATE;
    }

    /* Phase 2C — detach attachment bookkeeping before the PTE goes away. */
    bay_memtag_detach_all(cabin, bay->chunks, bay->chunk_count,
                          claim->user_va_base, bay->chunk_size);

    /* Unmap from this cabin first. After this the pages are exclusively
     * owned by other cabins (if any) — no strand of THIS cabin still has
     * a valid PTE pointing into the Bay. That is the whole weight of an
     * explicit release: the caller is speaking for every strand in the
     * flat, so it must be sure none of them is still reading. */
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

    /* The cabin's last strand has been reaped and its address space is
     * about to go, so every claim still on the list goes with it — the
     * whole list, unconditionally, because every row on it is this
     * cabin's. Nothing can link a new one: linking needs a strand, and
     * there is none left to have called us.
     *
     * The list is taken in one move under bay_lock and the heavy release
     * (detach / unmap / drop_ref, all of which take a bucket lock) runs
     * outside it — bay_lock and the bucket locks are never nested. */
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
