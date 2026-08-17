/*
 * ManifestStage — implementation.
 *
 * See manifest_stage.h for the architectural rationale and tier semantics.
 *
 * This file owns:
 *   - the private layout of struct ManifestStage
 *   - the static array g_stages[MAX_CORES] (one slot per K-Core)
 *   - the boot-time scratch sizing helper (autoscale by RAM/cores)
 *   - the acquire/release tier dispatch
 *   - the kprintf diagnostic dump
 */

#include "manifest_stage.h"
#include "manifest.h"      /* MANIFEST_RAW_MAX_SIZE */
#include "ktypes.h"
#include "klib.h"
#include "atomics.h"
#include "amp.h"
#include "pmm.h"
#include "vmm.h"           /* vmm_phys_to_virt for NUMA-direct alloc */
#include "acpi.h"          /* acpi_numa_domain_for_apic */

/* ---- Private layout ---------------------------------------------------- */

struct ManifestStage {
    /* Scratch slab — preallocated at boot, lives forever. */
    uint8_t  *scratch;
    uint32_t  scratch_capacity;
    bool      scratch_in_use;
    uint8_t   kcore_id;
    uint8_t   _pad;
    size_t    scratch_pages;     /* page count, needed for pmm_free on rollback */
    void     *scratch_phys;      /* phys addr (NUMA path only); needed for pmm_free */

    /* Counters. __ATOMIC_RELAXED for read paths is intentional — these are
     * statistics, not synchronisation primitives. Same-core writes need no
     * special ordering since the K-Core invariant serialises producers. */
    uint64_t  c_scratch_acquires;
    uint64_t  c_kmalloc_acquires;
    uint64_t  c_reentry_fallbacks;
    uint64_t  c_reject_too_large;
    uint64_t  c_reject_alloc_failed;
    uint32_t  c_max_bytes_seen;
    uint32_t  _pad2;
};

/* MAX_CORES comes from amp.h. */
static struct ManifestStage g_stages[MAX_CORES];
static bool                 g_stages_initialized;

/* Chosen per-K-Core scratch size, frozen at boot. Each stage holds its
 * own copy too — this global is just for diagnostic dumps. */
static uint32_t g_scratch_per_core;

/*
 * Two assertion flavours:
 *
 *   STAGE_ASSERT       — always-active. Used for load-bearing invariants
 *                        whose violation would silently corrupt kernel
 *                        memory (mismatched grant kbuf, double-release of
 *                        the scratch CAS, unknown tier enum, NULL kmalloc
 *                        kbuf in release). Better to crash than to limp
 *                        forward with corrupted dispatch state.
 *   STAGE_DEBUG_ASSERT — DEBUG-only. Used for soft invariants that have
 *                        a defined fallback path in release builds. The
 *                        re-entry detector belongs here: K-Core dispatch
 *                        is sequential by construction so re-entry MUST
 *                        NOT happen, but if it ever does the kmalloc tier
 *                        below is a safe fallback — better than panicking
 *                        a production kernel for a recoverable race.
 */
#define STAGE_ASSERT(cond, msg) do {                                      \
    if (__builtin_expect(!(cond), 0)) {                                   \
        panic("[ManifestStage] %s (cond '%s' at %s:%d)",                  \
              msg, #cond, __FILE__, __LINE__);                            \
    }                                                                     \
} while (0)

#if defined(CONFIG_DEBUG_ENABLED) && CONFIG_DEBUG_ENABLED
#define STAGE_DEBUG_ASSERT(cond, msg) STAGE_ASSERT(cond, msg)
#else
#define STAGE_DEBUG_ASSERT(cond, msg) ((void)0)
#endif

/* ---- Boot helpers ------------------------------------------------------ */

/* Round v DOWN to the largest power of two no greater than v, clamped to
 * [lo, hi]. Pow-of-two stride keeps the scratch buffer cacheline-aligned
 * naturally (PMM always returns page-aligned allocations; powers of two
 * within ≥ a page are trivially aligned). */
