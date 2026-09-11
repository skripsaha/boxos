
#include "guide.h"
#include "chit.h"
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

    error_t stage_rc = ManifestStageInitAll();
    if (stage_rc != OK) {
        panic("[GUIDE] ManifestStageInitAll failed: %s\n", ErrorString(stage_rc));
    }

    debug_printf("[GUIDE] ReadyQueue initialized (intrusive, unbounded)\n");
}

static volatile uint64_t g_dispatch_enclosed;
static volatile uint64_t g_dispatch_addressed;

void guide_dispatch_stats(uint64_t out[2])
{
    out[0] = __atomic_load_n(&g_dispatch_enclosed,  __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_dispatch_addressed, __ATOMIC_RELAXED);
}

static void guide_process_manifest_pocket(Pocket *pocket, process_t *proc)
{
    uint64_t manifest_uaddr = PocketManifestAddr(pocket);
    uint32_t manifest_size  = PocketManifestSize(pocket);
    uint64_t crates_uaddr   = PocketCratesAddr(pocket);
    uint16_t crate_count    = PocketCrateCount(pocket);

    bool handle_mode = (pocket->flags & POCKET_FLAG_MANIFEST_HANDLE) != 0;
    bool enclosed    = (pocket->flags & POCKET_FLAG_ENCLOSED) != 0;

    ManifestStage      *st         = NULL;
    ManifestStageGrant  grant      = { NULL, 0, MANIFEST_STAGE_TIER_INVALID, {0,0,0} };
    uint8_t            *m_kbuf     = NULL;
    ManifestHandle      handle     = MANIFEST_HANDLE_INVALID;
    CompiledManifest   *cm_pinned  = NULL;

    error_t rc;

    if (handle_mode && enclosed) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        execution_deck_handler(pocket, proc);
        return;
    }

    if (handle_mode) {
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
    } else if (enclosed) {
        if (manifest_size < sizeof(Manifest) || manifest_size > POCKET_ENCLOSURE_MAX) {
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
        memcpy(m_kbuf, pocket->enclosure, manifest_size);
        __atomic_add_fetch(&g_dispatch_enclosed, 1, __ATOMIC_RELAXED);
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
        __atomic_add_fetch(&g_dispatch_addressed, 1, __ATOMIC_RELAXED);
    }

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
    ctx.submit_cookie     = PocketCookie24(pocket);
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

    pocket->manifest_addr = 0;
    pocket->manifest_size = 0;
    pocket->target_pid    = 0;

    if (rc == ERR_WOULD_BLOCK && async_owns_crates) {
        if (ctx.submit_cookie != 0) {
            ChitView cv;
            ChitPeek(proc, &cv);
            if (cv.cookie != ctx.submit_cookie)
                kprintf("[GUIDE] DEFECT: pid %u deferred the answer to token 0x%06x "
                        "(%u op(s)) without leaving a chit — Nightwatch cannot see "
                        "who owes it\n",
                        proc->pid, ctx.submit_cookie, (unsigned)result.total_ops);
        }
        if (st)        ManifestStageRelease(st, &grant);
        if (cm_pinned) ManifestRelease(handle);
        return;
    }

    if (crates_kp) {
        crate_stage_commit_and_release(crates_kp, crate_count,
                                        proc->cabin->vmm, crates_uaddr);
    }

    if (st)        ManifestStageRelease(st, &grant);
    if (cm_pinned) ManifestRelease(handle);
    execution_deck_handler(pocket, proc);
}

static void guide_process_pocket(process_t *proc)
{
    if (!proc || !proc->cabin) return;

    uint64_t pos;
    Pocket  *pocket = KPocketPeek(proc, &pos);
    if (!pocket) return;

    pocket->pid = proc->pid;
    pocket->error_code = OK;

    if (pocket->flags & POCKET_FLAG_MANIFEST) {
        guide_process_manifest_pocket(pocket, proc);
        (void)KPocketPopAt(proc, pos);
        return;
    }

    pocket->error_code = ERR_INVALID_POCKET;
    debug_printf("[GUIDE] PID %u sent non-manifest pocket (flags=0x%x)\n",
                 proc->pid, pocket->flags);
    execution_deck_handler(pocket, proc);
    (void)KPocketPopAt(proc, pos);
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