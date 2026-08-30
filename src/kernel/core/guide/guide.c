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
#include "crate_stage.h"
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
 *   1. Validate envelope sizes against MANIFEST_RAW_MAX_SIZE. No cap on
 *      crate_count — CrateStage handles arbitrary sizes safely.
 *   2. Stage the Manifest BYTES into this K-Core's ManifestStage scratch
 *      via vmm_user_buf_in_into. Manifests are READ-ONLY in handlers
 *      (const ManifestOp *) and no handler captures op→params pointers
 *      into async state, so the scratch can be released at dispatch exit.
 *   3. Stage the Crate[] array via CrateStage (kmalloc + page-walked copy
 *      in). Unlike Manifest scratch, Crate ownership is conditional:
 *      sync handlers leave it on the dispatcher to commit_out + free;
 *      async handlers (storage_ops ObjRead, write_job) take ownership at
 *      ERR_WOULD_BLOCK + PROC_WAITING and call
 *      crate_stage_commit_and_release at I/O completion.
 *   4. Run ManifestExecuteOnce on the staged Manifest + staged Crates.
 *   5. Release: ManifestStage scratch always. CrateStage only on the sync
 *      path (commit + free); on async-park the handler owns the buffer.
 *
 * Release ordering: ManifestStage scratch is released BEFORE
 * execution_deck_handler pushes the synchronous Result. That hands the
 * per-K-Core scratch back for the next dispatch immediately and avoids
 * holding it across the wake IPI inside execution_deck_handler.
 */
