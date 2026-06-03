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
 *   - peer-death wake on process_destroy
 */

#include "brook.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "klib.h"
#include "tagfs.h"
#include "tag_registry.h"
#include "atomics.h"
#include "error.h"
#include "kernel_config.h"
#include "acpi.h"          /* acpi_get_numa, ACPI_NUMA_DOMAIN_UNKNOWN */
#include "amp.h"           /* amp_get_core_index, g_amp.cores */
#include "cpuid.h"         /* g_cpu_caps for WAITPKG + monitor line size */

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
    if (!tag || tag[0] == '\0') return TAGFS_INVALID_TAG_ID;
    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->registry) return TAGFS_INVALID_TAG_ID;

    char key[256], value[256];
    tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value));
    const char *v = (value[0] != '\0') ? value : NULL;

    uint16_t id = tag_registry_lookup(fs->registry, key, v);
    if (id != TAGFS_INVALID_TAG_ID) return id;
    if (!intern_if_missing) return TAGFS_INVALID_TAG_ID;
    return tag_registry_intern(fs->registry, key, v);
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

/* Map slot chunks into proc->cabin at user_va_base. Unwinds on failure. */
static error_t brook_map_slots_into_cabin(struct process_t *proc,
                                          uint64_t user_va_base,
                                          uint64_t *chunks,
                                          uint32_t chunk_count,
                                          uint64_t chunk_size)
{
    if (!proc || !proc->cabin) return ERR_INVALID_ARGUMENT;

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
            ok = vmm_map_huge_2m(proc->cabin, va, pa, vmm_flags);
        } else {
            vmm_map_result_t r = vmm_map_page(proc->cabin, va, pa, vmm_flags);
            ok = r.success;
        }

        if (!ok) {
            for (uint32_t j = 0; j < i; j++) {
                uint64_t uva = user_va_base + (uint64_t)j * chunk_size;
                if (chunk_size == BROOK_HUGE_SIZE) vmm_unmap_huge_2m(proc->cabin, uva);
                else                               vmm_unmap_page(proc->cabin, uva);
            }
            return ERR_NO_MEMORY;
        }
    }
    return OK;
}

static void brook_unmap_slots_from_cabin(struct process_t *proc,
                                         uint64_t user_va_base,
                                         uint32_t chunk_count,
                                         uint64_t chunk_size)
{
    if (!proc || !proc->cabin) return;
    for (uint32_t i = 0; i < chunk_count; i++) {
        uint64_t uva = user_va_base + (uint64_t)i * chunk_size;
        if (chunk_size == BROOK_HUGE_SIZE) vmm_unmap_huge_2m(proc->cabin, uva);
        else                               vmm_unmap_page(proc->cabin, uva);
    }
}

/* Map / unmap the 4 KiB BrookHeader page. */
static bool brook_map_header_into_cabin(struct process_t *proc,
                                        uint64_t user_va_header,
                                        uint64_t header_phys)
{
    if (!proc || !proc->cabin) return false;
    const uint64_t vmm_flags = VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE;
    vmm_map_result_t r = vmm_map_page(proc->cabin, user_va_header,
                                      header_phys, vmm_flags);
    return r.success;
}

