#ifndef MANIFEST_STAGE_H
#define MANIFEST_STAGE_H

#include "ktypes.h"
#include "error.h"

/*
 * ManifestStage — per-K-Core staging surface for incoming Manifest+Crate
 * payloads before dispatch.
 *
 *   Why this exists
 *   ───────────────
 *   vmm_translate_user_addr returns a kernel pointer valid for only ONE
 *   physical page (cross-page translations silently truncate `size` to the
 *   page-tail in vmm.c). Manifest payloads larger than 4 KiB, or Crate[]
 *   arrays wider than ~102 entries, would either truncate or — if the
 *   downstream code dereferenced past the first page — read random kernel
 *   memory mapped at the adjacent kernel VA. ManifestStage stages the
 *   entire payload into a guaranteed-contiguous kernel buffer via a
 *   page-walked copy, so the executor works on safe contiguous memory.
 *
 *   Two-tier dispatch (chosen at acquire time per payload size)
 *   ───────────────────────────────────────────────────────────
 *     TIER_SCRATCH  Per-K-Core preallocated scratch (default 64 KiB,
 *                   auto-scaled to RAM/cores at boot, NUMA-local on
 *                   multi-domain hosts). Zero per-syscall allocation.
 *                   Hot path: covers chain (~13 KiB), decks (~16 KiB),
 *                   MfCall1 (≤ 1 KiB).
 *     TIER_KMALLOC  Fallback for payloads that don't fit scratch but stay
 *                   under MANIFEST_RAW_MAX_SIZE. One kmalloc + one kfree
 *                   per syscall. Cold path.
 *
 *   Beyond MANIFEST_RAW_MAX_SIZE (1 MiB) — rejected with
 *   ERR_INVALID_ARGUMENT. Larger payloads should use Brook streaming
 *   instead of growing the cap.
 *
 *   Concurrency model
 *   ─────────────────
 *   Each K-Core owns its own ManifestStage; no cross-core sharing. The
 *   per-K-Core dispatch loop (kcore_run_loop → guide → ManifestExecuteOnce)
 *   is sequential by construction. Scratch is single-occupancy on a
 *   K-Core, guarded by a CAS on `scratch_in_use` — any nested dispatch on
 *   the same K-Core panics in DEBUG (KASSERT) and silently falls through
 *   to TIER_KMALLOC in release (defense in depth). Counter writes use
 *   __ATOMIC_RELAXED — stats, not synchronization.
 *
 *   Lifetime
 *   ────────
 *   Boot-once via ManifestStageInitAll() after amp_init has populated
 *   g_amp.total_cores. K-Cores are pinned to APs and never destroyed in
 *   production, so scratch is permanent.
 */

#define MANIFEST_STAGE_TIER_SCRATCH  0u
#define MANIFEST_STAGE_TIER_KMALLOC  1u
#define MANIFEST_STAGE_TIER_INVALID  0xFFu  /* poisoned grant (released or pre-acquire) */

/*
 * Auto-scaling bounds for per-K-Core scratch size at boot.
 *
 *   DEFAULT  64 KiB  — cap for the per-core scratch. Covers the largest
 *                       realistic single-syscall Manifest (chain test =
 *                       13 KiB, decks ≈ 16 KiB, MfCall1 ≤ 1 KiB) with
 *                       4×-headroom. Going BEYOND this just burns
 *                       bulk-allocation budget that scales with core
 *                       count without speeding any workload — payloads
 *                       larger than 64 KiB belong on TIER_KMALLOC.
 *   FLOOR    16 KiB  — minimum acceptable scratch. On tiny embedded
 *                       boxes (< 64 MB RAM × many cores) the 0.1 %
 *                       per-core budget can drop below default; floor
 *                       prevents the scratch from shrinking past the
 *                       realistic op-stream size and forcing every
 *                       dispatch onto TIER_KMALLOC.
 */
#define MANIFEST_STAGE_SCRATCH_DEFAULT_SIZE  (64u * 1024u)
#define MANIFEST_STAGE_SCRATCH_FLOOR_SIZE    (16u * 1024u)

/*
 * ManifestStageGrant — RAII-like ticket returned by ManifestStageAcquire
 * and consumed by ManifestStageRelease. Holds the dispatch-side knowledge
 * of *where the staged buffer lives* (scratch vs kmalloc) so release knows
 * what to clean up.
 */
