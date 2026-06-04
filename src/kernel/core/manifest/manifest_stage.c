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

/* ---- Private layout ---------------------------------------------------- */

struct ManifestStage {
    /* Scratch slab — preallocated at boot, lives forever. */
    uint8_t  *scratch;
    uint32_t  scratch_capacity;
    bool      scratch_in_use;
    uint8_t   kcore_id;
    uint8_t   _pad[2];

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
     * Per-core kmalloc — autoscale caps per_core at DEFAULT (64 KiB), so
     * the sequential alloc storm during init is bounded by
     *   per_core × MAX_CORES = 64 KiB × MAX_CORES (256) = 16 MiB worst case,
     * and in practice 16 cores × 64 KiB = 1 MiB. kmalloc is sized for
     * this regime on every supported board.
     *
     * Bulk single-pmm_alloc was considered and rejected: PMM_TAG_KERNEL is
     * a tight zone that fails at boot, and tag=0 buddy_alloc returns
     * memory the post-alloc MAXPHYADDR check rejects in some QEMU/HPET
     * configs. kmalloc routes through the same buddy but with kernel-
     * heap-resident metadata that keeps the addresses stable.
     */
    for (uint32_t i = 0; i < ncores; i++) {
        struct ManifestStage *st = &g_stages[i];
        memset(st, 0, sizeof(*st));
        st->scratch = (uint8_t *)kmalloc(per_core);
        if (!st->scratch) {
            kprintf("[ManifestStage] FATAL: core %u kmalloc(%u B) failed "
                    "(autoscale=%u B, ncores=%u, RAM=%lu MiB)\n",
                    i, per_core, per_core, ncores,
                    (unsigned long)(pmm_get_total_ram_bytes() / (1024ull * 1024ull)));
            /* Free what we already grabbed — we panic right after but the
             * cleanup keeps the invariant clean for unit-test reuse. */
            for (uint32_t j = 0; j < i; j++) {
                if (g_stages[j].scratch) kfree(g_stages[j].scratch);
                g_stages[j].scratch = NULL;
            }
            return ERR_NO_MEMORY;
        }
        memset(st->scratch, 0, per_core);
        st->scratch_capacity = per_core;
        st->scratch_in_use   = false;
        st->kcore_id         = (uint8_t)i;
    }

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
