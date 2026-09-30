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
#include "chit.h"        /* ChitPeek — a deferred answer must have left a chit */
#include "execution_deck.h"
#include "touch.h"
#include "bay.h"
#include "brook.h"
#include "process.h"
#include "use_context.h"   /* a strand's verdict recounted as it enters */
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
#include "kheap.h"       /* the pocket's charge: whom the kernel heap serves */
#include "auth_tags.h"

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

/* Dispatch transport counters — how many Manifests arrived enclosed in the
 * envelope vs named by address. Surfaced via system.perf.dump. The split is
 * the runtime witness that the enclosure path actually runs (a green result
 * with zero enclosed dispatches would mean the path was never exercised). */
static volatile uint64_t g_dispatch_enclosed;
static volatile uint64_t g_dispatch_addressed;

void guide_dispatch_stats(uint64_t out[2])
{
    out[0] = __atomic_load_n(&g_dispatch_enclosed,  __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_dispatch_addressed, __ATOMIC_RELAXED);
}

/*
 * Manifest-mode dispatch.
 *
 *   1. Validate envelope sizes against MANIFEST_RAW_MAX_SIZE. No cap on
 *      crate_count — CrateStage handles arbitrary sizes safely.
 *   2. Take the Manifest BYTES onto the kernel side. An ENCLOSED envelope
 *      carries them inside the envelope itself — one bounded memcpy out of
 *      the guide's OWN copy of the ring slot (guide_process_pocket took the
 *      whole 128 bytes before dispatch), no cabin-memory read at all, so a
 *      cabin scribbling on its slot mid-dispatch races only with itself and
 *      never with a validation already passed. An addressed envelope stages
 *      them into this K-Core's ManifestStage scratch via
 *      vmm_user_buf_in_into — that cabin memory must still be alive, which
 *      is the submitter's side of the contract (it waits, or it owns the
 *      bytes past the call; see boxos_pocket.h). Manifests are READ-ONLY
 *      in handlers (const ManifestOp *) and no handler captures op→params
 *      pointers into async state, so both copies can be dropped at
 *      dispatch exit.
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
    bool enclosed    = (pocket->flags & POCKET_FLAG_ENCLOSED) != 0;

    ManifestStage      *st         = NULL;
    ManifestStageGrant  grant      = { NULL, 0, MANIFEST_STAGE_TIER_INVALID, {0,0,0} };
    uint8_t            *m_kbuf     = NULL;
    ManifestHandle      handle     = MANIFEST_HANDLE_INVALID;
    CompiledManifest   *cm_pinned  = NULL;   /* refcount pin (handle-mode) */

    error_t rc;

    /* A handle names a compiled form already in kernel memory; an enclosure
     * carries raw bytes. Claiming both at once is contradictory. */
    if (handle_mode && enclosed) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        execution_deck_handler(pocket, proc);
        return;
    }

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
    } else if (enclosed) {
        /* The letter is in the envelope, and the envelope is already the
         * guide's own copy of the ring slot — the cabin cannot reach it any
         * more, so validation and execution cannot be raced. It moves once
         * more, into this K-Core's staging scratch, so that the manifest's
         * home and lifetime are identical to the addressed path and the
         * dispatch frame stays small. */
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

    /* Clear envelope payload fields so the Result delivered to the sender's
     * ResultRing does not leak the Manifest user vaddr. */
    pocket->manifest_addr = 0;
    pocket->manifest_size = 0;
    pocket->target_pid    = 0;

    /*
     * Async-park detection — handler-set flag, not state read.
     *
     * The flag is set by async handlers (ObjReadAsync setup, a
     * WriteJob that parks its caller) BEFORE they hand control back via ERR_WOULD_BLOCK,
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
        /* A deferred answer to a submit somebody is waiting for MUST have
         * left a chit (chit.h): that is what lets Nightwatch tell a promise
         * from a drop. ChitGive is the only writer of the flag, so this can
         * only fail for a handler that set the flag some other way — a
         * coding error, said at once rather than found two looks later. A
         * completion that already outran us has KEPT the chit, not cleared
         * it, so the token still matches. */
        if (ctx.submit_cookie != 0) {
            ChitView cv;
            ChitPeek(proc, &cv);
            if (cv.cookie != ctx.submit_cookie)
                kprintf("[GUIDE] DEFECT: pid %u deferred the answer to token 0x%06x "
                        "(%u op(s)) without leaving a chit — Nightwatch cannot see "
                        "who owes it\n",
                        proc->pid, ctx.submit_cookie, (unsigned)result.total_ops);
        }
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

/* The class a cabin's pockets charge the kernel heap to (kheap.h): the
 * highest authority key it carries. Bypass authorises as system does, so it
 * is charged as system is. */
static KHeapCharge guide_charge_of(const cabin_t *cabin)
{
    uint32_t auth = __atomic_load_n(&cabin->auth_bits, __ATOMIC_RELAXED);
    if (auth & AUTH_TAG_GOD)                        return KHEAP_CHARGE_GOD;
    if (auth & (AUTH_TAG_SYSTEM | AUTH_TAG_BYPASS)) return KHEAP_CHARGE_SYSTEM;
    if (auth & AUTH_TAG_UTILITY)                    return KHEAP_CHARGE_UTILITY;
    if (auth & AUTH_TAG_APP)                        return KHEAP_CHARGE_APP;
    return KHEAP_CHARGE_OTHER;
}

/*
 * Process one Pocket from a process's PocketRing.
 *
 * The guide TAKES the pocket; it does not borrow the cabin's slot. What
 * KPocketPeek hands back is the READ form of vmm_translate_user_addr — a
 * supervisor-writable Pull Map alias of the cabin's page, and vmm.h is
 * explicit that a store through it "consults neither the cabin's own R/W
 * bit, nor CR0.WP, nor CR4.SMAP", so it is "safe for reads and unsafe as a
 * write channel". The three fields the guide used to stamp into that slot —
 * pid, error_code, and later the cleared envelope — were exactly such
 * stores, and the address they landed at is not the kernel's to trust: a
 * slot vaddr is slots_base + (idx % slot_count_max) * slot_size, and all
 * three of those live in the ring header page the cabin can write. A cabin
 * that names its own read-only text there gets the kernel to scribble on a
 * page it is not allowed to write, and one shared with every other process
 * running the same image.
 *
 * Copying the whole 128-byte slot once closes the other half of it too. The
 * slot stays user-writable for as long as the dispatch runs, so every field
 * read twice could be two different values; the enclosure was already copied
 * out for that exact reason (see guide_process_manifest_pocket), and now the
 * envelope travels the same way. Nothing downstream needs the cabin to see
 * these writes: the answer goes back through the ResultRing, and boxlib
 * never reads a submitted slot again.
 */
static void guide_process_pocket(process_t *proc)
{
    if (!proc || !proc->cabin) return;

    uint64_t pos;
    const Pocket *slot = KPocketPeek(proc, &pos);
    if (!slot) return;

    Pocket pocket;
    memcpy(&pocket, slot, sizeof(pocket));

    /* Kernel sets pid (security: userspace can't forge it). */
    pocket.pid = proc->pid;
    pocket.error_code = OK;

    /* Whatever serving this pocket makes the kernel hold is the cabin's to
     * answer for, from the staging of its crates to its Result. */
    KHeapScope outer = KHeapScopeEnter(guide_charge_of(proc->cabin), proc->pid);

    /* Every pocket carries the Manifest flag since Phase 12. */
    if (pocket.flags & POCKET_FLAG_MANIFEST) {
        guide_process_manifest_pocket(&pocket, proc);
    } else {
        /* Malformed pocket — neither yield nor manifest. Deliver an error
         * Result to the sender and drop. */
        pocket.error_code = ERR_INVALID_POCKET;
        debug_printf("[GUIDE] PID %u sent non-manifest pocket (flags=0x%x)\n",
                     proc->pid, pocket.flags);
        execution_deck_handler(&pocket, proc);
    }

    KHeapScopeLeave(outer);
    (void)KPocketPopAt(proc, pos);
}

void guide(void)
{
    uint32_t pockets_processed = 0;
    uint32_t perf_snapshot = perf_trace_snapshot();

    while (!ready_queue_is_empty(&g_ready_queue)) {
        process_t *proc = ready_queue_pop(&g_ready_queue);
        if (!proc) break;

        UseContextVerdictRefresh(proc);
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

    /* The strand is in the kernel: a stale Use Context verdict on its cabin
     * is recounted here, with interrupts on, not in the scheduler. */
    UseContextVerdictRefresh(proc);
    while (!KPocketIsEmpty(proc)) {
        guide_process_pocket(proc);
        count++;
    }

    if (count > 0) {
        perf_trace_flush_since(perf_snapshot);
    }
}