typedef struct ManifestStageGrant {
    void    *kbuf;     /* contiguous kernel VA, valid for `bytes` bytes */
    uint32_t bytes;    /* exact bytes granted (== requested) */
    uint8_t  tier;     /* MANIFEST_STAGE_TIER_* */
    uint8_t  _pad[3];
} ManifestStageGrant;

_Static_assert(sizeof(ManifestStageGrant) == 16,
               "ManifestStageGrant must stay 16 bytes (half cacheline)");

/*
 * ManifestStageStats — diagnostic snapshot of per-K-Core acquire/reject
 * counts. Surfaced via system.perf.dump alongside KResultPushStats etc.
 */
typedef struct ManifestStageStats {
    uint64_t scratch_acquires;     /* count: TIER_SCRATCH grants */
    uint64_t kmalloc_acquires;     /* count: TIER_KMALLOC grants */
    uint64_t reentry_fallbacks;    /* count: scratch CAS failed → kmalloc */
    uint64_t reject_too_large;     /* count: bytes > MANIFEST_RAW_MAX_SIZE */
    uint64_t reject_alloc_failed;  /* count: kmalloc returned NULL */
    uint32_t max_bytes_seen;       /* high-water mark on bytes requested */
    uint32_t scratch_capacity;     /* this K-Core's scratch size in bytes */
} ManifestStageStats;

/* Forward declaration — full layout is private to manifest_stage.c. */
typedef struct ManifestStage ManifestStage;

/*
 * ManifestStageInitAll — allocate per-K-Core scratch arenas for every
 * core listed in g_amp.cores[0..total_cores). Idempotent — second call
 * is a no-op. MUST run after amp_init (needs g_amp.total_cores).
 *
 * On NUMA boxes, each K-Core's scratch is allocated from its local
 * domain when domain-aware PMM is available (preserves cache locality
 * of the hot dispatch path).
 */
error_t ManifestStageInitAll(void);

/*
 * ManifestStageCurrent — return THIS K-Core's stage. Resolves via the
 * per-CPU GS pointer (amp_get_core_index). Safe from any non-IRQ kernel
 * context; never blocks.
 *
 * Returns NULL if the stage subsystem hasn't been initialized yet OR
 * the caller is on an unknown K-Core (cannot happen post-amp_init).
 */
ManifestStage *ManifestStageCurrent(void);

/*
 * ManifestStageAcquire — reserve a contiguous staged region of at least
 * `bytes` bytes. On success fills *out and returns OK; out->kbuf is
 * valid until the matching ManifestStageRelease.
 *
 * Errors:
 *   ERR_NULL_POINTER     `st` or `out` is NULL
 *   ERR_INVALID_ARGUMENT `bytes == 0` or `bytes > MANIFEST_RAW_MAX_SIZE`
 *   ERR_NO_MEMORY        kmalloc fallback exhausted
 */
error_t ManifestStageAcquire(ManifestStage *st, uint32_t bytes,
                              ManifestStageGrant *out);

/*
 * ManifestStageRelease — return the grant. For TIER_SCRATCH this clears
 * the in_use flag; for TIER_KMALLOC this kfree's. Idempotent against
 * release-of-poisoned-grant (a previously released grant has tier set
 * to TIER_INVALID and this call no-ops). Double-release of an active
 * scratch grant is a programming error and panics in DEBUG via KASSERT.
 */
void    ManifestStageRelease(ManifestStage *st, ManifestStageGrant *grant);

/*
 * ManifestStageStatsGet — snapshot of per-K-Core counters. kcore_id
 * must be < g_amp.total_cores. Reads use __ATOMIC_RELAXED so a busy
 * K-Core's counters may lag by a few increments — acceptable for
 * diagnostic dumps. Returns ERR_INVALID_ARGUMENT if kcore_id is out
 * of range, ERR_NOT_INITIALIZED if ManifestStageInitAll hasn't run.
 */
error_t ManifestStageStatsGet(uint8_t kcore_id, ManifestStageStats *out);

/*
 * ManifestStageDumpAll — kprintf a one-line-per-K-Core table. Wired
 * into system.perf.dump (SysPerfDump op handler) alongside the other
 * diagnostic dumps.
 */
void    ManifestStageDumpAll(void);

#endif /* MANIFEST_STAGE_H */
