/*
 * brook.c — SPSC ordered streaming primitive.
 *
 * Mirrors the Bay subsystem (src/kernel/core/bay/bay.c) for its hashing,
 * tag resolution, claim-list lifecycle and PMM/VMM chunk allocation —
 * but layers a stream-specific role discipline (writer/reader pids in
 * BrookObject), a shared 4 KiB BrookHeader page that both peers map
 * read-write, and a futex-style wait/wake protocol that lets the
 * userspace hot path run lock-free (atomic head/tail updates) and only
 * descend into a syscall when the ring is full (writer) / empty (reader).
 *
 * See brook.h for the full lifecycle, ordering rules and lost-wakeup
 * proof. This file owns:
 *   - 256-bucket hash keyed by tag_id
 *   - chunk allocation (4 KiB vs implicit 2 MiB huge pages)
 *   - per-cabin VA reservation inside CABIN_BROOK_BASE..CABIN_BROOK_END
 *   - hard SPSC enforcement
 *   - wait/wake syscalls' kernel side
 *   - peer-death wake on strand death
 *
 * Two lifetimes, and the seat is vacated before the room is given back
 * ────────────────────────────────────────────────────────────────────
 * A claim's SEAT (writer/reader role) is the strand's and a claim's
 * MAPPING is the cabin's — brook.h argues why. The consequence that
 * shapes every function below is an ordering one: a BrookObject is kept
 * addressable by the MAPPING references on it, so any path that ends
 * both must end the seat FIRST and drop the mapping reference LAST.
 * Reversed, the last reference frees the object and the seat is vacated
 * through a dangling pointer.
 *
 * Lock order
 * ----------
 * Two locks, and they are NEVER NESTED:
 *
 *   cabin->brook_lock  guards the cabin's claim list AND the `holder`
 *                      field on every row of it.
 *   bucket->lock       guards the BrookObject — its role pids, its
 *                      ref_count, and the shared header page's alive
 *                      flags and bells.
 *
 * Every path is therefore written as short alternating sections: locate
 * or unlink the row under brook_lock, do the BrookObject work under the
 * bucket lock, ring outside both. What makes that safe without a nested
 * hold is `holder`: while it is non-zero the row is pinned (no sibling
 * may release it, and cabin teardown cannot run while a strand lives),
 * so a pointer taken under brook_lock is still good when the bucket
 * lock is taken a moment later.
 *
 * The rejected alternative was to take the bucket lock inside
 * brook_lock and re-stamp everything in one breath. It is simpler to
 * read and it introduces the file's first lock nesting, for a window
 * that `holder` already closes.
 */

#include "brook.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "klib.h"
#include "tagfs.h"
#include "tag_registry.h"
#include "memtag.h"
#include "atomics.h"
#include "error.h"
#include "kernel_config.h"
#include "acpi.h"          /* acpi_get_numa, ACPI_NUMA_DOMAIN_UNKNOWN */
#include "amp.h"           /* amp_get_core_index, g_amp.cores */
#include "cpuid.h"         /* g_cpu_caps for WAITPKG + monitor line size */
#include "kring.h"         /* KResultPush — the kernel rings a departing peer's bell */
#include "result.h"        /* Result */

/* ─────────────────────────────────────────────────────────────────────
 * Page-class policy mirrors Bay. 4 KiB by default; 2 MiB when total
 * slot size ≥ 2 MiB AND a 2 MiB-aligned chunk can be carved. 1 GiB
 * pages are out of scope for v1 (BUDDY_MAX_ORDER caps order at 14).
 *
 * NOTE: the header page is always exactly 1 × 4 KiB regardless of
 * page class — it carries only the 128 B BrookHeader plus padding.
 * ───────────────────────────────────────────────────────────────────── */
#define BROOK_PAGE_CLASS_4K     12u
#define BROOK_PAGE_CLASS_2M     21u
#define BROOK_HUGE_THRESHOLD    VMM_LARGE_PAGE_2M_SIZE
#define BROOK_HUGE_SIZE         VMM_LARGE_PAGE_2M_SIZE
#define BROOK_HUGE_PAGES        VMM_LARGE_PAGE_2M_PAGES

/* ─────────────────────────────────────────────────────────────────────
 * Hash table — 256 buckets, per-bucket spinlock. Cross-tag operations
 * never serialise. Identical sizing to Bay; the typical concurrent
 * Brook count per cabin is single-digit so 256 is plenty.
 * ───────────────────────────────────────────────────────────────────── */
#define BROOK_BUCKETS           256u
#define BROOK_BUCKET_MASK       (BROOK_BUCKETS - 1)

typedef struct BrookBucket {
    spinlock_t   lock;
    BrookObject *head;
} BrookBucket;

static BrookBucket g_brook_buckets[BROOK_BUCKETS];

/* The kernel's half of the bell — defined near BrookBellUnrung, declared here
 * because both release paths use it and both sit above it. */
static uint32_t brook_take_survivor_bell_locked(BrookHeader *kh, uint32_t departing_role);
static void     brook_ring_pid(uint32_t who);

static volatile uint64_t g_stat_objects;
static volatile uint64_t g_stat_claims;
static volatile uint64_t g_stat_pages;
static volatile uint64_t g_stat_releases;

static inline uint32_t brook_bucket_index(uint16_t tag_id)
{
    return ((uint32_t)tag_id ^ ((uint32_t)tag_id >> 4)) & BROOK_BUCKET_MASK;
}

