
#include "manifest_stage.h"
#include "manifest.h"
#include "ktypes.h"
#include "klib.h"
#include "atomics.h"
#include "amp.h"
#include "pmm.h"
#include "vmm.h"
#include "acpi.h"


struct ManifestStage {
    uint8_t  *scratch;
    uint32_t  scratch_capacity;
    bool      scratch_in_use;
    uint8_t   kcore_id;
    uint8_t   _pad;
    size_t    scratch_pages;
    void     *scratch_phys;

    uint64_t  c_scratch_acquires;
    uint64_t  c_kmalloc_acquires;
    uint64_t  c_reentry_fallbacks;
    uint64_t  c_reject_too_large;
    uint64_t  c_reject_alloc_failed;
    uint32_t  c_max_bytes_seen;
    uint32_t  _pad2;
};

static struct ManifestStage g_stages[MAX_CORES];
static bool                 g_stages_initialized;

static uint32_t g_scratch_per_core;

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

static uint32_t stage_choose_scratch_size(uint32_t ncores)
{
    if (ncores == 0) ncores = 1;
    uint64_t total = pmm_get_total_ram_bytes();
    if (total == 0) return MANIFEST_STAGE_SCRATCH_DEFAULT_SIZE;

    uint64_t budget = total / 1024ull;
    uint64_t per_core_64 = budget / (uint64_t)ncores;
    uint32_t per_core = per_core_64 > (uint64_t)MANIFEST_STAGE_SCRATCH_DEFAULT_SIZE
                       ? MANIFEST_STAGE_SCRATCH_DEFAULT_SIZE
                       : (uint32_t)per_core_64;
    return stage_round_pow2_clamp(per_core,
                                   MANIFEST_STAGE_SCRATCH_FLOOR_SIZE,
                                   MANIFEST_STAGE_SCRATCH_DEFAULT_SIZE);
}

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

static void stage_free_scratch(struct ManifestStage *st)
{
    if (!st->scratch) return;
    if (st->scratch_phys) pmm_free(st->scratch_phys, st->scratch_pages);
    st->scratch      = NULL;
    st->scratch_phys = NULL;
}


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
    if (core >= g_amp.total_cores) return NULL;
    return &g_stages[core];
}

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
        __atomic_add_fetch(&st->c_reentry_fallbacks, 1, __ATOMIC_RELAXED);
        STAGE_DEBUG_ASSERT(false,
                           "scratch_in_use was true on acquire — nested dispatch on the same K-Core");
    }

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
        return;
    default:
        STAGE_ASSERT(false, "unknown grant tier");
    }

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