static void brook_unmap_header_from_cabin(struct process_t *proc,
                                          uint64_t user_va_header)
{
    if (!proc || !proc->cabin) return;
    vmm_unmap_page(proc->cabin, user_va_header);
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
static error_t brook_reserve_user_va(struct process_t *proc,
                                     uint64_t slot_total_size,
                                     uint64_t slot_chunk_size,
                                     uint64_t *out_va_header,
                                     uint64_t *out_va_slots)
{
    if (!proc) return ERR_INVALID_ARGUMENT;
    if (slot_chunk_size < PMM_PAGE_SIZE) slot_chunk_size = PMM_PAGE_SIZE;

    spin_lock(&proc->brook_lock);

    /* Page-align cursor for the header. */
    uint64_t cur = (proc->brook_va_next + (PMM_PAGE_SIZE - 1))
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
        spin_unlock(&proc->brook_lock);
        return ERR_NO_MEMORY;
    }

    proc->brook_va_next = va_slots + slot_total_size;

    spin_unlock(&proc->brook_lock);

    *out_va_header = va_header;
    *out_va_slots  = va_slots;
    return OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * Claim list helpers — proc->brook_claims_head singly-linked.
 * Identical shape to Bay's claim list.
 * ───────────────────────────────────────────────────────────────────── */
static bool brook_link_claim(struct process_t *proc, BrookClaim *claim)
{
    spin_lock(&proc->brook_lock);
    if (proc->destroying) {
        spin_unlock(&proc->brook_lock);
        return false;
    }
    claim->proc_next = (BrookClaim *)proc->brook_claims_head;
    proc->brook_claims_head = claim;
    spin_unlock(&proc->brook_lock);
    return true;
}

static BrookClaim *brook_unlink_claim_by_va(struct process_t *proc,
                                            uint64_t user_va_header)
{
    spin_lock(&proc->brook_lock);
    BrookClaim **p = (BrookClaim **)&proc->brook_claims_head;
    while (*p) {
        if ((*p)->user_va_header == user_va_header) {
            BrookClaim *hit = *p;
            *p = hit->proc_next;
            spin_unlock(&proc->brook_lock);
            return hit;
        }
        p = &(*p)->proc_next;
    }
    spin_unlock(&proc->brook_lock);
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

/* Drop one ref. Caller MUST hold b->lock on entry; releases the lock
 * before returning. If ref_count hits zero, BrookObject is unlinked,
 * backing pages return to PMM, and the struct is freed. */
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
        fresh->create_pid       = proc->pid;
        fresh->create_tsc       = rdtsc();

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
    error_t rc = brook_reserve_user_va(proc, slot_total, slot_cs,
                                       &va_header, &va_slots);
    if (rc != OK) goto rollback_ref;

    /* Map header. */
    if (!brook_map_header_into_cabin(proc, va_header, hdr_phys)) {
        rc = ERR_NO_MEMORY;
        goto rollback_ref;
    }

    /* Map slot chunks. */
    rc = brook_map_slots_into_cabin(proc, va_slots, brook->slot_chunks,
                                    slot_cc, slot_cs);
    if (rc != OK) {
        brook_unmap_header_from_cabin(proc, va_header);
        goto rollback_ref;
    }

    /* Allocate claim and link. */
    BrookClaim *claim = (BrookClaim *)kmalloc(sizeof(BrookClaim));
    if (!claim) {
        brook_unmap_slots_from_cabin(proc, va_slots, slot_cc, slot_cs);
        brook_unmap_header_from_cabin(proc, va_header);
        rc = ERR_NO_MEMORY;
        goto rollback_ref;
    }
    memset(claim, 0, sizeof(*claim));
    claim->brook           = brook;
    claim->proc            = proc;
    claim->user_va_header  = va_header;
    claim->user_va_slots   = va_slots;
    claim->role            = role;
    claim->flags           = flags;

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
             * back: unmap, free claim, drop ref. */
            brook_unmap_slots_from_cabin(proc, va_slots, slot_cc, slot_cs);
            brook_unmap_header_from_cabin(proc, va_header);
            kfree(claim);
            rc = (expected == BROOK_ALIVE_FROZEN) ? ERR_INVALID_STATE
                                                  : ERR_BUSY;
            goto rollback_ref;
        }
        __atomic_store_n(ever_ptr, 1u, __ATOMIC_RELEASE);
    }

    /* Link claim AFTER alive published. The destroying-check inside
     * brook_link_claim closes the window where BrookCleanupProcess
     * might have drained the list while we were busy. */
    if (!brook_link_claim(proc, claim)) {
        /* Undo alive flag (CAS 1→0; if reader concurrently FROZEN'd,
         * leave it FROZEN). */
        uint32_t one = 1u;
        __atomic_compare_exchange_n(alive_ptr, &one, 0u, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        brook_unmap_slots_from_cabin(proc, va_slots, slot_cc, slot_cs);
        brook_unmap_header_from_cabin(proc, va_header);
        kfree(claim);
        rc = ERR_INVALID_STATE;
        goto rollback_ref;
    }

    atomic_fetch_add_u64(&g_stat_claims, 1);

    *out_user_va_header = va_header;
    *out_user_va_slots  = va_slots;
    *out_frame_size     = out_fs;
    *out_frame_count    = out_fc;
    return OK;

rollback_ref:
    /* Drop the role we just claimed + the ref. Same-bucket; re-lock. */
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
 * Release — drop this cabin's claim. If both peers gone, destroy.
 * Wakes the surviving peer (if any) so it sees peer_alive=0 immediately.
 * ───────────────────────────────────────────────────────────────────── */
error_t BrookReleaseInternal(struct process_t *proc, uint64_t user_va_header)
{
    if (!proc || user_va_header == 0) return ERR_INVALID_ARGUMENT;

    BrookClaim *claim = brook_unlink_claim_by_va(proc, user_va_header);
    if (!claim) return ERR_TAG_NOT_FOUND;

    BrookObject *brook = claim->brook;
    if (!brook) { kfree(claim); return ERR_INVALID_STATE; }

    /* Unmap from this cabin first. */
    brook_unmap_slots_from_cabin(proc, claim->user_va_slots,
                                 brook->slot_chunk_count,
                                 brook->slot_chunk_size);
    brook_unmap_header_from_cabin(proc, claim->user_va_header);

    BrookBucket *b = &g_brook_buckets[brook_bucket_index(brook->tag_id)];

    spin_lock(&b->lock);

    /* Mark our side dead in the shared header BEFORE dropping the pid.
     * Use CAS expected=1→0 so a concurrent FROZEN set by the surviving
     * peer (single-session EOF decision) is preserved — alive becomes
     * sticky FROZEN, blocking any future attach to this session. */
    BrookHeader *kh = brook_kernel_header(brook);
    if (claim->role == BROOK_WRITER) {
        uint32_t one = 1u;
        __atomic_compare_exchange_n(&kh->writer_alive, &one, 0u, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        if (brook->writer_pid == proc->pid) brook->writer_pid = 0;
    } else {
        uint32_t one = 1u;
        __atomic_compare_exchange_n(&kh->reader_alive, &one, 0u, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        if (brook->reader_pid == proc->pid) brook->reader_pid = 0;
    }

    brook_drop_ref_locked(b, brook);
    /* `brook` may have been freed inside brook_drop_ref_locked when
     * ref_count hit zero — do NOT touch it past this point. */

    kfree(claim);
    if (g_stat_claims > 0) atomic_fetch_sub_u64(&g_stat_claims, 1);
    atomic_fetch_add_u64(&g_stat_releases, 1);
    return OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * BrookCleanupProcess — process_destroy hook. Drains the per-cabin
 * claim list, drops every role, wakes peers with peer-alive=0.
 * ───────────────────────────────────────────────────────────────────── */
void BrookCleanupProcess(struct process_t *proc)
{
    if (!proc) return;

    /* Detach the claim list under brook_lock so concurrent open paths
     * (which check proc->destroying first) cannot race-link a new
     * claim after we walk past it. */
    spin_lock(&proc->brook_lock);
    BrookClaim *head = (BrookClaim *)proc->brook_claims_head;
    proc->brook_claims_head = NULL;
    spin_unlock(&proc->brook_lock);

    while (head) {
        BrookClaim *next = head->proc_next;
        BrookObject *brook = head->brook;

        if (brook && proc->cabin) {
            brook_unmap_slots_from_cabin(proc, head->user_va_slots,
                                         brook->slot_chunk_count,
                                         brook->slot_chunk_size);
            brook_unmap_header_from_cabin(proc, head->user_va_header);

            BrookBucket *b = &g_brook_buckets[brook_bucket_index(brook->tag_id)];
            spin_lock(&b->lock);

            BrookHeader *kh = brook_kernel_header(brook);
            if (head->role == BROOK_WRITER) {
                uint32_t one = 1u;
                __atomic_compare_exchange_n(&kh->writer_alive, &one, 0u,
                                            false, __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED);
                if (brook->writer_pid == proc->pid) brook->writer_pid = 0;
            } else {
                uint32_t one = 1u;
                __atomic_compare_exchange_n(&kh->reader_alive, &one, 0u,
                                            false, __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED);
                if (brook->reader_pid == proc->pid) brook->reader_pid = 0;
            }

            brook_drop_ref_locked(b, brook);
            /* `brook` may be freed past this point — don't touch it. */

            if (g_stat_claims > 0) atomic_fetch_sub_u64(&g_stat_claims, 1);
            atomic_fetch_add_u64(&g_stat_releases, 1);
        }

        kfree(head);
        head = next;
    }
}

void BrookStatsSnapshot(uint64_t out[4])
{
    out[0] = atomic_load_u64(&g_stat_objects);
    out[1] = atomic_load_u64(&g_stat_claims);
    out[2] = atomic_load_u64(&g_stat_pages);
    out[3] = atomic_load_u64(&g_stat_releases);
}