void BrookInit(void)
{
    for (uint32_t i = 0; i < BROOK_BUCKETS; i++) {
        spinlock_init(&g_brook_buckets[i].lock);
        g_brook_buckets[i].head = NULL;
    }
    debug_printf("[Brook] init: %u buckets, 2 MiB huge-page threshold\n",
                 (unsigned)BROOK_BUCKETS);

    /* Real-HW posture log. Brook's wait path correctness depends on
     * three runtime invariants:
     *
     *   - UMONITOR cacheline granularity (CPUID.05H) — the BrookHeader
     *     layout (writer state on CL0, reader state on CL1) assumes 64 B
     *     cachelines. Other sizes don't break correctness (every store
     *     to the monitored line wakes the watcher, regardless of line
     *     size) but expose either false-sharing penalty (line > 64 B,
     *     CL0 and CL1 share one monitor line — spurious cross-peer
     *     wakes) or unused padding (line < 64 B).
     *
     *   - WAITPKG (CPUID.07H:ECX[5]) — userspace cpu_has_waitpkg picks
     *     the UMWAIT fast path; otherwise the PAUSE+yield fallback.
     *     This bit is the kernel-wide INTERSECTION (per_core.c calls
     *     cpu_intersect_features_ap on every AP) so heterogeneous P+E
     *     CPUs surface here correctly.
     *
     *   - NUMA topology (SRAT) — brook_alloc_zero_in_domain places ring
     *     pages on the opener's home node when SRAT is present. Log
     *     domain count so the operator sees the placement landscape.
     *
     * One line per concern; quiet by default on uniform single-domain
     * single-vendor configs (the common case). */
    {
        unsigned line_min = (unsigned)g_cpu_caps.monitor_line_min;
        unsigned line_max = (unsigned)g_cpu_caps.monitor_line_max;
        if (line_min == 0) line_min = 64;
        if (line_max == 0) line_max = 64;

        const char *wait_path =
            g_cpu_caps.has_waitpkg ? "UMWAIT" : "PAUSE+yield";

        const acpi_numa_info_t *n = acpi_get_numa();
        unsigned domain_count = (n && n->present) ? n->domain_count : 0;

        if (line_min == 64 && line_max == 64 && domain_count <= 1) {
            debug_printf("[Brook] wait path=%s, monitor line=64 B, uniform memory\n",
                         wait_path);
        } else {
            debug_printf("[Brook] wait path=%s, monitor line=%u..%u B, NUMA domains=%u\n",
                         wait_path, line_min, line_max, domain_count);
        }

        /* BrookHeader layout sanity: with line_max > 64 the writer-side
         * cacheline (CL0) and reader-side cacheline (CL1) collide on
         * one monitor line. Correctness preserved (per SDM Vol 2A
         * UMONITOR — any store to the monitored line wakes the
         * watcher) but every push/pop pings both peers. Log the
         * scenario so the operator can correlate any perf surprise. */
        if (line_max > 64) {
            debug_printf("[Brook] note: monitor line %u B > 64 B "
                         "(writer/reader cachelines share one line — wakes correct, "
                         "expect cross-peer wake amplification)\n",
                         line_max);
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * Tag resolution. Same pattern as Bay/Touch.
 * ───────────────────────────────────────────────────────────────────── */
static uint16_t brook_resolve_tag(const char *tag, bool intern_if_missing)
{
    if (!intern_if_missing) return tagfs_tag_lookup(tag);
    uint16_t id;
    return tagfs_tag_intern(tag, &id) == OK ? id : TAGFS_INVALID_TAG_ID;
}

/* ─────────────────────────────────────────────────────────────────────
 * NUMA-aware backing-page allocation.
 *
 * SPSC streams are sensitive to cross-node coherence: on a 2-socket
 * Xeon / EPYC the cross-socket cache-coherent bounce of a tail update
 * can be 3-5× slower than intra-socket. Brook's hot path is exactly
 * that — producer writes tail, consumer's cacheline goes M → I → S.
 * Co-locating the ring + header pages with the caller's CPU keeps the
 * coherence chatter on-socket.
 *
 * Determine the preferred NUMA domain from the current CPU's APIC ID
 * mapped through SRAT (acpi_numa_info_t.cpus[]). ACPI_NUMA_DOMAIN_UNKNOWN
 * when there's no SRAT (uniform memory machine), no matching CPU entry,
 * or CPUs are running before SRAT parse — every caller of
 * pmm_alloc_in_domain handles UNKNOWN by falling through to any-zone
 * buddy_alloc, so the unknown path is the safe default.
 *
 * The "opener's CPU wins" heuristic is intentional: in BoxOS the
 * opening cabin typically also produces or consumes — proc-spawn
 * affinity tends to keep the producer/consumer on the same socket as
 * the original creator. Cross-socket spawns lose ~10 % vs an optimal
 * placement; uniform-memory machines see no difference. ───────────── */
static uint32_t brook_preferred_domain(void)
{
    const acpi_numa_info_t *n = acpi_get_numa();
    if (!n || !n->present) return ACPI_NUMA_DOMAIN_UNKNOWN;

    uint8_t core = amp_get_core_index();
    if (core >= MAX_CORES) return ACPI_NUMA_DOMAIN_UNKNOWN;
    uint32_t lapic_id = g_amp.cores[core].lapic_id;

    for (uint16_t i = 0; i < n->cpu_count; i++) {
        if (!n->cpus[i].enabled) continue;
        if (n->cpus[i].apic_id == lapic_id) return n->cpus[i].domain;
    }
    return ACPI_NUMA_DOMAIN_UNKNOWN;
}

/* Zero-fill wrapper around pmm_alloc_in_domain. pmm_alloc_in_domain
 * returns raw uncleared pages (vs pmm_alloc_zero); we must memset to
 * preserve the "ring starts in a known state" invariant for the
 * BrookHeader and the lock-free push path.
 *
 * Falls back to any-zone pmm_alloc_zero on UNKNOWN domain or in-domain
 * failure — exactly what pmm_alloc_in_domain already does on the
 * domain side, but we keep the symmetric fallback at the brook layer
 * so the zero-fill always happens regardless of which path served the
 * allocation. */
static void *brook_alloc_zero_in_domain(size_t pages, uint32_t domain)
{
    if (domain == ACPI_NUMA_DOMAIN_UNKNOWN) {
        return pmm_alloc_zero(pages);
    }
    void *p = pmm_alloc_in_domain(pages, domain);
    if (!p) {
        /* Domain-local allocator + global fallback inside it failed.
         * Try the zero-fill any-zone path one more time — different
         * code path inside the buddy might find pages even though the
         * range-constrained walk didn't. */
        p = pmm_alloc_zero(pages);
        if (p) return p;
        return NULL;
    }
    memset((void *)vmm_phys_to_virt((uintptr_t)p), 0, pages * PMM_PAGE_SIZE);
    return p;
}

/* ─────────────────────────────────────────────────────────────────────
 * Allocate slot-region chunks. Same fall-back-to-4K pattern as Bay:
 * try 2 MiB chunks first when total ≥ 2 MiB, drop to 4 KiB for the
 * WHOLE region if even one chunk can't be served at 2 MiB granularity.
 *
 * Returns OK + fills out_* on success. On failure, every partially-
 * allocated chunk is returned to PMM before propagating the error.
 * ───────────────────────────────────────────────────────────────────── */
static error_t brook_alloc_slot_chunks(uint64_t total_size,
                                       uint32_t domain,
                                       uint16_t *out_class,
                                       uint64_t *out_chunk_size,
                                       uint32_t *out_chunk_count,
                                       uint64_t **out_chunks)
{
    if (total_size == 0) return ERR_INVALID_ARGUMENT;

    uint16_t cls;
    uint64_t cs;
    if (total_size >= BROOK_HUGE_THRESHOLD) {
        cls = BROOK_PAGE_CLASS_2M;
        cs  = BROOK_HUGE_SIZE;
    } else {
        cls = BROOK_PAGE_CLASS_4K;
        cs  = PMM_PAGE_SIZE;
    }
    uint32_t cc = (uint32_t)((total_size + cs - 1) / cs);

    uint64_t *chunks = (uint64_t *)kmalloc(cc * sizeof(uint64_t));
    if (!chunks) return ERR_NO_MEMORY;
    memset(chunks, 0, cc * sizeof(uint64_t));

    for (uint32_t i = 0; i < cc; i++) {
        size_t pages = (cls == BROOK_PAGE_CLASS_2M) ? BROOK_HUGE_PAGES : 1;
        void *p = brook_alloc_zero_in_domain(pages, domain);
        if (!p && cls == BROOK_PAGE_CLASS_2M) {
            /* Drop the partial 2 MiB run and rebuild at 4 KiB. */
            for (uint32_t j = 0; j < i; j++) {
                if (chunks[j]) pmm_free((void *)chunks[j], BROOK_HUGE_PAGES);
            }
            kfree(chunks);

            cls = BROOK_PAGE_CLASS_4K;
            cs  = PMM_PAGE_SIZE;
            cc  = (uint32_t)((total_size + cs - 1) / cs);
            chunks = (uint64_t *)kmalloc(cc * sizeof(uint64_t));
            if (!chunks) return ERR_NO_MEMORY;
            memset(chunks, 0, cc * sizeof(uint64_t));
            /* Re-enter the loop at i=0. The for-loop post-increment
             * runs on `continue`, so we set i = UINT32_MAX here; the
             * post-increment wraps to 0 and the body starts fresh. */
            i = (uint32_t)-1;
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

/* Map slot chunks into the cabin at user_va_base. Unwinds on failure.
 *
 * These take the CABIN and not a strand, and that is the change made
 * structural: the page tables being written are the cabin's and outlive
 * any one strand, and BrookCleanupCabin runs when there is no strand
 * left to pass. */
/* Phase 2C — register MemRegionAttach bookkeeping for the Brook header
 * (1 page) + each slot chunk. Mirror flag set used in brook_map_*_into_cabin
 * verbatim (USER_RW | NX). Best-effort per-chunk; future enforcement
 * walks tolerate missing entries. */
static void brook_memtag_attach_all(cabin_t *cabin,
                                     uint64_t hdr_phys,
                                     uint64_t va_header,
                                     uint64_t *slot_chunks,
                                     uint32_t slot_cc,
                                     uint64_t va_slots,
                                     uint64_t slot_cs)
{
    const uint64_t att_flags = VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE;

    if (hdr_phys != 0) {
        uint32_t hdr_rid = MemRegionFromPhys((uintptr_t)hdr_phys);
        if (hdr_rid != MEMTAG_INVALID_REGION_ID) {
            MemRegionAttachCabin(hdr_rid, (void *)cabin->vmm,
                                  va_header, 1,
                                  MEMTAG_ATTACH_CLASS_4K, att_flags);
        }
    }

    uint8_t  pc = (slot_cs == BROOK_HUGE_SIZE)
                  ? MEMTAG_ATTACH_CLASS_2M : MEMTAG_ATTACH_CLASS_4K;
    uint32_t pages_per_chunk = (slot_cs == BROOK_HUGE_SIZE)
                               ? (uint32_t)BROOK_HUGE_PAGES : 1u;
    for (uint32_t i = 0; i < slot_cc; i++) {
        if (slot_chunks[i] == 0) continue;
        uint64_t att_va = va_slots + (uint64_t)i * slot_cs;
        uint32_t rid    = MemRegionFromPhys((uintptr_t)slot_chunks[i]);
        if (rid != MEMTAG_INVALID_REGION_ID) {
            MemRegionAttachCabin(rid, (void *)cabin->vmm, att_va,
                                  pages_per_chunk, pc, att_flags);
        }
    }
}

/* Inverse — detach header + every slot chunk. Idempotent. */
static void brook_memtag_detach_all(cabin_t *cabin,
                                     uint64_t hdr_phys,
                                     uint64_t va_header,
                                     uint64_t *slot_chunks,
                                     uint32_t slot_cc,
                                     uint64_t va_slots,
                                     uint64_t slot_cs)
{
    if (hdr_phys != 0) {
        uint32_t hdr_rid = MemRegionFromPhys((uintptr_t)hdr_phys);
        if (hdr_rid != MEMTAG_INVALID_REGION_ID) {
            MemRegionDetachCabin(hdr_rid, (void *)cabin->vmm, va_header);
        }
    }
    for (uint32_t i = 0; i < slot_cc; i++) {
        if (slot_chunks[i] == 0) continue;
        uint64_t att_va = va_slots + (uint64_t)i * slot_cs;
        uint32_t rid    = MemRegionFromPhys((uintptr_t)slot_chunks[i]);
        if (rid != MEMTAG_INVALID_REGION_ID) {
            MemRegionDetachCabin(rid, (void *)cabin->vmm, att_va);
        }
    }
}

static error_t brook_map_slots_into_cabin(cabin_t *cabin,
                                          uint64_t user_va_base,
                                          uint64_t *chunks,
                                          uint32_t chunk_count,
                                          uint64_t chunk_size)
{
    if (!cabin || !cabin->vmm) return ERR_INVALID_ARGUMENT;

    /* RW + NX — Brook payload is data, not code. Both peers always get
     * RW; there is no read-only-reader flag (the kernel cannot enforce
     * SPSC discipline if the reader can write the cursor, and the
     * frame slots are exclusive-write-by-producer by protocol). */
    const uint64_t vmm_flags = VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE;

    for (uint32_t i = 0; i < chunk_count; i++) {
        uint64_t va = user_va_base + (uint64_t)i * chunk_size;
        uint64_t pa = chunks[i];

        bool ok;
        if (chunk_size == BROOK_HUGE_SIZE) {
            ok = vmm_map_huge_2m(cabin->vmm, va, pa, vmm_flags);
        } else {
            vmm_map_result_t r = vmm_map_page(cabin->vmm, va, pa, vmm_flags);
            ok = r.success;
        }

        if (!ok) {
            for (uint32_t j = 0; j < i; j++) {
                uint64_t uva = user_va_base + (uint64_t)j * chunk_size;
                if (chunk_size == BROOK_HUGE_SIZE) vmm_unmap_huge_2m(cabin->vmm, uva);
                else                               vmm_unmap_page(cabin->vmm, uva);
            }
            return ERR_NO_MEMORY;
        }
    }
    return OK;
}

static void brook_unmap_slots_from_cabin(cabin_t *cabin,
                                         uint64_t user_va_base,
                                         uint32_t chunk_count,
                                         uint64_t chunk_size)
{
    if (!cabin || !cabin->vmm) return;
    for (uint32_t i = 0; i < chunk_count; i++) {
        uint64_t uva = user_va_base + (uint64_t)i * chunk_size;
        if (chunk_size == BROOK_HUGE_SIZE) vmm_unmap_huge_2m(cabin->vmm, uva);
        else                               vmm_unmap_page(cabin->vmm, uva);
    }
}

/* Map / unmap the 4 KiB BrookHeader page. */
static bool brook_map_header_into_cabin(cabin_t *cabin,
                                        uint64_t user_va_header,
                                        uint64_t header_phys)
{
    if (!cabin || !cabin->vmm) return false;
    const uint64_t vmm_flags = VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE;
    vmm_map_result_t r = vmm_map_page(cabin->vmm, user_va_header,
                                      header_phys, vmm_flags);
    return r.success;
}

static void brook_unmap_header_from_cabin(cabin_t *cabin,
                                          uint64_t user_va_header)
{
    if (!cabin || !cabin->vmm) return;
    vmm_unmap_page(cabin->vmm, user_va_header);
}

/* Free every backing chunk + the header page via PMM. */
static void brook_free_backing(BrookObject *brook)
{
    if (!brook) return;
    if (brook->slot_chunks) {
        size_t pages_per_chunk = (brook->slot_chunk_size == BROOK_HUGE_SIZE)
                                 ? BROOK_HUGE_PAGES : 1;
        for (uint32_t i = 0; i < brook->slot_chunk_count; i++) {
            if (brook->slot_chunks[i] == 0) continue;
            pmm_free((void *)brook->slot_chunks[i], pages_per_chunk);
        }
        kfree(brook->slot_chunks);
        brook->slot_chunks = NULL;
    }
    if (brook->header_phys) {
        pmm_free((void *)brook->header_phys, 1);
        brook->header_phys = 0;
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * Per-cabin VA reservation. Bump-allocator within
 * CABIN_BROOK_BASE..CABIN_BROOK_END. Same model as Bay — released VAs
 * are NOT recycled (the 31 TiB window is enormous; fragmentation
 * irrelevant for any realistic workload). Each Brook open reserves
 * [header_page | aligned slot region]. The header page is always 4 KiB
 * (one PMM_PAGE_SIZE), the slot region is aligned to slot_chunk_size
 * so huge-page mappings land on PDE boundaries.
 * ───────────────────────────────────────────────────────────────────── */
static error_t brook_reserve_user_va(cabin_t *cabin,
                                     uint64_t slot_total_size,
                                     uint64_t slot_chunk_size,
                                     uint64_t *out_va_header,
                                     uint64_t *out_va_slots)
{
    if (!cabin) return ERR_INVALID_ARGUMENT;
    if (slot_chunk_size < PMM_PAGE_SIZE) slot_chunk_size = PMM_PAGE_SIZE;

    spin_lock(&cabin->brook_lock);

    /* Page-align cursor for the header. */
    uint64_t cur = (cabin->brook_va_next + (PMM_PAGE_SIZE - 1))
                   & ~(PMM_PAGE_SIZE - 1);
    uint64_t va_header = cur;

    /* Slot region aligned to slot_chunk_size (= 2 MiB for huge). */
    uint64_t va_slots = (va_header + PMM_PAGE_SIZE + slot_chunk_size - 1)
                        & ~(slot_chunk_size - 1);

    /* Wrap-safe bounds check.
     *   - va_slots < va_header: ALIGN_UP wrapped through 0
     *   - va_slots >= CABIN_BROOK_END: header took us past the window
     *     end (next subtraction would underflow)
     *   - slot_total_size > CABIN_BROOK_END - va_slots: slot region
     *     would spill past the window
     *   - Also reject overflow of the final cursor (va_slots + size
     *     could wrap past 0 on the upper end). */
    if (va_slots < va_header ||
        va_slots >= CABIN_BROOK_END ||
        slot_total_size > CABIN_BROOK_END - va_slots ||
        va_slots + slot_total_size < va_slots) {
        spin_unlock(&cabin->brook_lock);
        return ERR_NO_MEMORY;
    }

    cabin->brook_va_next = va_slots + slot_total_size;

    spin_unlock(&cabin->brook_lock);

    *out_va_header = va_header;
    *out_va_slots  = va_slots;
    return OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * Claim list helpers — cabin->brook_claims_head singly-linked.
 * Identical shape to Bay's claim list.
 * ───────────────────────────────────────────────────────────────────── */
/* Linking cannot fail. It used to: a claim opened while its strand was
 * being destroyed had to be refused, because the strand-death hook
 * drained the whole list and a row linked past it would leak. That
 * drain is gone — strand death now vacates seats and touches no row's
 * link — and the only drain left, BrookCleanupCabin, cannot run while a
 * strand exists to be calling this. */
static void brook_link_claim(cabin_t *cabin, BrookClaim *claim)
{
    spin_lock(&cabin->brook_lock);
    claim->cabin_next = (BrookClaim *)cabin->brook_claims_head;
    cabin->brook_claims_head = claim;
    spin_unlock(&cabin->brook_lock);
}

/* Unlink the claim at this VA on behalf of `by_pid`, or refuse.
 *
 * The VA alone is not enough, and this is the one place Brook parts
 * company with Bay. A claim whose seat is still warm may only be handed
 * back by the strand sitting in it: unlinking somebody else's row
 * unmaps the pages and leaves the seat stamped on a BrookObject that no
 * longer has a claim to vacate it — a role slot lost for the lifetime
 * of the tag, on a single-occupancy stream. A vacated claim
 * (holder == 0) is nothing but the cabin's mapping, and then any strand
 * of the cabin may give it back, exactly as in Bay.
 *
 * *why distinguishes the two failures for the caller; the claim is left
 * on the list in both. */
static BrookClaim *brook_unlink_claim_by_va(cabin_t *cabin,
                                            uint64_t user_va_header,
                                            uint32_t by_pid,
                                            error_t *why)
{
    spin_lock(&cabin->brook_lock);
    BrookClaim **p = (BrookClaim **)&cabin->brook_claims_head;
    while (*p) {
        if ((*p)->user_va_header == user_va_header) {
            BrookClaim *hit = *p;
            if (hit->holder != 0 && hit->holder != by_pid) {
                spin_unlock(&cabin->brook_lock);
                *why = ERR_PERMISSION_DENIED;
                return NULL;
            }
            *p = hit->cabin_next;
            spin_unlock(&cabin->brook_lock);
            *why = OK;
            return hit;
        }
        p = &(*p)->cabin_next;
    }
    spin_unlock(&cabin->brook_lock);
    *why = ERR_TAG_NOT_FOUND;
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────
 * Bucket helpers — caller must hold bucket->lock.
 * ───────────────────────────────────────────────────────────────────── */
static BrookObject *brook_bucket_find_locked(BrookBucket *b, uint16_t tag_id)
{
    for (BrookObject *o = b->head; o; o = o->bucket_next) {
        if (o->tag_id == tag_id) return o;
    }
    return NULL;
}

static void brook_bucket_unlink_locked(BrookBucket *b, BrookObject *obj)
{
    BrookObject **p = &b->head;
    while (*p && *p != obj) p = &(*p)->bucket_next;
    if (*p) *p = obj->bucket_next;
    obj->bucket_next = NULL;
}

static inline uint64_t brook_total_pages(const BrookObject *brook)
{
    uint64_t pages_per_chunk = (brook->slot_chunk_size == BROOK_HUGE_SIZE)
                               ? BROOK_HUGE_PAGES : 1;
    uint64_t slot_pages = (uint64_t)brook->slot_chunk_count * pages_per_chunk;
    return slot_pages + 1;  /* +1 for header */
}

/* Drop one MAPPING reference. Caller MUST hold b->lock on entry;
 * releases the lock before returning. If ref_count hits zero, the
 * BrookObject is unlinked, backing pages return to PMM, and the struct
 * is freed.
 *
 * Zero here means no cabin maps these pages any more — which is a
 * stronger fact than "both peers left", and deliberately so: roles end
 * when strands do, mappings when cabins do, and only the second is a
 * safe moment to hand the ring back to the PMM. */
static void brook_drop_ref_locked(BrookBucket *b, BrookObject *brook)
{
    brook->ref_count--;
    if (brook->ref_count == 0) {
        brook_bucket_unlink_locked(b, brook);
        spin_unlock(&b->lock);

        atomic_fetch_sub_u64(&g_stat_objects, 1);
        atomic_fetch_sub_u64(&g_stat_pages, brook_total_pages(brook));
        brook_free_backing(brook);
        kfree(brook);
        return;
    }
    spin_unlock(&b->lock);
}

/* ─────────────────────────────────────────────────────────────────────
 * Header convenience — translate BrookObject.header_phys to a writable
 * kernel virtual pointer so the kernel side of wait/wake can read and
 * mutate the futex flags.
 *
 * vmm_phys_to_virt resolves through the kernel direct map, which is
 * mapped write-back (WB). That's the cacheability class UMONITOR
 * requires for the userspace peer's monitor to fire: Intel SDM
 * Vol 2A UMONITOR — "The address range must use memory of the
 * write-back type. Only write-back memory is guaranteed to correctly
 * trigger the monitoring hardware." If the kernel direct map were ever
 * remapped UC/WC, the kernel's alive=0 store would bypass caches and
 * the userspace peer's UMWAIT would never wake on it. The userspace
 * mapping is also VMM_FLAGS_USER_RW (no PCD/PWT/PAT bits) → WB.
 * Both aliases agree; alive-flag writes go through the cache and
 * wake the monitor as expected. ────────────────────────────────────── */
static inline BrookHeader *brook_kernel_header(const BrookObject *brook)
{
    return (BrookHeader *)vmm_phys_to_virt(brook->header_phys);
}

/* Initialise a freshly-allocated header page. */
static void brook_header_init(BrookHeader *hdr,
                              uint32_t frame_size,
                              uint32_t frame_count)
{
    memset(hdr, 0, sizeof(*hdr));
    hdr->head                 = 0;
    hdr->tail                 = 0;
    hdr->frame_size           = frame_size;
    hdr->frame_count          = frame_count;
    hdr->magic                = BROOK_HEADER_MAGIC;
    hdr->writer_alive         = 0;   /* set when writer attaches */
    hdr->reader_alive         = 0;   /* set when reader attaches */
    hdr->writer_ever_attached = 0;
    hdr->reader_ever_attached = 0;
}

/* Power-of-two helper. */
static inline bool brook_is_pow2(uint32_t x)
{
    return x != 0 && (x & (x - 1)) == 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * Vacate ONE seat. Takes and drops the bucket lock itself; returns the
 * pid whose bell is to be rung, or 0 if nobody on the far side is
 * asleep. The ring happens in the caller, outside every lock, because
 * ringing walks the target's page tables.
 *
 * `holder` is who is being lifted out of the seat — the claim's, never
 * the caller's, because after a hand-over those are different strands
 * and it is the claim that knows which.
 *
 * ‼ The caller must still be holding this claim's MAPPING reference. It
 * is what keeps `brook` addressable: drop it first and the last
 * reference frees the object, and this function then clears a role pid
 * through a dangling pointer. Every caller below is written seat-first,
 * reference-last for that reason.
 * ───────────────────────────────────────────────────────────────────── */
static uint32_t brook_role_end(BrookObject *brook, uint32_t role, uint32_t holder)
{
    if (!brook || holder == 0) return 0;

    BrookBucket *b = &g_brook_buckets[brook_bucket_index(brook->tag_id)];
    spin_lock(&b->lock);

    /* Mark our side dead in the shared header. CAS expected=1→0 so a
     * concurrent FROZEN set by the surviving peer (its single-session EOF
     * decision) is preserved — alive becomes sticky FROZEN, blocking any
     * future attach to this session. */
    BrookHeader *kh = brook_kernel_header(brook);
    volatile uint32_t *alive = (role == BROOK_WRITER) ? &kh->writer_alive
                                                      : &kh->reader_alive;
    uint32_t one = 1u;
    __atomic_compare_exchange_n(alive, &one, 0u, false,
                                __ATOMIC_RELEASE, __ATOMIC_RELAXED);

    /* The seat is cleared only if it still names the strand we are
     * lifting out. Anything else means the seat has already moved on and
     * is not ours to vacate. */
    uint32_t *seat = (role == BROOK_WRITER) ? &brook->writer_pid
                                            : &brook->reader_pid;
    if (*seat == holder) *seat = 0;

    uint32_t ring_pid = brook_take_survivor_bell_locked(kh, role);

    spin_unlock(&b->lock);
    return ring_pid;
}

/* ─────────────────────────────────────────────────────────────────────
 * BrookOpenInternal
 * ───────────────────────────────────────────────────────────────────── */
error_t BrookOpenInternal(struct process_t *proc,
                          const char *tag,
                          uint32_t frame_size,
                          uint32_t frame_count,
                          uint32_t flags,
                          uint64_t *out_user_va_header,
                          uint64_t *out_user_va_slots,
                          uint32_t *out_frame_size,
                          uint32_t *out_frame_count)
{
    if (!proc || !proc->cabin || !tag || !out_user_va_header ||
        !out_user_va_slots || !out_frame_size || !out_frame_count)
        return ERR_INVALID_ARGUMENT;

    if (proc->destroying) return ERR_INVALID_STATE;

    /* Everything below the tag lookup is the cabin's — the VA window, the
     * page tables, the claim row. The strand is kept for the two things
     * that are genuinely its own: the pid the MemTag gate judges, and the
     * pid that goes into the seat. */
    cabin_t *cabin = proc->cabin;

    if (flags & ~BROOK_FLAGS_MASK)                   return ERR_INVALID_ARGUMENT;
    uint32_t role = flags & (BROOK_WRITER | BROOK_READER);
    if (role != BROOK_WRITER && role != BROOK_READER) return ERR_INVALID_ARGUMENT;

    bool wants_create = (flags & BROOK_CREATE) != 0;
    if (wants_create) {
        if (frame_size  < BROOK_FRAME_SIZE_MIN  || frame_size  > BROOK_FRAME_SIZE_MAX)
            return ERR_INVALID_ARGUMENT;
        if (frame_count < BROOK_FRAME_COUNT_MIN || frame_count > BROOK_FRAME_COUNT_MAX)
            return ERR_INVALID_ARGUMENT;
        if (!brook_is_pow2(frame_count))             return ERR_INVALID_ARGUMENT;
        if ((uint64_t)frame_size * frame_count > BROOK_MAX_TOTAL_SIZE)
            return ERR_INVALID_ARGUMENT;
    }

    uint16_t tag_id = brook_resolve_tag(tag, wants_create);
    if (tag_id == TAGFS_INVALID_TAG_ID) return ERR_TAG_NOT_FOUND;

    BrookBucket *b = &g_brook_buckets[brook_bucket_index(tag_id)];

    /* Phase 1: probe for existing. */
    spin_lock(&b->lock);
    BrookObject *brook = brook_bucket_find_locked(b, tag_id);

    if (!brook) {
        if (!wants_create) {
            spin_unlock(&b->lock);
            return ERR_TAG_NOT_FOUND;
        }
        /* Drop the lock for the chunk allocation; we'll re-lock and
         * resolve any concurrent-create race. */
        spin_unlock(&b->lock);

        uint64_t slot_total = (uint64_t)frame_size * frame_count;
        /* NUMA placement hint: the cabin that creates the Brook gets
         * its ring + header on its CPU's home memory node. On uniform-
         * memory (no SRAT) machines this resolves to UNKNOWN and the
         * NUMA-aware helpers fall through to any-zone allocation. */
        uint32_t domain = brook_preferred_domain();
        uint16_t  cls;
        uint64_t  cs;
        uint32_t  cc;
        uint64_t *chunks;
        error_t rc = brook_alloc_slot_chunks(slot_total, domain,
                                             &cls, &cs, &cc, &chunks);
        if (rc != OK) return rc;

        void *hdr_phys = brook_alloc_zero_in_domain(1, domain);
        if (!hdr_phys) {
            for (uint32_t i = 0; i < cc; i++) {
                if (chunks[i]) pmm_free((void *)chunks[i],
                                        (cls == BROOK_PAGE_CLASS_2M) ? BROOK_HUGE_PAGES : 1);
            }
            kfree(chunks);
            return ERR_NO_MEMORY;
        }

        BrookObject *fresh = (BrookObject *)kmalloc(sizeof(BrookObject));
        if (!fresh) {
            pmm_free(hdr_phys, 1);
            for (uint32_t i = 0; i < cc; i++) {
                if (chunks[i]) pmm_free((void *)chunks[i],
                                        (cls == BROOK_PAGE_CLASS_2M) ? BROOK_HUGE_PAGES : 1);
            }
            kfree(chunks);
            return ERR_NO_MEMORY;
        }
        memset(fresh, 0, sizeof(*fresh));
        fresh->tag_id           = tag_id;
        fresh->slot_page_class  = cls;
        fresh->flags            = flags & ~(BROOK_WRITER | BROOK_READER);
        fresh->frame_size       = frame_size;
        fresh->frame_count      = frame_count;
        fresh->header_phys      = (uint64_t)hdr_phys;
        fresh->slot_total_size  = (uint64_t)cc * cs;
        fresh->slot_chunk_size  = cs;
        fresh->slot_chunk_count = cc;
        fresh->slot_chunks      = chunks;
        fresh->writer_pid       = 0;
        fresh->reader_pid       = 0;
        fresh->ref_count        = 0;

        brook_header_init(brook_kernel_header(fresh), frame_size, frame_count);

        spin_lock(&b->lock);
        BrookObject *winner = brook_bucket_find_locked(b, tag_id);
        if (winner) {
            /* Lost the race — discard our fresh copy, use the winner. */
            spin_unlock(&b->lock);
            brook_free_backing(fresh);
            kfree(fresh);
            spin_lock(&b->lock);
            brook = brook_bucket_find_locked(b, tag_id);
            if (!brook) {
                spin_unlock(&b->lock);
                return ERR_TAG_NOT_FOUND;
            }
        } else {
            fresh->bucket_next = b->head;
            b->head            = fresh;
            brook              = fresh;
            atomic_fetch_add_u64(&g_stat_objects, 1);
            atomic_fetch_add_u64(&g_stat_pages, brook_total_pages(fresh));

            /* MemTag integration: tag header + each slot chunk with the
             * Brook's key:value AND "purpose:brook" + "purpose:stream"
             * so the streaming buffers are queryable via tag algebra. The
             * key:value goes whole, however long: measured first, then
             * copied at its length. */
            size_t tag_len  = tagfs_tag_text(tag_id, NULL, 0);
            char  *tag_text = tag_len ? kmalloc(tag_len + 1) : NULL;
            if (tag_text && tagfs_tag_text(tag_id, tag_text, tag_len + 1) == tag_len) {
                MemTagApplyByPhys(fresh->header_phys, 1, tag_text);
                MemTagApplyByPhys(fresh->header_phys, 1, "purpose:brook");
                MemTagApplyByPhys(fresh->header_phys, 1, "purpose:brook-header");
                size_t pages_per_chunk =
                    (cs == BROOK_HUGE_SIZE) ? BROOK_HUGE_PAGES : 1;
                for (uint32_t ci = 0; ci < cc; ci++) {
                    MemTagApplyByPhys((uintptr_t)chunks[ci], pages_per_chunk, tag_text);
                    MemTagApplyByPhys((uintptr_t)chunks[ci], pages_per_chunk, "purpose:brook");
                    MemTagApplyByPhys((uintptr_t)chunks[ci], pages_per_chunk, "purpose:stream");
                }
            }
            kfree(tag_text);
        }
    }

    /* At this point we hold b->lock and `brook` points to a live BrookObject. */

    /* Shape check: if caller passed frame_size/count and they differ from
     * the existing Brook (and caller is not just opening), reject. This
     * keeps stream shape as part of tag identity — two cabins that ask
     * for different shapes can't accidentally share a stream. */
    if (frame_size != 0 && brook->frame_size != frame_size) {
        spin_unlock(&b->lock);
        return ERR_ALREADY_EXISTS;
    }
    if (frame_count != 0 && brook->frame_count != frame_count) {
        spin_unlock(&b->lock);
        return ERR_ALREADY_EXISTS;
    }

    /* Hard SPSC: second open with same role → ERR_BUSY. Same role +
     * same pid (re-open by us) is also rejected — boxlib should be
     * caching its handle. */
    if (role == BROOK_WRITER) {
        if (brook->writer_pid != 0) {
            spin_unlock(&b->lock);
            return ERR_BUSY;
        }
        brook->writer_pid = proc->pid;
    } else {
        if (brook->reader_pid != 0) {
            spin_unlock(&b->lock);
            return ERR_BUSY;
        }
        brook->reader_pid = proc->pid;
    }
    /* The reference taken here is the MAPPING's — the one we are on our
     * way to building. Taking it under the same lock that stamps the seat
     * is what stops a peer's concurrent release from freeing the object
     * out from under the rest of this function. */
    brook->ref_count++;

    uint32_t out_fs = brook->frame_size;
    uint32_t out_fc = brook->frame_count;
    uint64_t hdr_phys = brook->header_phys;
    uint64_t slot_total = brook->slot_total_size;
    uint64_t slot_cs    = brook->slot_chunk_size;
    uint32_t slot_cc    = brook->slot_chunk_count;
    spin_unlock(&b->lock);

    /* Reserve VA window. */
    uint64_t va_header = 0, va_slots = 0;
    error_t rc = brook_reserve_user_va(cabin, slot_total, slot_cs,
                                       &va_header, &va_slots);
    if (rc != OK) goto rollback_ref;

    /* Phase 2B M2 — enforce MemTag capabilities BEFORE mapping the
     * Brook's header/slot pages into the cabin. Header phys is the
     * canonical witness — slot regions carry the same tags. */
    if (!MemTagEnforcePhys(proc->pid, (uintptr_t)hdr_phys)) {
        rc = ERR_PERMISSION_DENIED;
        goto rollback_ref;
    }

    /* Map header. */
    if (!brook_map_header_into_cabin(cabin, va_header, hdr_phys)) {
        rc = ERR_NO_MEMORY;
        goto rollback_ref;
    }

    /* Map slot chunks. */
    rc = brook_map_slots_into_cabin(cabin, va_slots, brook->slot_chunks,
                                    slot_cc, slot_cs);
    if (rc != OK) {
        brook_unmap_header_from_cabin(cabin, va_header);
        goto rollback_ref;
    }

    /* Phase 2C — attach bookkeeping for header + each slot chunk so
     * future revoke/grant sweeps can locate Brook's PTEs without scanning
     * the page table. */
    brook_memtag_attach_all(cabin, hdr_phys, va_header,
                             brook->slot_chunks, slot_cc, va_slots, slot_cs);

    /* Allocate claim and link. */
    BrookClaim *claim = (BrookClaim *)kmalloc(sizeof(BrookClaim));
    if (!claim) {
        brook_memtag_detach_all(cabin, hdr_phys, va_header,
                                 brook->slot_chunks, slot_cc, va_slots, slot_cs);
        brook_unmap_slots_from_cabin(cabin, va_slots, slot_cc, slot_cs);
        brook_unmap_header_from_cabin(cabin, va_header);
        rc = ERR_NO_MEMORY;
        goto rollback_ref;
    }
    memset(claim, 0, sizeof(*claim));
    claim->brook           = brook;
    claim->user_va_header  = va_header;
    claim->user_va_slots   = va_slots;
    claim->role            = role;
    claim->flags           = flags;
    claim->holder          = proc->pid;   /* whose hand is on the seat */

    /* Publish peer-alive bit. CAS expected=0, new=1 — if the surviving
     * peer has already FROZEN this side (single-session EOF/
     * PROCESS_TERMINATED decision), the CAS fails and we reject the
     * attach with ERR_INVALID_STATE. This is the lock-free race
     * elimination: writer's attach-CAS and reader's EOF-CAS contend
     * for the alive flag; exactly one wins, exclusive outcome.
     *
     * ever_attached is set ONLY AFTER alive CAS succeeds — the survivor's
     * check (`alive==0 && ever_attached==1`) only ever observes a true
     * "peer was here and left" state, never a half-published attach. */
    BrookHeader *kh = brook_kernel_header(brook);
    volatile uint32_t *alive_ptr;
    volatile uint32_t *ever_ptr;
    if (role == BROOK_WRITER) {
        alive_ptr = &kh->writer_alive;
        ever_ptr  = &kh->writer_ever_attached;
    } else {
        alive_ptr = &kh->reader_alive;
        ever_ptr  = &kh->reader_ever_attached;
    }
    {
        uint32_t expected = 0u;
        if (!__atomic_compare_exchange_n(alive_ptr, &expected, 1u,
                                         false,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            /* Either FROZEN (peer EOF'd this session — terminal) or 1
             * (shouldn't happen under bucket_lock + pid check). Roll
             * back: detach memtag, unmap, free claim, drop ref. */
            brook_memtag_detach_all(cabin, hdr_phys, va_header,
                                     brook->slot_chunks, slot_cc, va_slots, slot_cs);
            brook_unmap_slots_from_cabin(cabin, va_slots, slot_cc, slot_cs);
            brook_unmap_header_from_cabin(cabin, va_header);
            kfree(claim);
            rc = (expected == BROOK_ALIVE_FROZEN) ? ERR_INVALID_STATE
                                                  : ERR_BUSY;
            goto rollback_ref;
        }
        __atomic_store_n(ever_ptr, 1u, __ATOMIC_RELEASE);
    }

    /* Link claim AFTER alive published, and it cannot fail — see
     * brook_link_claim. The window it used to guard belonged to a drain
     * that no longer happens at strand death. */
    brook_link_claim(cabin, claim);

    atomic_fetch_add_u64(&g_stat_claims, 1);

    *out_user_va_header = va_header;
    *out_user_va_slots  = va_slots;
    *out_frame_size     = out_fs;
    *out_frame_count    = out_fc;
    return OK;

rollback_ref:
    /* Give the seat back and drop the mapping reference in one hold —
     * the only place in the file where that is right, because the claim
     * was never published: no list holds it, so nothing else can be
     * looking at either half. */
    spin_lock(&b->lock);
    if (role == BROOK_WRITER && brook->writer_pid == proc->pid) {
        brook->writer_pid = 0;
    } else if (role == BROOK_READER && brook->reader_pid == proc->pid) {
        brook->reader_pid = 0;
    }
    brook_drop_ref_locked(b, brook);
    return rc;
}

/* ─────────────────────────────────────────────────────────────────────
 * Release — the cabin hands a mapping back, and with it the seat if the
 * caller is the one sitting in it.
 *
 * Order is load-bearing: seat first, mapping-reference last. See
 * brook_role_end.
 * ───────────────────────────────────────────────────────────────────── */
error_t BrookReleaseInternal(struct process_t *proc, uint64_t user_va_header)
{
    if (!proc || !proc->cabin || user_va_header == 0) return ERR_INVALID_ARGUMENT;

    cabin_t *cabin = proc->cabin;

    error_t     why   = OK;
    BrookClaim *claim = brook_unlink_claim_by_va(cabin, user_va_header,
                                                 proc->pid, &why);
    if (!claim) return why;

    BrookObject *brook = claim->brook;
    if (!brook) { kfree(claim); return ERR_INVALID_STATE; }

    /* Vacate the seat FIRST, while this claim's own mapping reference is
     * still holding the BrookObject up, and ring the survivor outside the
     * bucket lock. A vacated claim (holder == 0 — its strand already died)
     * skips straight past: there is no seat left to give up. */
    uint32_t ring_pid = brook_role_end(brook, claim->role, claim->holder);
    brook_ring_pid(ring_pid);

    /* Phase 2C — detach memtag attachments before the PTEs go away. */
    brook_memtag_detach_all(cabin, brook->header_phys, claim->user_va_header,
                             brook->slot_chunks, brook->slot_chunk_count,
                             claim->user_va_slots, brook->slot_chunk_size);

    /* Unmap from this cabin. After this no strand of THIS cabin still has
     * a valid PTE pointing into the Brook — which is the whole weight of
     * an explicit release: the caller speaks for every strand in the flat,
     * so it must be sure none of them is still reading. */
    brook_unmap_slots_from_cabin(cabin, claim->user_va_slots,
                                 brook->slot_chunk_count,
                                 brook->slot_chunk_size);
    brook_unmap_header_from_cabin(cabin, claim->user_va_header);

    BrookBucket *b = &g_brook_buckets[brook_bucket_index(brook->tag_id)];
    spin_lock(&b->lock);
    brook_drop_ref_locked(b, brook);
    /* `brook` may have been freed inside brook_drop_ref_locked when the
     * last mapping went — do NOT touch it past this point. */

    kfree(claim);
    if (g_stat_claims > 0) atomic_fetch_sub_u64(&g_stat_claims, 1);
    atomic_fetch_add_u64(&g_stat_releases, 1);
    return OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * BrookHandOver — the seat moves to a sibling, the room does not.
 *
 * A watch being handed over. `pass` was the obvious word and it is taken:
 * this tree already has a Baton.
 *
 * The three stamps that used to be able to disagree — the claim's holder,
 * the BrookObject's role pid, and the header bell that names the same
 * sleeper — move here as one act. The bell is the subtle one: it is hung
 * by USERSPACE, with the sleeper's own pid, so nothing the kernel does can
 * make the two writes atomic with each other. What it can do is carry the
 * pid across with the seat, which is what closes the gap where a sibling's
 * bell hung under the opener's name.
 * ───────────────────────────────────────────────────────────────────── */

/* Carry a hung bell's pid across with the seat it names, under the bucket
 * lock. The RUNG mark is preserved and the word is never cleared: only the
 * sleeper's own hand clears a bell (see BROOK_BELL_RUNG in brook.h), and a
 * bell already taken must stay taken or the peer would ring twice.
 *
 * A bell hung by anyone else — or not hung at all, which is the common
 * case, since the strand doing the handing is running and therefore not
 * asleep — is left exactly as found. The CAS loop re-reads on failure
 * because the peer may set RUNG underneath us; it terminates because the
 * only other writer is the sleeper taking its own bell down. */
static void brook_bell_carry_locked(BrookHeader *kh, uint32_t role,
                                    uint32_t from_pid, uint32_t to_pid)
{
    if (!kh) return;
    volatile uint32_t *bell = (role == BROOK_WRITER) ? &kh->writer_bell
                                                     : &kh->reader_bell;
    uint32_t v = __atomic_load_n(bell, __ATOMIC_ACQUIRE);
    while (BROOK_BELL_PID(v) == from_pid) {
        uint32_t want = to_pid | (v & BROOK_BELL_RUNG);
        if (__atomic_compare_exchange_n(bell, &v, want, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return;
    }
}

error_t BrookHandOver(struct process_t *from, uint64_t user_va_header,
                      uint32_t to_pid)
{
    if (!from || !from->cabin || user_va_header == 0 || to_pid == 0)
        return ERR_INVALID_ARGUMENT;
    if (to_pid == from->pid)   return ERR_INVALID_ARGUMENT;
    if (from->destroying)      return ERR_INVALID_STATE;

    cabin_t *cabin = from->cabin;

    /* The target must share this cabin, because the seat is bolted to a
     * mapping that exists in this address space and nowhere else. Handing
     * it to a strand elsewhere would seat somebody at a table they cannot
     * reach; building them one is brook_open, with its own VA reservation,
     * page-table work and MemTag gate, and its own way of failing. */
    process_t *target = process_find_ref(to_pid);
    if (!target) return ERR_PROCESS_NOT_FOUND;
    if (target->destroying)    { process_ref_dec(target); return ERR_PROCESS_DESTROYING; }
    if (target->cabin != cabin) { process_ref_dec(target); return ERR_NOT_MAPPED; }

    /* Locate the row and prove the caller is the one in the seat. We do
     * NOT unlink it and we do NOT clear `holder` yet: holder == from->pid
     * is what pins the row (and, through its mapping reference, the
     * BrookObject) across the bucket-lock section below, so the two locks
     * are taken one after the other and never one inside the other. */
    spin_lock(&cabin->brook_lock);
    BrookClaim *claim = (BrookClaim *)cabin->brook_claims_head;
    while (claim && claim->user_va_header != user_va_header)
        claim = claim->cabin_next;
    if (!claim) {
        spin_unlock(&cabin->brook_lock);
        process_ref_dec(target);
        return ERR_TAG_NOT_FOUND;
    }
    if (claim->holder != from->pid) {
        spin_unlock(&cabin->brook_lock);
        process_ref_dec(target);
        return ERR_PERMISSION_DENIED;
    }
    BrookObject *brook = claim->brook;
    uint32_t     role  = claim->role;
    spin_unlock(&cabin->brook_lock);

    if (!brook) { process_ref_dec(target); return ERR_INVALID_STATE; }

    BrookBucket *b = &g_brook_buckets[brook_bucket_index(brook->tag_id)];
    spin_lock(&b->lock);

    /* Compare before stamping, and the compare is an interlock rather
     * than a formality: the seat is only ever written under THIS lock, so
     * reading it here and finding a different name means something vacated
     * it between the two locks — a kill that reached BrookCleanupStrand
     * first — and stamping the target onto it would seat them at a stream
     * whose alive flag has already gone to 0. */
    uint32_t *seat = (role == BROOK_WRITER) ? &brook->writer_pid
                                            : &brook->reader_pid;
    if (*seat != from->pid) {
        spin_unlock(&b->lock);
        process_ref_dec(target);
        return ERR_INVALID_STATE;
    }
    *seat = to_pid;
    brook_bell_carry_locked(brook_kernel_header(brook), role,
                            from->pid, to_pid);

    spin_unlock(&b->lock);

    spin_lock(&cabin->brook_lock);
    claim->holder = to_pid;
    spin_unlock(&cabin->brook_lock);

    process_ref_dec(target);
    return OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * BrookCleanupStrand — the ROLE half of teardown, from process_destroy.
 *
 * It ends seats and nothing else. The mapping each seat sat on stays
 * exactly where it is, in an address space this strand's siblings are
 * still executing in; it goes at cabin teardown, with the cabin that owns
 * it. The code that used to stand here spliced rows out by a back-pointer
 * to the opening strand and cleared PTEs under those siblings' feet.
 *
 * One row per pass, re-scanning from the head each time. The list is
 * single-digit and this runs once per strand death, so the cost is
 * nothing; what it buys is that `holder` is only ever read or written
 * inside a brook_lock section, while the BrookObject work happens inside a
 * bucket-lock section, and neither lock is ever held across the other.
 *
 * The order within a pass is the same one release uses and for the same
 * reason: the row keeps its mapping reference throughout, and `holder` is
 * cleared LAST — until it is, no sibling may release the row out from
 * under the seat we are still vacating.
 * ───────────────────────────────────────────────────────────────────── */
void BrookCleanupStrand(struct process_t *proc)
{
    if (!proc || !proc->cabin) return;
    /* 0 is the vacated marker, never a strand. Scanning for it would match
     * every already-vacated row and clear nothing, and the pass-per-row
     * loop below would not terminate — a hang inside process_destroy. */
    if (proc->pid == 0) return;

    cabin_t *cabin = proc->cabin;

    for (;;) {
        spin_lock(&cabin->brook_lock);
        BrookClaim *claim = (BrookClaim *)cabin->brook_claims_head;
        while (claim && claim->holder != proc->pid) claim = claim->cabin_next;
        if (!claim) {
            spin_unlock(&cabin->brook_lock);
            return;
        }
        BrookObject *brook = claim->brook;
        uint32_t     role  = claim->role;
        spin_unlock(&cabin->brook_lock);

        /* A strand that DIES holding one end is the commonest way a stream
         * ends, and the sleeper on the other end has even less to go on
         * than after an orderly release — nobody called anything. */
        uint32_t ring_pid = brook_role_end(brook, role, proc->pid);
        brook_ring_pid(ring_pid);

        spin_lock(&cabin->brook_lock);
        if (claim->holder == proc->pid) claim->holder = 0;
        spin_unlock(&cabin->brook_lock);
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * BrookCleanupCabin — the MAPPING half, and the only automatic release
 * there is. The cabin's last strand has been reaped and its address space
 * is about to go, so every row still on the list goes with it — the whole
 * list, unconditionally, because every row on it is this cabin's. Nothing
 * can link a new one: linking needs a strand, and there is none left to
 * have called us.
 *
 * It must run BEFORE vmm_destroy_context. That walk frees every mapped
 * user data page as though it were the cabin's alone, and a Brook's ring
 * is shared with whatever cabin holds the other end — the pages would go
 * back to the PMM under a live peer.
 *
 * A seat found still warm means a strand was reaped without
 * BrookCleanupStrand reaching it. Vacate it here rather than free a
 * BrookObject with a live role stamped on it; the row's own mapping
 * reference is still held at that point, which is what makes the object
 * safe to touch.
 * ───────────────────────────────────────────────────────────────────── */
void BrookCleanupCabin(struct cabin_t *cabin)
{
    if (!cabin) return;

    spin_lock(&cabin->brook_lock);
    BrookClaim *to_free = (BrookClaim *)cabin->brook_claims_head;
    cabin->brook_claims_head = NULL;
    spin_unlock(&cabin->brook_lock);

    while (to_free) {
        BrookClaim  *next  = to_free->cabin_next;
        BrookObject *brook = to_free->brook;

        if (brook) {
            uint32_t ring_pid = brook_role_end(brook, to_free->role,
                                               to_free->holder);
            brook_ring_pid(ring_pid);

            brook_memtag_detach_all(cabin, brook->header_phys,
                                     to_free->user_va_header,
                                     brook->slot_chunks, brook->slot_chunk_count,
                                     to_free->user_va_slots,
                                     brook->slot_chunk_size);
            brook_unmap_slots_from_cabin(cabin, to_free->user_va_slots,
                                         brook->slot_chunk_count,
                                         brook->slot_chunk_size);
            brook_unmap_header_from_cabin(cabin, to_free->user_va_header);

            BrookBucket *b = &g_brook_buckets[brook_bucket_index(brook->tag_id)];
            spin_lock(&b->lock);
            brook_drop_ref_locked(b, brook);
            /* `brook` may be freed past this point — don't touch it. */

            if (g_stat_claims > 0) atomic_fetch_sub_u64(&g_stat_claims, 1);
            atomic_fetch_add_u64(&g_stat_releases, 1);
        }

        kfree(to_free);
        to_free = next;
    }
}

/* See brook.h. Walks every bucket because a brook is keyed by tag, not by the
 * strands on its ends — there is no index from a pid to the streams it holds,
 * and building one to serve a report that runs only when the whole machine has
 * gone quiet would be a lock in every open and release to save a walk nobody
 * is waiting on. 256 buckets, single-digit brooks in each.
 *
 * ‼ WHAT A LOST RING ACTUALLY LOOKS LIKE, and it is not what it sounds like.
 * The obvious test — "a bell still hanging over a stream that has work" — is
 * the wrong one, and a staged defect proved it: the peer TAKES the bell (the
 * CAS is what makes exactly one of them pay) and only then rings. Lose the ring
 * and the bell is already down; the stream sits with one frame and a clean
 * header, and that test sees nothing at all. Measured, not reasoned: with the
 * ring disabled the wedged lane read bell=0 head=0 tail=1.
 *
 * So the accusation rests on two facts from the same strand:
 *
 *   IT WENT TO SLEEP EXPECTING A BELL — at least one of its brooks carries its
 *   pid, on the side it holds. Only a strand that hung one is owed a ring,
 *   which is what keeps this away from the perfectly legitimate case of a
 *   strand parked on something else while a stream waits for it to come back.
 *
 *   AND ONE OF THEM COULD HAVE SERVED IT — a reader with frames it has not
 *   read, or a writer with room it has not been told about. Somebody moved a
 *   cursor for a strand that is asleep.
 *
 * Both ends are checked, because both ends sleep: a reader waiting for a frame
 * and a writer waiting for a slot are the same defect wearing different hats,
 * and the writer's is the console jam that used to be visible only as a line
 * of print sixty seconds later. */
bool BrookBellUnrung(uint32_t strand_pid, uint16_t *out_tag_id,
                     uint64_t *out_head, uint64_t *out_tail)
{
    if (strand_pid == 0) return false;

    bool     hung   = false;
    bool     served = false;
    uint16_t tag    = 0;
    uint64_t shead = 0, stail = 0;

    for (uint32_t i = 0; i < BROOK_BUCKETS; i++) {
        BrookBucket *b = &g_brook_buckets[i];
        spin_lock(&b->lock);
        for (BrookObject *o = b->head; o; o = o->bucket_next) {
            if (!o->header_phys) continue;
            bool is_reader = (o->reader_pid == strand_pid);
            bool is_writer = (o->writer_pid == strand_pid);
            if (!is_reader && !is_writer) continue;

            const BrookHeader *h = brook_kernel_header(o);
            if (!h) continue;

            /* Asleep in a stream — durable now that taking a bell only marks
             * it. This is the fact that separates a strand owed a ring from
             * one merely parked elsewhere while a stream waits for it. */
            uint32_t rb = __atomic_load_n(&h->reader_bell, __ATOMIC_ACQUIRE);
            uint32_t wb = __atomic_load_n(&h->writer_bell, __ATOMIC_ACQUIRE);
            if (is_reader && rb != 0) hung = true;
            if (is_writer && wb != 0) hung = true;

            /* head BEFORE tail — same rule as the Result ring: read the
             * consumer cursor first and the producer cursor cannot be sampled
             * behind it, so an empty stream can never look full. */
            uint64_t head = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
            uint64_t tail = __atomic_load_n(&h->tail, __ATOMIC_ACQUIRE);
            bool     can  = is_reader ? (head != tail)
                                      : ((tail - head) < o->frame_count);
            if (!served && can) {
                served = true;
                tag    = o->tag_id;
                shead  = head;
                stail  = tail;
            }
        }
        spin_unlock(&b->lock);
        if (hung && served) break;
    }

    if (!hung || !served) return false;
    if (out_tag_id) *out_tag_id = tag;
    if (out_head)   *out_head   = shead;
    if (out_tail)   *out_tail   = stail;
    return true;
}

/* ─────────────────────────────────────────────────────────────────────
 * BrookRingPeer — the kernel rings for a peer that cannot ring for itself.
 *
 * A strand asleep on a stream is woken by the other end moving a cursor. A
 * strand asleep on a stream whose other end has DEPARTED is woken by nothing
 * at all: the departure is a store to `*_alive` made by a kernel that has no
 * cursor to move, and the sleeper's last look happened before it. It would
 * wait for a frame that can never come, or a slot that will never be freed.
 *
 * So the kernel does exactly what the departing peer would have done if it
 * still could: take the survivor's bell and ring it. Split in two on purpose —
 * TAKE runs under the bucket lock that guards the header, RING does not,
 * because ringing walks the target's page tables (KResultPush) and that is not
 * work to do with a bucket lock held.
 * ───────────────────────────────────────────────────────────────────── */

/* Under the bucket lock: take the SURVIVOR's bell. `departing_role` is the
 * side that is leaving, so the bell taken is the other one. Returns the pid to
 * ring, or 0 if nobody was asleep. */
static uint32_t brook_take_survivor_bell_locked(BrookHeader *kh, uint32_t departing_role)
{
    if (!kh) return 0;
    volatile uint32_t *bell = (departing_role == BROOK_WRITER) ? &kh->reader_bell
                                                               : &kh->writer_bell;
    uint32_t who = __atomic_load_n(bell, __ATOMIC_ACQUIRE);
    if (who == 0) return 0;
    if (who & BROOK_BELL_RUNG) return 0;        /* already rung for this sleep */
    /* Mark, never erase — the sleeper's own hand is the only one that clears
     * this word, so the fact that it IS asleep in a stream survives the ring.
     * See BROOK_BELL_RUNG in brook.h. */
    if (!__atomic_compare_exchange_n(bell, &who, who | BROOK_BELL_RUNG, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return 0;                       /* the peer took it first and rang */
    return BROOK_BELL_PID(who);
}

/* Outside every lock: deliver the ring.
 *
 * Exactly the record system.bell pushes — no sender, no token,
 * ERR_WOULD_BLOCK: the shape every consumer in boxlib has discarded since the
 * async-park ack existed. Its whole job is to move the target's Result cursor,
 * which is what its sleep is watching, and wearing a shape nobody can mistake
 * for a message is what keeps this out of anybody's mailbox.
 * Best-effort: a target already gone needs no telling. */
static void brook_ring_pid(uint32_t who)
{
    if (who == 0) return;
    process_t *target = process_find_ref(who);
    if (!target) return;

    Result r;
    memset(&r, 0, sizeof(r));
    r.error_code = ERR_WOULD_BLOCK;
    (void)KResultPush(target, &r);
    process_ref_dec(target);
}

void BrookStatsSnapshot(uint64_t out[4])
{
    out[0] = atomic_load_u64(&g_stat_objects);
    out[1] = atomic_load_u64(&g_stat_claims);
    out[2] = atomic_load_u64(&g_stat_pages);
    out[3] = atomic_load_u64(&g_stat_releases);
}
