/*
 * MCE Page Migration — production-grade hot-page rescue
 *
 * Phase 2F shipped #MC bank decode + per-page poisoning + Touch publish.
 * What was missing: if a poisoned phys page was already mapped into one
 * or more live cabins, those cabins would silently keep using stale
 * translations until the affected line was actually touched, at which
 * point the process would die from the next UC error or — worse — read
 * garbled data on a re-fault that races with PMM recycling.
 *
 * mce_migrate closes that gap. After Phase 2F poisons the phys page and
 * publishes `mce:fault:detected`, the #MC handler queues a migration
 * request. A K-Core worker, running in normal kernel context, then:
 *
 *   1. Resolves the poisoned phys → owning MemRegion (Phase 2D O(1)
 *      `id_by_page` reverse-map).
 *   2. Snapshots the per-region attach chain (cabin owners, va_base,
 *      orig_flags) under per-region bucket lock.
 *   3. Allocates ONE replacement phys page (single alloc — Bay-shared
 *      regions stay shared by construction).
 *   4. Safe-copies the old phys to the new phys with nested-#MC
 *      detection: a per-CPU "in-migration" flag is set so a nested #MC
 *      hitting the same phys can abort the copy mid-stream, after which
 *      remaining bytes are zero-filled (better partial-zero than process
 *      kill).
 *   5. For every (ctx, va) attach: atomic CAS on the leaf PTE swaps
 *      PHYS bits (preserving low-12 flags + NX + Phase 2D region_id +
 *      Phase 2H PKEY + Phase 2K CET) and runs a per-context TLB
 *      shootdown via the existing vmm_shootdown_pages path.
 *   6. Publishes `mce:migration:completed` / `failed` / `unmapped` with a
 *      64-byte payload so userspace observers can record the event.
 *
 * Lost-write window
 * -----------------
 * Between the PTE CAS and the shootdown ACK from every CPU, a CPU still
 * caching the old translation may write the OLD phys. Those writes are
 * lost once the TLB flush completes. This is bounded (~10 µs IPI
 * round-trip) and considered acceptable because the page was already
 * marked poisoned — the application was scheduled to die anyway. The
 * alternative (PTE.P=0 + shootdown + copy + restore-PTE with #PF
 * coordination) trades that 10 µs window for #PF-handler complexity
 * and a synchronization point with the scheduler, and is not justified
 * for the rare-event MCE path.
 *
 * Nested #MC during safe-copy
 * ---------------------------
 * If we're copying through the bad line (the one that fired the
 * original #MC), the read may re-trigger a #MC on the same CPU. The
 * inner handler observes g_in_migration[cpu] == old_phys, sets
 * g_migration_aborted[cpu] = true, and returns. The outer copy loop
 * checks the abort flag after each 64-byte chunk and zero-fills the
 * remainder. Result: process keeps running with partially-zeroed data
 * instead of dying. The Touch payload's `copy_clean=0` informs the
 * subscriber that this happened.
 *
 * Concurrency
 * -----------
 * The request ring is a static 32-slot CAS-claim pool (no allocation,
 * IRQ-safe producer). A claimed slot is handed to irq_defer() — the
 * existing MPMC bottom-half pipeline. Slots are released as soon as the
 * worker has copied the parameters into its stack, so back-pressure is
 * minimal even when several #MC events fire in quick succession.
 *
 * Hardware references
 * -------------------
 * - Intel SDM Vol 3B §15.6 — Software Recovery from Uncorrectable
 *   Errors (SRAR vs SRAO; per-cacheline isolation hints).
 * - Intel SDM Vol 3A §4.10 — TLB shootdown semantics.
 * - AMD APM Vol 2 §9 — Machine Check Architecture.
 */

#ifndef MCE_MIGRATE_H
#define MCE_MIGRATE_H

#include "ktypes.h"
#include "mce.h"

/* Public API ─────────────────────────────────────────────────────── */

/* Initialize the migration subsystem. Resolves Touch tag handles +
 * zeroes the per-CPU nesting state. Must be called AFTER:
 *   - MemTagInit() — needs the MemRegion registry alive
 *   - irq_defer_init() — uses irq_defer() to defer to K-Core
 *   - guide_init() / MemTagEnableTouchPublish() — needs Touch usable
 * Idempotent: second call is a no-op. Safe to skip if mce_init refused
 * to bring up (CPUID lacked MCA). */
void mce_migrate_init(void);

bool mce_migrate_is_initialized(void);

/* Request migration of `phys` (page-aligned) on behalf of an MCE event.
 * Called from inside #MC handler context (IST stack) — must remain
 * IRQ-safe (no kmalloc, no spinlock that could be held by a non-IRQ
 * caller). Implementation reserves a slot from a static ring and hands
 * the (phys, sev, status) tuple to irq_defer() for K-Core execution.
 *
 * Returns true if the request was queued, false if the ring was full
 * (back-pressure → drop, counted in stats). A dropped request is
 * non-fatal: Phase 2F already marked the page poisoned, so future
 * allocations skip it; the affected process will simply #PF on next
 * access and be killed by the standard handler. */
bool mce_migrate_request(uintptr_t phys, mce_severity_t sev, uint64_t status);

/* Nested-#MC notifier. Called by mce_handle() at the very top of the
 * IST handler. If the current CPU has an in-progress migration of the
 * SAME phys reported by this nested #MC, the migration is marked
 * aborted (the copy loop will observe this and zero-fill the
 * remainder). Returns true if the nested fault was consumed by an
 * active migration, false otherwise. */
bool mce_migrate_note_nested(uintptr_t phys);

/* Telemetry ──────────────────────────────────────────────────────── */

typedef struct {
    uint64_t requests;          /* total queued via mce_migrate_request */
    uint64_t drops;             /* ring full → dropped requests */
    uint64_t completed;         /* migrations that swapped ≥ 1 PTE */
    uint64_t failed;            /* migrations that couldn't allocate / no PTE */
    uint64_t no_owner;          /* phys not covered by any MemRegion */
    uint64_t nested_aborts;     /* nested #MC observed during copy */
    uint64_t cabins_touched;    /* sum of (ctx, va) attaches actually swapped */
    uint64_t pages_migrated;    /* always 1 per request in Phase 1 */
    uint64_t skipped_2m;        /* 2 MiB attaches skipped (Phase 1 = 4K only) */
} mce_migrate_stats_t;

void mce_migrate_get_stats(mce_migrate_stats_t *out);
void mce_migrate_dump(void);

/* Test surface ───────────────────────────────────────────────────── */

/* Run synchronously (NO defer) so a test can observe the side effects
 * immediately. Same code path as the deferred worker. Used by
 * mce_migrate_test.c. Returns the number of cabins whose PTE was
 * swapped (>= 0). */
uint32_t mce_migrate_run_sync(uintptr_t phys);

#endif /* MCE_MIGRATE_H */