static uint32_t stage_round_pow2_clamp(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) v = hi;
    if (v == 0) return lo;
    if (v & (v - 1)) {
        uint32_t shift = 31u - (uint32_t)__builtin_clz(v);
        v = 1u << shift;
    }
    if (v < lo) return lo;
    return v;
}

/*
 * Pick the per-K-Core scratch size at boot. Budget 0.1 % of total RAM
 * for ALL stages combined, divide by core count, clamp upward to FLOOR
 * (so embedded boxes don't drop below realistic op-stream size) and
 * downward to DEFAULT (so big-RAM boxes don't waste bulk allocation
 * budget on scratches no workload uses).
 *
 * Examples:
 *    64 MB ×  1 core    →  64 KB / 1024 / 1   = 64 KB raw → 64 KB default
 *   256 MB ×  4 cores   → 256 KB / 4          = 64 KB     → 64 KB default
 *   1 GB  × 16 cores    →   1 MB / 16         = 64 KB     → 64 KB default
 *   8 GB  × 16 cores    →   8 MB / 16         = 512 KB raw→ 64 KB (cap)
 *  128 MB × 16 cores    → 128 KB / 16         =  8 KB raw → 16 KB floor
 *
 * Bulk total = per_core × ncores stays bounded:
 *   single-core box:  64 KB
 *   16-core 8 GB box:  1 MB
 *   64-core box (max realistic): 4 MB
 */
static uint32_t stage_choose_scratch_size(uint32_t ncores)
{
    if (ncores == 0) ncores = 1;
    uint64_t total = pmm_get_total_ram_bytes();
    if (total == 0) return MANIFEST_STAGE_SCRATCH_DEFAULT_SIZE;

    /* 0.1% of RAM combined, divided by core count. */
    uint64_t budget = total / 1024ull;
    uint64_t per_core_64 = budget / (uint64_t)ncores;
    uint32_t per_core = per_core_64 > (uint64_t)MANIFEST_STAGE_SCRATCH_DEFAULT_SIZE
                       ? MANIFEST_STAGE_SCRATCH_DEFAULT_SIZE
                       : (uint32_t)per_core_64;
    return stage_round_pow2_clamp(per_core,
                                   MANIFEST_STAGE_SCRATCH_FLOOR_SIZE,
                                   MANIFEST_STAGE_SCRATCH_DEFAULT_SIZE);
}

/*
 * Allocate `pages` contiguous physical frames preferring the K-Core's NUMA
 * domain (looked up by LAPIC ID via SRAT), and return a kernel Pull-Map
 * pointer. NULL only when the PMM has no contiguous block at this order.
 *
 * The domain hint is dropped — not the allocation — when: SRAT is absent
 * (single-socket boards), the LAPIC ID is not in SRAT's CPU table, or the
 * named domain has no free block at this order.
 *
 * The scratch is page-scale, so BOTH tiers take it from the PMM: domain-
 * hinted when SRAT names this core's domain, plain otherwise.
 *
 * The second tier used to be kmalloc, on the stated belief that it "routes
 * through the same buddy but cannot honour a domain hint". It does not: a
 * request of this size skips the slab and lands in the kernel's FIXED
 * small-object pool. And SRAT naming no domain is not the exotic case — it
 * is every UMA machine, so on a 16-core host this quietly carved
 * 16 × 64 KiB = 1 MiB out of a 2 MiB heap and left every other large
 * allocation in the kernel fighting for the remainder. */
