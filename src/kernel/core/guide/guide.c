/*
 * Guide — drains each process's PocketRing and dispatches each Pocket
 * through the unified Manifest path.
 *
 * Phase 12 cleanup: the legacy prefix-chain dispatcher is gone. Every Pocket
 * arriving at the kernel either carries the YIELD flag (cooperative tick) or
 * the MANIFEST flag (single-shot Manifest execution). Anything else is a
 * malformed Pocket and is rejected.
 */

#include "guide.h"
#include "execution_deck.h"
#include "touch.h"
#include "bay.h"
#include "brook.h"
#include "process.h"
#include "klib.h"
#include "vmm.h"
#include "pocket_ring.h"
#include "kring.h"
#include "error.h"
#include "perf_trace.h"
#include "amp.h"
#include "boxos_crate.h"
#include "boxos_manifest.h"
#include "manifest.h"
#include "manifest_exec.h"
#include "manifest_stage.h"
#include "op_registry.h"

ReadyQueue g_ready_queue;

void guide_init(void)
{
    debug_printf("[GUIDE] Initializing Guide Dispatcher...\n");
    ready_queue_init(&g_ready_queue);
    TouchInit();
    BayInit();
    BrookInit();
    perf_trace_init();

    /* ManifestStage — per-K-Core scratch arenas for safe multi-page Manifest
     * payload staging. Must run after amp_init (consults g_amp.total_cores)
     * and before guide() ever runs; main.c orders us here, immediately after
     * amp_init and well before kcore boot. */
    error_t stage_rc = ManifestStageInitAll();
    if (stage_rc != OK) {
        panic("[GUIDE] ManifestStageInitAll failed: %s\n", ErrorString(stage_rc));
    }

    debug_printf("[GUIDE] ReadyQueue initialized (intrusive, unbounded)\n");
}

/*
 * Manifest-mode dispatch.
 *
 *   1. Validate envelope sizes against MANIFEST_RAW_MAX_SIZE.
 *   2. Stage the Manifest BYTES into this K-Core's ManifestStage scratch
 *      via vmm_user_buf_in_into — page-by-page walk that handles non-
 *      contiguous physical backing and unaligned start/end. This is the
 *      multi-page safety fix: the legacy vmm_translate_user_addr path
 *      silently truncated cross-page ranges, producing kernel-memory
 *      reads for Manifests > 4 KiB. Manifests are READ-ONLY in handlers
 *      (const ManifestOp *), and no handler captures op→params pointers
 *      into async state — staging is safe to release at dispatch exit.
 *   3. Translate the Crate[] array via the legacy vmm_translate_user_addr
 *      path (Pull-Map kernel pointer into the user PT). Crates are
 *      written by handlers AND have their descriptor pointer captured
 *      by async handlers (storage_ops:async_ctx→out_crate); the Pull-Map
 *      pointer points to USER memory which outlives the dispatch, so
 *      the async write of crates[i].size lands in the user's heap
 *      directly and survives our stage release. crate_count > ~102
 *      would silently truncate, but CrateIsValid trips on the garbage
 *      and ManifestExecuteOnce rejects gracefully — no kernel leak.
 *   4. Run ManifestExecuteOnce on the staged Manifest + Pull-Mapped Crates.
 *   5. Release the stage. No write-back step: Manifest is const (nothing
 *      to commit), Crates were never staged (writes already live to user).
 *
 * Release ordering: stage is released BEFORE execution_deck_handler pushes
 * the synchronous Result. That hands the per-K-Core scratch back for the
 * next dispatch immediately and avoids holding it across the wake IPI
 * inside execution_deck_handler.
 */