static void guide_process_manifest_pocket(Pocket *pocket, process_t *proc)
{
    uint64_t manifest_uaddr = PocketManifestAddr(pocket);
    uint32_t manifest_size  = PocketManifestSize(pocket);
    uint64_t crates_uaddr   = PocketCratesAddr(pocket);
    uint16_t crate_count    = PocketCrateCount(pocket);

    /*
     * Handle-mode dispatch (POCKET_FLAG_MANIFEST_HANDLE) skips:
     *   - ManifestStage scratch acquire + page-walked manifest copy
     *   - per-syscall validate + per-op OpRegistryLookup walk
     * The CompiledManifest is already cached in kernel memory from a
     * prior SYSTEM_OP_MANIFEST_COMPILE; we just verify ownership and
     * call ManifestExecute on the cached handler-resolved op stream.
     * Crate staging remains identical to the bytes-mode path — handlers
     * still mutate crates[i].size etc., and async handlers still pin
     * the staged kbuf into their async_ctx.
     */
    bool handle_mode = (pocket->flags & POCKET_FLAG_MANIFEST_HANDLE) != 0;

    ManifestStage      *st         = NULL;
    ManifestStageGrant  grant      = { NULL, 0, MANIFEST_STAGE_TIER_INVALID, {0,0,0} };
    uint8_t            *m_kbuf     = NULL;
    ManifestHandle      handle     = MANIFEST_HANDLE_INVALID;
    CompiledManifest   *cm_pinned  = NULL;   /* refcount pin (handle-mode) */

    error_t rc;

    if (handle_mode) {
        /* manifest_addr carries a 64-bit handle, not a vaddr. manifest_size
         * is ignored. Verify ownership via Resolve (pins the form). */
        handle = (ManifestHandle)manifest_uaddr;
        cm_pinned = ManifestResolve(handle);
        if (!cm_pinned) {
            pocket->error_code = ERR_INVALID_ARGUMENT;
            execution_deck_handler(pocket, proc);
            return;
        }
        if (cm_pinned->owner_pid != proc->pid) {
            ManifestRelease(handle);
            pocket->error_code = ERR_ACCESS_DENIED;
            execution_deck_handler(pocket, proc);
            return;
        }
    } else {
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

        st = ManifestStageCurrent();
        if (!st) {
            pocket->error_code = ERR_NOT_INITIALIZED;
            execution_deck_handler(pocket, proc);
            return;
        }

        rc = ManifestStageAcquire(st, manifest_size, &grant);
        if (rc != OK) {
            pocket->error_code = (uint32_t)rc;
            execution_deck_handler(pocket, proc);
            return;
        }

        m_kbuf = (uint8_t *)grant.kbuf;

        if (vmm_user_buf_in_into(proc->cabin->vmm, (uintptr_t)manifest_uaddr,
                                 (size_t)manifest_size, m_kbuf) != OK) {
            ManifestStageRelease(st, &grant);
            pocket->error_code = ERR_INVALID_ADDRESS;
            execution_deck_handler(pocket, proc);
            return;
        }
    }

    /* Crate staging is identical for both modes — async handlers still pin
     * the staged kbuf into async_ctx and call crate_stage_commit_and_release
     * at I/O completion. */
    Crate *crates_kp = NULL;
    if (crate_count > 0) {
        rc = crate_stage_in(proc->cabin->vmm, crates_uaddr, crate_count, &crates_kp);
        if (rc != OK) {
            if (cm_pinned) ManifestRelease(handle);
            if (st)        ManifestStageRelease(st, &grant);
            pocket->error_code = (uint32_t)rc;
            execution_deck_handler(pocket, proc);
            return;
        }
    }

    bool async_owns_crates = false;

    OpContext ctx;
    ctx.proc              = proc;
    ctx.target_pid        = pocket->target_pid;
    ctx.flags             = pocket->flags;
    ctx.pier_id           = PocketPierId(pocket);
    ctx.crate_count       = crate_count;
    ctx.crates_uaddr      = crates_uaddr;
    ctx.async_owns_crates = &async_owns_crates;

    ManifestExecResult result;
    if (handle_mode) {
        rc = ManifestExecute(handle, crates_kp, crate_count, &ctx, &result);
    } else {
        rc = ManifestExecuteOnce(m_kbuf, manifest_size,
                                  crates_kp, crate_count,
                                  &ctx, &result);
    }
    pocket->error_code = (uint32_t)rc;

    if (rc != OK && proc->pid >= 3) {
        if (handle_mode) {
            debug_printf("[MANIFEST_FAIL] PID=%u rc=%d ops=%u/%u "
                         "mode=HANDLE handle=0x%lx\n",
                         proc->pid, rc, result.completed_ops, result.total_ops,
                         (unsigned long)handle);
        } else {
            const Manifest *mh = (const Manifest *)m_kbuf;
            debug_printf("[MANIFEST_FAIL] PID=%u rc=%d ops=%u/%u "
                         "magic=0x%x ver=%u op_count=%u total=%u uaddr=0x%lx tier=%u\n",
                         proc->pid, rc, result.completed_ops, result.total_ops,
                         mh->magic, mh->version, mh->op_count, mh->total_size,
                         (unsigned long)manifest_uaddr, (unsigned)grant.tier);
        }
    }

    /* Clear envelope payload fields so the Result delivered to the sender's
     * ResultRing does not leak the Manifest user vaddr. */
    pocket->manifest_addr = 0;
    pocket->manifest_size = 0;
    pocket->target_pid    = 0;

    /*
     * Async-park detection — handler-set flag, not state read.
     *
     * The flag is set by async handlers (ObjReadAsync setup,
     * ObjWriteAsync) BEFORE they hand control back via ERR_WOULD_BLOCK,
     * to signal explicit ownership transfer of the staged crates kbuf.
     * The previous state-based check (PROC_WAITING) raced on fast async
     * paths: if AHCI completion fired before the dispatcher rechecked,
     * the state was already PROC_WORKING, sync cleanup ran, and the
     * staged crates were double-freed by both dispatcher and async.
     *
     * Sync ERR_WOULD_BLOCK (HwKbReadline "no data ready" etc.) does NOT
     * set the flag — dispatcher cleans up normally and pushes the
     * "would block" Result so userspace can retry.
     */
    if (rc == ERR_WOULD_BLOCK && async_owns_crates) {
        /* Async handler owns crates_kp; it will commit+free at I/O
         * completion via crate_stage_commit_and_release. */
        if (st)        ManifestStageRelease(st, &grant);
        if (cm_pinned) ManifestRelease(handle);
        return;
    }

    /* Sync path: write any handler-mutated Crate descriptors (crates[i].size
     * for output crates) back to user memory and release the staged kbuf. */
    if (crates_kp) {
        crate_stage_commit_and_release(crates_kp, crate_count,
                                        proc->cabin->vmm, crates_uaddr);
    }

    /* Release scratch + handle pin (both no-ops if respective mode was
     * inactive) BEFORE the sync Result push to maximize K-Core throughput. */
    if (st)        ManifestStageRelease(st, &grant);
    if (cm_pinned) ManifestRelease(handle);
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

        /* Nothing is un-parked here, and that is the point. A strand left
         * this loop PROC_WAITING for exactly one reason — a handler parked it
         * and owes it a Result — so waking it would be waking it on behalf of
         * an event that has not happened. The line that used to stand here
         * could not tell that park apart from the caller's own "being served"
         * mark, because both were spelled PROC_WAITING; sync_syscall_dispatch
         * no longer writes that mark, so there is nothing left to restore.
         * See the comment at its call site in idt.c. */
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