static uint8_t *stage_alloc_scratch_numa(uint8_t kcore_id, size_t pages,
                                          void **out_phys,
                                          uint32_t *out_numa_hits,
                                          uint32_t *out_uma_hits)
{
    uint32_t apic_id = g_amp.cores[kcore_id].lapic_id;
    uint32_t domain  = acpi_numa_domain_for_apic(apic_id);

    void *phys      = NULL;
    bool  domain_hit = false;

    if (domain != ACPI_NUMA_DOMAIN_UNKNOWN) {
        phys = pmm_alloc_in_domain(pages, domain);
        domain_hit = (phys != NULL);
    }
    if (!phys)
        phys = pmm_alloc(pages);
    if (!phys)
        return NULL;

    void *kv = vmm_phys_to_virt((uintptr_t)phys);
    if (!kv) {
        pmm_free(phys, pages);
        return NULL;
    }

    if (domain_hit) (*out_numa_hits)++;
    else            (*out_uma_hits)++;
    *out_phys = phys;
    return (uint8_t *)kv;
}

/* Only called on the init-rollback path — per-K-Core scratch never frees
 * during steady-state lifetime. Both allocation tiers are PMM, so there is
 * one deallocator. */
static void stage_free_scratch(struct ManifestStage *st)
{
    if (!st->scratch) return;
    if (st->scratch_phys) pmm_free(st->scratch_phys, st->scratch_pages);
    st->scratch      = NULL;
    st->scratch_phys = NULL;
}

/* ---- Public API -------------------------------------------------------- */