static void guide_process_manifest_pocket(Pocket *pocket, process_t *proc)
{
    uint64_t manifest_uaddr = PocketManifestAddr(pocket);
    uint32_t manifest_size  = PocketManifestSize(pocket);
    uint64_t crates_uaddr   = PocketCratesAddr(pocket);
    uint16_t crate_count    = PocketCrateCount(pocket);

    if (manifest_uaddr == 0 || manifest_size < sizeof(Manifest)) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        execution_deck_handler(pocket, proc);
        return;
    }
    if (manifest_size > MANIFEST_RAW_MAX_SIZE) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        execution_deck_handler(pocket, proc);
        return;
    }

    /* Crate[] uses vmm_translate_user_addr below (Pull-Map, async-safe).
     * That helper silently clips ranges that cross a 4 KiB page boundary,
     * so we explicitly cap crate_count at MAX_CRATES_PER_POCKET to fail
     * fast with a clear error code instead of letting CrateIsValid trip
     * on past-page garbage later. 100 crates × 40 B = 4000 B fits one
     * page; the real boxlib MfCall1 / ManifestSubmit shapes use ≤ 4. */
    if (crate_count > MAX_CRATES_PER_POCKET) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        execution_deck_handler(pocket, proc);
        return;
    }

    ManifestStage *st = ManifestStageCurrent();
    if (!st) {
        /* Pre-amp_init path or unknown K-Core — should never trigger
         * post-boot, but a hard fail is preferable to silent corruption. */
        pocket->error_code = ERR_NOT_INITIALIZED;
        execution_deck_handler(pocket, proc);
        return;
    }

    ManifestStageGrant grant;
    error_t rc = ManifestStageAcquire(st, manifest_size, &grant);
    if (rc != OK) {
        pocket->error_code = (uint32_t)rc;
        execution_deck_handler(pocket, proc);
        return;
    }

    uint8_t *m_kbuf = (uint8_t *)grant.kbuf;

    /* Stage the Manifest stream. vmm_user_buf_in_into walks the user PT
     * one phys page at a time — safe for any range up to the stage's
     * grant size. After this returns OK the kernel owns a contiguous
     * copy of every byte; the user's original bytes can mutate without
     * affecting dispatch. */
    if (vmm_user_buf_in_into(proc->cabin, (uintptr_t)manifest_uaddr,
                             (size_t)manifest_size, m_kbuf) != 0) {
        ManifestStageRelease(st, &grant);
        pocket->error_code = ERR_INVALID_ADDRESS;
        execution_deck_handler(pocket, proc);
        return;
    }

    /* Pull-Map translate Crates: kernel pointer that aliases user memory.
     * Writes by handlers (crates[i].size = N) land directly in user heap
     * and remain visible after stage release. Async handlers capture this
     * pointer into per-op state; it stays valid for the lifetime of the
     * user process because Pull-Map covers all of phys RAM. */
    Crate *crates_kp = NULL;
    if (crate_count > 0) {
        size_t crates_bytes = (size_t)crate_count * sizeof(Crate);
        crates_kp = (Crate *)vmm_translate_user_addr(proc->cabin,
                                                     (uintptr_t)crates_uaddr,
                                                     crates_bytes);
        if (!crates_kp) {
            ManifestStageRelease(st, &grant);
            pocket->error_code = ERR_INVALID_ADDRESS;
            execution_deck_handler(pocket, proc);
            return;
        }
    }

    OpContext ctx;
    ctx.proc       = proc;
    ctx.target_pid = pocket->target_pid;
    ctx.flags      = pocket->flags;
    ctx.pier_id    = PocketPierId(pocket);
    ctx._pad       = 0;

    ManifestExecResult result;
    rc = ManifestExecuteOnce(m_kbuf, manifest_size,
                              crates_kp, crate_count,
                              &ctx, &result);
    pocket->error_code = (uint32_t)rc;

    /* Log every non-OK manifest. Filter to test PIDs (3+) only; PIDs 1 and
     * 2 (display, shell) generate constant kb/HW chatter we don't care
     * about for race hunting. */
    if (rc != OK && proc->pid >= 3) {
        const Manifest *mh = (const Manifest *)m_kbuf;
        debug_printf("[MANIFEST_FAIL] PID=%u rc=%d ops=%u/%u "
                     "magic=0x%x ver=%u op_count=%u total=%u uaddr=0x%lx tier=%u\n",
                     proc->pid, rc, result.completed_ops, result.total_ops,
                     mh->magic, mh->version, mh->op_count, mh->total_size,
                     (unsigned long)manifest_uaddr, (unsigned)grant.tier);
    }

    /* Clear envelope payload fields so the Result delivered to the sender's
     * ResultRing does not leak the Manifest user vaddr. Zero target_pid:
     * IPC delivery is the explicit job of system.route / system.broadcast
     * ops; execution_deck_handler writes only the local confirmation
     * Result back to the sender. */
    pocket->manifest_addr = 0;
    pocket->manifest_size = 0;
    pocket->target_pid    = 0;

    /* When an op parks the process asynchronously (PROC_WAITING) and returns
     * ERR_WOULD_BLOCK, no synchronous Result should be pushed. The async
     * path (KResultPush from obj_read_finish, TouchRestDeliver, etc.) will
     * deliver the real result later. Pushing ERR_WOULD_BLOCK now would
     * cause result_wait to return immediately with an empty out-crate
     * before the event fires.
     *
     * Synchronous ERR_WOULD_BLOCK (e.g. HwKbReadline with no data ready)
     * does NOT set PROC_WAITING — the op writes a "no data" marker in the
     * out_crate and still needs the Result delivered so the caller can
     * retry. */
    if (rc == ERR_WOULD_BLOCK && process_get_state(proc) == PROC_WAITING) {
        ManifestStageRelease(st, &grant);
        return;
    }

    /* Release scratch BEFORE the sync Result push — gets the per-K-Core
     * scratch available for the next dispatch immediately, and avoids
     * holding it across the wake IPI inside execution_deck_handler. */
    ManifestStageRelease(st, &grant);
    execution_deck_handler(pocket, proc);
}

