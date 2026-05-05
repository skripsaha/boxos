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
#include "manifest_exec.h"
#include "op_registry.h"

ReadyQueue g_ready_queue;

void guide_init(void)
{
    debug_printf("[GUIDE] Initializing Guide Dispatcher...\n");
    ready_queue_init(&g_ready_queue);
    TouchInit();
    perf_trace_init();
    debug_printf("[GUIDE] ReadyQueue initialized (intrusive, unbounded)\n");
}

/* Manifest-mode dispatch: extract Manifest+Crates from reinterpreted Pocket
 * fields, translate user pointers, run ManifestExecuteOnce, deliver Result
 * via execution_deck_handler. */
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

    void *manifest_kp = vmm_translate_user_addr(proc->cabin,
                                                (uintptr_t)manifest_uaddr,
                                                (size_t)manifest_size);
    if (!manifest_kp) {
        pocket->error_code = ERR_INVALID_ADDRESS;
        execution_deck_handler(pocket, proc);
        return;
    }

    Crate *crates_kp = NULL;
    if (crate_count > 0) {
        size_t crates_bytes = (size_t)crate_count * sizeof(Crate);
        crates_kp = vmm_translate_user_addr(proc->cabin,
                                            (uintptr_t)crates_uaddr,
                                            crates_bytes);
        if (!crates_kp) {
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
    error_t rc = ManifestExecuteOnce(manifest_kp, manifest_size,
                                     crates_kp, crate_count,
                                     &ctx, &result);
    pocket->error_code = (uint32_t)rc;

    /* Log every non-OK manifest. Filter to test PIDs (3+) only; PIDs 1 and
     * 2 (display, shell) generate constant kb/HW chatter we don't care
     * about for race hunting. */
    if (rc != OK && proc->pid >= 3) {
        const Manifest *mh = (const Manifest *)manifest_kp;
        debug_printf("[MANIFEST_FAIL] PID=%u rc=%d ops=%u/%u "
                     "magic=0x%x ver=%u op_count=%u total=%u uaddr=0x%lx\n",
                     proc->pid, rc, result.completed_ops, result.total_ops,
                     mh->magic, mh->version, mh->op_count, mh->total_size,
                     (unsigned long)manifest_uaddr);
    }

    /* Clear legacy data fields so the Result delivered to the sender's
     * ResultRing does not leak the Manifest user vaddr. Zero target_pid:
     * IPC delivery is the explicit job of system.route / system.broadcast
     * ops; execution_deck_handler writes only the local confirmation
     * Result back to the sender. */
    pocket->data_addr   = 0;
    pocket->data_length = 0;
    pocket->target_pid  = 0;

    /* When an op parks the process asynchronously (PROC_WAITING) and returns
     * ERR_WOULD_BLOCK, no synchronous Result should be pushed. The async path
     * (KResultPush from TouchRestDeliver, etc.) will deliver the real result
     * later. Pushing ERR_WOULD_BLOCK now would cause result_wait to return
     * immediately with an empty out-crate before the event has fired.
     *
     * Synchronous ERR_WOULD_BLOCK (e.g. HwKbReadline with no data ready) does
     * NOT set PROC_WAITING — the op writes a "no data" marker in the out_crate
     * and still needs the Result delivered so the caller can retry. */
    if (rc == ERR_WOULD_BLOCK && process_get_state(proc) == PROC_WAITING) return;

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