error_t ManifestStageInitAll(void)
{
    if (g_stages_initialized) return OK;

    uint32_t ncores = g_amp.total_cores;
    if (ncores == 0) {
        debug_printf("[ManifestStage] init refused — g_amp.total_cores == 0\n");
        return ERR_NOT_INITIALIZED;
    }
    if (ncores > MAX_CORES) ncores = MAX_CORES;

    uint32_t per_core = stage_choose_scratch_size(ncores);
    g_scratch_per_core = per_core;

    /*
     * Per-core allocation. NUMA-aware on multi-socket boxes:
     *   1. Look up the K-Core's NUMA domain via its LAPIC ID (SRAT-driven).
     *   2. pmm_alloc_in_domain returns contiguous phys frames in that
     *      domain; vmm_phys_to_virt converts to the kernel direct-map
     *      pointer (Pull Map). The buffer is then zeroed.
     *   3. On UMA hosts, SRAT-absent BIOS, or the rare case where the
     *      domain has no free contiguous block of the required order,
     *      fall through to kmalloc (which routes through the same buddy
     *      but cannot honour a domain hint). Either path returns a
     *      contiguous kernel buffer.
     *
     * Autoscale caps per_core at DEFAULT (64 KiB), so the sequential
     * alloc storm during init is bounded:
     *   per_core × MAX_CORES = 64 KiB × MAX_CORES (256) = 16 MiB worst,
     *   in practice 16 cores × 64 KiB = 1 MiB.
     */
    size_t per_core_pages = (per_core + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint32_t numa_hits   = 0;
    uint32_t uma_hits = 0;
    for (uint32_t i = 0; i < ncores; i++) {
        struct ManifestStage *st = &g_stages[i];
        memset(st, 0, sizeof(*st));

        void *phys = NULL;
        st->scratch = stage_alloc_scratch_numa((uint8_t)i, per_core_pages,
                                                &phys, &numa_hits, &uma_hits);
        if (!st->scratch) {
            kprintf("[ManifestStage] FATAL: core %u alloc(%u B) failed "
                    "(autoscale=%u B, ncores=%u, RAM=%lu MiB, "
                    "numa_hits=%u uma_hits=%u)\n",
                    i, per_core, per_core, ncores,
                    (unsigned long)(pmm_get_total_ram_bytes() / (1024ull * 1024ull)),
                    numa_hits, uma_hits);
            for (uint32_t j = 0; j < i; j++) {
                stage_free_scratch(&g_stages[j]);
            }
            return ERR_NO_MEMORY;
        }
        memset(st->scratch, 0, per_core);
        st->scratch_capacity = per_core;
        st->scratch_in_use   = false;
        st->kcore_id         = (uint8_t)i;
        st->scratch_pages    = per_core_pages;
        st->scratch_phys     = phys;
    }
    debug_printf("[ManifestStage] NUMA placement: %u cores domain-local, "
                 "%u cores unhinted\n", numa_hits, uma_hits);

    g_stages_initialized = true;
    debug_printf("[ManifestStage] ready: %u cores × %u B scratch "
                 "(RAM=%lu MiB)\n",
                 ncores, per_core,
                 (unsigned long)(pmm_get_total_ram_bytes() / (1024ull * 1024ull)));
    return OK;
}

ManifestStage *ManifestStageCurrent(void)
{
    if (!g_stages_initialized) return NULL;
    uint8_t core = amp_get_core_index();
    /* core < MAX_CORES always holds for uint8_t given MAX_CORES==256;
     * the meaningful guard is against an out-of-bounds index for the
     * currently-active set published by amp_init. */
    if (core >= g_amp.total_cores) return NULL;
    return &g_stages[core];
}

/* Race-free monotonic max via CAS. RELAXED ordering — same as the other
 * counters; stats accuracy is best-effort. */
static inline void stage_observe_size(struct ManifestStage *st, uint32_t bytes)
{
    uint32_t cur;
    do {
        cur = __atomic_load_n(&st->c_max_bytes_seen, __ATOMIC_RELAXED);
        if (bytes <= cur) return;
    } while (!__atomic_compare_exchange_n(&st->c_max_bytes_seen, &cur, bytes,
                                          false, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

error_t ManifestStageAcquire(ManifestStage *st, uint32_t bytes,
                              ManifestStageGrant *out)
{
    if (!st || !out)       return ERR_NULL_POINTER;
    if (bytes == 0)        return ERR_INVALID_ARGUMENT;

    out->kbuf  = NULL;
    out->bytes = 0;
    out->tier  = MANIFEST_STAGE_TIER_INVALID;

    if (bytes > MANIFEST_RAW_MAX_SIZE) {
        __atomic_add_fetch(&st->c_reject_too_large, 1, __ATOMIC_RELAXED);
        return ERR_INVALID_ARGUMENT;
    }

    /* Tier-1: scratch. CAS on scratch_in_use serialises against re-entry. */
    if (bytes <= st->scratch_capacity) {
        bool expected = false;
        if (__atomic_compare_exchange_n(&st->scratch_in_use, &expected, true,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            __atomic_add_fetch(&st->c_scratch_acquires, 1, __ATOMIC_RELAXED);
            stage_observe_size(st, bytes);
            out->kbuf  = st->scratch;
            out->bytes = bytes;
            out->tier  = MANIFEST_STAGE_TIER_SCRATCH;
            return OK;
        }
        /* Re-entry — the K-Core invariant has been broken. Count the event,
         * panic in DEBUG so we catch the violation at the boundary, and
         * fall through to kmalloc in release so the syscall STILL completes
         * correctly (defense in depth — a kernel panic is worse than a
         * minor heap allocation for a once-in-lifetime race). */
        __atomic_add_fetch(&st->c_reentry_fallbacks, 1, __ATOMIC_RELAXED);
        STAGE_DEBUG_ASSERT(false,
                           "scratch_in_use was true on acquire — nested dispatch on the same K-Core");
    }

    /* Tier-2: kmalloc. Used when bytes > scratch_capacity OR on rare
     * re-entry fallback. The cost (one alloc + one free per syscall) is
     * acceptable for cold-path Manifest sizes; tier-1 carries the hot
     * path. */
    void *kbuf = kmalloc(bytes);
    if (!kbuf) {
        __atomic_add_fetch(&st->c_reject_alloc_failed, 1, __ATOMIC_RELAXED);
        return ERR_NO_MEMORY;
    }
    __atomic_add_fetch(&st->c_kmalloc_acquires, 1, __ATOMIC_RELAXED);
    stage_observe_size(st, bytes);
    out->kbuf  = kbuf;
    out->bytes = bytes;
    out->tier  = MANIFEST_STAGE_TIER_KMALLOC;
    return OK;
}

void ManifestStageRelease(ManifestStage *st, ManifestStageGrant *grant)
{
    if (!st || !grant) return;

    switch (grant->tier) {
    case MANIFEST_STAGE_TIER_SCRATCH: {
        STAGE_ASSERT(grant->kbuf == st->scratch,
                     "scratch grant kbuf mismatch — memory corruption?");
        bool expected = true;
        bool released = __atomic_compare_exchange_n(&st->scratch_in_use,
                                                    &expected, false,
                                                    false, __ATOMIC_ACQ_REL,
                                                    __ATOMIC_RELAXED);
        STAGE_ASSERT(released,
                     "release without prior acquire OR double-release of scratch");
        break;
    }
    case MANIFEST_STAGE_TIER_KMALLOC:
        STAGE_ASSERT(grant->kbuf != NULL,
                     "kmalloc-tier grant carries NULL kbuf");
        kfree(grant->kbuf);
        break;
    case MANIFEST_STAGE_TIER_INVALID:
        /* Already-released or never-acquired grant — no-op is the right
         * behaviour, matches the guide.c fast-fail paths that release a
         * partially-initialised grant on early error returns. */
        return;
    default:
        STAGE_ASSERT(false, "unknown grant tier");
    }

    /* Poison the grant so accidental reuse is caught immediately on the
     * next release (TIER_INVALID branch above) instead of corrupting
     * state silently. */
    grant->kbuf  = NULL;
    grant->bytes = 0;
    grant->tier  = MANIFEST_STAGE_TIER_INVALID;
}

error_t ManifestStageStatsGet(uint8_t kcore_id, ManifestStageStats *out)
{
    if (!out) return ERR_NULL_POINTER;
    if (!g_stages_initialized) return ERR_NOT_INITIALIZED;
    if (kcore_id >= g_amp.total_cores) return ERR_INVALID_ARGUMENT;

    struct ManifestStage *st = &g_stages[kcore_id];
    out->scratch_acquires    = __atomic_load_n(&st->c_scratch_acquires,    __ATOMIC_RELAXED);
    out->kmalloc_acquires    = __atomic_load_n(&st->c_kmalloc_acquires,    __ATOMIC_RELAXED);
    out->reentry_fallbacks   = __atomic_load_n(&st->c_reentry_fallbacks,   __ATOMIC_RELAXED);
    out->reject_too_large    = __atomic_load_n(&st->c_reject_too_large,    __ATOMIC_RELAXED);
    out->reject_alloc_failed = __atomic_load_n(&st->c_reject_alloc_failed, __ATOMIC_RELAXED);
    out->max_bytes_seen      = __atomic_load_n(&st->c_max_bytes_seen,      __ATOMIC_RELAXED);
    out->scratch_capacity    = st->scratch_capacity;
    return OK;
}

void ManifestStageDumpAll(void)
{
    if (!g_stages_initialized) {
        kprintf("[ManifestStage] not initialized\n");
        return;
    }
    kprintf("[ManifestStage] %u cores  scratch=%u B/core  max_raw=%u B\n",
            (unsigned)g_amp.total_cores,
            (unsigned)g_stages[0].scratch_capacity,
            (unsigned)MANIFEST_RAW_MAX_SIZE);
    kprintf("  core | scratch | kmalloc | reentry | reject_big | reject_oom | max_bytes\n");
    for (uint32_t i = 0; i < g_amp.total_cores; i++) {
        ManifestStageStats s;
        if (ManifestStageStatsGet((uint8_t)i, &s) != OK) continue;
        kprintf("  %4u | %7lu | %7lu | %7lu | %10lu | %10lu | %9u\n",
                (unsigned)i,
                (unsigned long)s.scratch_acquires,
                (unsigned long)s.kmalloc_acquires,
                (unsigned long)s.reentry_fallbacks,
                (unsigned long)s.reject_too_large,
                (unsigned long)s.reject_alloc_failed,
                (unsigned)s.max_bytes_seen);
    }
}