/* Process one Pocket from a process's PocketRing. */
static void guide_process_pocket(process_t *proc)
{
    if (!proc || !proc->cabin) return;

    Pocket *pocket = KPocketPeek(proc);
    if (!pocket) return;

    /* Kernel sets pid (security: userspace can't forge it). */
    pocket->pid = proc->pid;
    pocket->error_code = OK;

    /* Yield: cooperative tick, no work, no Result. */
    if (pocket->flags & POCKET_FLAG_YIELD) {
        KPocketPop(proc);
        return;
    }

    /* All non-yield pockets must carry the Manifest flag in Phase 12. */
    if (pocket->flags & POCKET_FLAG_MANIFEST) {
        guide_process_manifest_pocket(pocket, proc);
        KPocketPop(proc);
        return;
    }

    /* Malformed pocket — neither yield nor manifest. Deliver an error
     * Result to the sender and drop. */
    pocket->error_code = ERR_INVALID_POCKET;
    debug_printf("[GUIDE] PID %u sent non-manifest pocket (flags=0x%x)\n",
                 proc->pid, pocket->flags);
    execution_deck_handler(pocket, proc);
    KPocketPop(proc);
}

void guide(void)
{
    uint32_t pockets_processed = 0;
    uint32_t perf_snapshot = perf_trace_snapshot();

    while (!ready_queue_is_empty(&g_ready_queue)) {
        process_t *proc = ready_queue_pop(&g_ready_queue);
        if (!proc) break;

        while (!KPocketIsEmpty(proc)) {
            guide_process_pocket(proc);
            pockets_processed++;
        }

        if (process_get_state(proc) == PROC_WAITING) {
            process_set_state(proc, PROC_WORKING);
        }
    }

    if (pockets_processed > 0) {
        perf_trace_flush_since(perf_snapshot);
    }
}

void guide_process_one(process_t *proc)
{
    if (!proc || !proc->cabin) return;

    uint32_t perf_snapshot = perf_trace_snapshot();
    uint32_t count = 0;

    while (!KPocketIsEmpty(proc)) {
        guide_process_pocket(proc);
        count++;
    }

    if (count > 0) {
        perf_trace_flush_since(perf_snapshot);
    }
}
