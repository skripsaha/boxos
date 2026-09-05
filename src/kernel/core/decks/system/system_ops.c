/*
 * System Deck — Manifest-native handlers (Phase 8 complete).
 *
 * Surface (all opcodes already defined in system_deck.h):
 *
 *   IPC                 route, broadcast, listen
 *   Process lifecycle   spawn, kill, info, exec
 *   Context             ctx.use
 *   Buffers             buf.alloc, buf.free, buf.resize
 *   Tags                tag.add, tag.remove, tag.check
 *   Filesystem          defrag, frag_score
 *   Telemetry           perf.dump
 *
 * Inputs that are inherently small (PIDs, handles, sizes) ride in op->params.
 * Variable inputs (ELF tags, filenames, tag strings, IPC payload) live in
 * in_crate. Variable outputs (proc info, frag stats, buf descriptors) live in
 * out_crate. There is no fixed 192-byte response cargo.
 */

#include "klib.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "auth_tags.h"
#include "manifest_stage.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "crate_io.h"
#include "system_deck.h"
#include "buffer_registry.h"
#include "touch.h"
#include "result.h"
#include "result_ring.h"
#include "kring.h"
#include "kresult.h"
#include "proc_exit.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "pid_allocator.h"  /* pid_generation — own-child authority defeats pid-reuse */
#include "tagfs.h"
#include "use_context.h"
#include "perf_trace.h"
#include "kernel_config.h"
#include "amp.h"
#include "guide.h"   /* guide_dispatch_stats — enclosed/addressed split for perf.dump */
#include "atomics.h"        /* rdtsc — system.proc.cputime's in-flight term */

/* TSC frequency, for turning a cycle delta into microseconds. */
extern uint64_t cpu_get_tsc_freq_khz(void);
#include "rtc.h"
#include "pit.h"
#include "cpu_calibrate.h"
#include "cpuid.h"
#include "fpu.h"   /* g_user_fsbase_used — TLS FS-base context-switch gate */
#include "sync_ops.h"
#include "cabin.h"          /* cabin_t.vmm — for StrandPool bind virt->phys walk */
#include "cabin_layout.h"   /* CABIN_USER_VA_CANONICAL_END — bind VA range check */
#include "strand_pool_abi.h" /* StrandPool — _Alignof for the bind alignment check */

#define MAX_BROADCAST_TARGETS  256u
#define BROADCAST_TAG_MAX      64u

#define MAX_CTX_USE_TAGS       3u
#define CTX_TAG_LENGTH         64u
#define CTX_USE_PARSE_BUF      512u

/* -------------------------------------------------------------------------
 * Crate I/O
 *
 * Fixed payloads move through crate_io.{h,c} (crate_read / crate_write) and
 * variable payloads through crate_in_buf / crate_out_alloc / crate_out_commit.
 * Each walks the user page table page-by-page, so a Crate whose payload
 * crosses a page boundary is copied across every backing frame instead of
 * being clipped to its first page (the vmm_translate_user_addr straddle bug).
 * ------------------------------------------------------------------------- */

/* Read a NUL-bounded copy of in_crate into a caller-supplied buffer via the
 * page-walked crate_io snapshot. Returns OK / ERR_INVALID_ARGUMENT /
 * ERR_INVALID_ADDRESS. */
static error_t sys_crate_string(const Crate *c, const OpContext *ctx,
                                char *dst, size_t dst_size)
{
    if (!c || c->size == 0 || dst_size == 0) return ERR_INVALID_ARGUMENT;

    size_t copy = c->size < dst_size - 1 ? c->size : dst_size - 1;
    error_t rc = crate_read(c, ctx, dst, copy);
    if (rc != OK) return rc;
    dst[copy] = '\0';
    if (dst[0] == '\0') return ERR_INVALID_ARGUMENT;
    return OK;
}

static bool target_alive(process_t *t)
{
    if (!t) return false;
    process_state_t s = process_get_state(t);
    return s != PROC_CRASHED && s != PROC_DONE;
}

static bool push_ipc_result(process_t *target,
                            uint32_t   sender_pid,
                            uint64_t   target_data_addr,
                            uint32_t   data_length)
{
    Result r;
    r.error_code  = OK;
    r.data_length = data_length;
    r.data_addr   = target_data_addr;
    r.sender_pid  = sender_pid;
    r.context     = KCTX_IPC;
    return KResultPush(target, &r);
}

/* =========================================================================
 *  IPC
 * ========================================================================= */

static int SysRoute(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                    const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)            return ERR_INVALID_ARGUMENT;
    if (ctx->target_pid == 0)          return ERR_INVALID_ARGUMENT;
    if (ctx->target_pid == ctx->proc->pid) return ERR_ROUTE_SELF;

    /* Pin the target with a refcount so it cannot be torn down between
     * the lookup and the result push. Without this, a concurrent
     * process_destroy on another core can free target's cabin/result_ring
     * while ipc_copy_to_heap or KResultPush still dereferences them. */
    process_t *target = process_find_ref(ctx->target_pid);
    if (!target) return ERR_PROCESS_NOT_FOUND;
    if (!target_alive(target)) {
        process_ref_dec(target);
        return ERR_PROCESS_NOT_FOUND;
    }

    uint64_t target_addr = 0;
    uint32_t length      = 0;
    int      rc          = OK;

    if (op->in_crate != CRATE_INDEX_NONE) {
        Crate *src = &crates[op->in_crate];
        if (src->size > 0) {
            length = (uint32_t)(src->size > UINT32_MAX ? UINT32_MAX : src->size);
            target_addr = ipc_copy_to_heap(ctx->proc, target,
                                           (uint64_t)src->addr, length);
            if (target_addr == 0) rc = ERR_NO_MEMORY;
        }
    }

    if (rc == OK && !push_ipc_result(target, ctx->proc->pid, target_addr, length)) {
        rc = ERR_ROUTE_TARGET_FULL;
    }

    process_ref_dec(target);
    return rc;
}

static int SysBroadcast(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size == 0 || op->param_size > BROADCAST_TAG_MAX) {
        return ERR_INVALID_ARGUMENT;
    }

    char tag[BROADCAST_TAG_MAX];
    size_t plen = op->param_size < sizeof(tag) ? op->param_size : sizeof(tag) - 1;
    memcpy(tag, op->params, plen);
    tag[plen] = '\0';
    if (tag[0] == '\0') return ERR_INVALID_ARGUMENT;

    Crate    *src    = NULL;
    uint32_t  length = 0;
    if (op->in_crate != CRATE_INDEX_NONE) {
        src    = &crates[op->in_crate];
        length = (uint32_t)(src->size > UINT32_MAX ? UINT32_MAX : src->size);
        /* Fail fast if the whole source range is unreadable, before fanning
         * out to subscribers. ipc_copy_to_heap re-reads it per target; this
         * page-walks the full range once so a bad address is ERR_INVALID_ADDRESS
         * rather than a misleading "no subscribers". */
        if (length > 0) {
            void *probe = crate_in_buf(src, ctx);
            if (!probe) return ERR_INVALID_ADDRESS;
            crate_buf_free(probe);
        }
    }

    /* Resolve the tag to its registry id once, outside the process-list
     * lock — sys_proc_has_tag's string/wildcard path would re-acquire
     * process_lock through process_snapshot_tags and self-deadlock. */
    uint16_t tid = tagfs_tag_lookup(tag);
    if (tid == TAGFS_INVALID_TAG_ID) return ERR_ROUTE_NO_SUBSCRIBERS;

    /* Phase 1: snapshot matching pids under process_list_lock so the
     * iter->next chain cannot mutate mid-walk (process_destroy unlinks
     * under the same lock). Tag check is the inline bitfield path —
     * no nested lock acquisition. */
    uint32_t pid_list[MAX_BROADCAST_TARGETS];
    uint16_t pid_count = 0;
    process_list_lock();
    for (process_t *iter = process_get_first();
         iter && pid_count < MAX_BROADCAST_TARGETS;
         iter = iter->next)
    {
        if (iter->pid == ctx->proc->pid)        continue;
        process_state_t s = iter->state;
        if (s == PROC_CRASHED || s == PROC_DONE) continue;
        if (!process_has_tag_id(iter, tid))      continue;
        pid_list[pid_count++] = iter->pid;
    }
    process_list_unlock();

    /* Phase 2: pin each target via process_find_ref before any cabin or
     * result_ring dereference; release the ref before moving on. This
     * is the same UAF-closing pattern as SysRoute. */
    uint32_t delivered = 0;
    for (uint16_t i = 0; i < pid_count; i++) {
        process_t *target = process_find_ref(pid_list[i]);
        if (!target) continue;

        /* Recheck liveness — pid could have been recycled. */
        if (!target_alive(target)) {
            process_ref_dec(target);
            continue;
        }

        uint64_t target_addr = 0;
        if (length > 0 && src) {
            target_addr = ipc_copy_to_heap(ctx->proc, target,
                                           (uint64_t)src->addr, length);
            if (target_addr == 0) {
                process_ref_dec(target);
                continue;
            }
        }
        if (push_ipc_result(target, ctx->proc->pid, target_addr, length)) {
            delivered++;
        }
        process_ref_dec(target);
        if (delivered >= MAX_BROADCAST_TARGETS) break;
    }
    return delivered > 0 ? OK : ERR_ROUTE_NO_SUBSCRIBERS;
}

/* =========================================================================
 *  Process lifecycle
 * ========================================================================= */

/* Shared grant gate for spawn-tags and tag.add: the granter must not confer any
 * auth privilege it does not itself hold (granted auth-level ⊆ granter auth-level).
 * god grants anything. stopped is a self-freeze, not an escalation, so it passes.
 *
 * This gates only the auth-privilege keys — unlike the PROC_EXEC merge path,
 * which rejects every reserved key. The asymmetry is deliberate: a spawn tag
 * set is wholly caller-supplied (there is no trusted file-tag base to protect),
 * and the non-auth reserved keys (name/autostart/snapshot/trashed/hidden) confer
 * no op-authority — none appear in any auth mask — so they are not escalations.
 *
 * Authority is the FIXED auth_bits (auth_tags.h) on both sides, so the subset
 * check no longer depends on the privilege tags interning below registry id 64.
 * The colon-split key keeps "god:foo" detected as a god request. */
static error_t proc_authorize_tag_grant(const char *tags, const process_t *spawner)
{
    uint32_t caller = spawner->cabin->auth_bits;
    if (caller & AUTH_TAG_GOD) return OK;       /* god may grant anything */

    uint32_t requested = 0;
    const char *p = tags;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t tlen = comma ? (size_t)(comma - p) : strlen(p);
        if (tlen == 0) { if (!comma) break; p = comma + 1; continue; }
        char key[PROCESS_TAG_SIZE];
        if (tlen >= sizeof(key)) return ERR_INVALID_ARGUMENT;
        size_t klen = tlen;
        for (size_t i = 0; i < tlen; i++) { if (p[i] == ':') { klen = i; break; } }
        memcpy(key, p, klen); key[klen] = '\0';
        requested |= auth_bit_for_key(key);
        if (!comma) break;
        p = comma + 1;
    }
    if (requested == 0) return OK;              /* nothing privileged requested */
    if (requested & AUTH_TAG_GOD) return ERR_ACCESS_DENIED;  /* only god grants god */

    const uint32_t levels[] = { OP_AUTH_APP, OP_AUTH_UTILITY, OP_AUTH_SYSTEM, OP_AUTH_NETWORK };
    for (size_t i = 0; i < sizeof(levels)/sizeof(levels[0]); i++) {
        uint32_t mask = auth_mask_for_level(levels[i]);
        if ((requested & mask) && !(caller & mask)) return ERR_ACCESS_DENIED;  /* amplification */
    }
    return OK;
}

/* Authority to MUTATE (kill/tag) a target: self ∨ same cabin (one trust domain,
 * shared address space) ∨ system-ensign (god|system|bypass) ∨ the exact child
 * this caller launched. "Own child" is (spawner_pid, spawner_gen) — a pid alone
 * is not identity (pids recycle), so a later process inheriting a dead spawner's
 * pid must NOT inherit authority. Parentless procs (autostart/init,
 * spawner_pid==0) are reachable only via the system ensign. */
static bool proc_has_authority_over(const process_t *caller, const process_t *target)
{
    if (!caller || !target) return false;
    if (caller->pid == target->pid) return true;
    /* Same cabin = one trust domain: sibling strands share this address space
     * and can already read/write each other's memory, so tagging or killing a
     * cabin-mate is an in-domain act — and is what lets a spawned strand run
     * box::tag_scope (its pid is not the cabin-main pid boxlib addresses). Guard
     * NULL so cabin-less procs don't alias; foreign targets keep a distinct
     * cabin_t and fall through. Cf. the same rule in SysStrandRelease. */
    if (caller->cabin && caller->cabin == target->cabin) return true;
    uint32_t cb = caller->cabin ? caller->cabin->auth_bits : 0;
    if (auth_level_permits(cb, OP_AUTH_SYSTEM)) return true;
    const cabin_t *tc = target->cabin;
    if (tc && tc->spawner_pid != PROCESS_INVALID_PID &&
        tc->spawner_pid == caller->pid &&
        tc->spawner_gen == pid_generation(caller->pid))
        return true;
    return false;
}

/* Boot self-test for proc_has_authority_over. Synthetic process_t/cabin_t on the
 * stack drive the predicate directly (it is static here). caller.pid is the top
 * pid index, which boot never allocates — its generation is a stable snapshot for
 * the run, so the match/stale pair reads one deterministic pid_generation value. */
error_t ProcAuthSelfTest(void)
{
    process_t caller, target;
    cabin_t   caller_cabin, target_cabin;
    memset(&caller, 0, sizeof(caller));
    memset(&target, 0, sizeof(target));
    memset(&caller_cabin, 0, sizeof(caller_cabin));
    memset(&target_cabin, 0, sizeof(target_cabin));
    caller.cabin = &caller_cabin;
    target.cabin = &target_cabin;

    caller.pid = PID_MAX_COUNT;                       /* index PID_MAX_COUNT-1, unallocated at boot */
    uint32_t caller_gen = pid_generation(caller.pid);

    /* self: same pid is permitted at any auth level (self-exit needs this). */
    target.pid = caller.pid;
    if (!proc_has_authority_over(&caller, &target)) {
        kprintf("[PROCAUTH] FAIL: self denied\n");
        return ERR_INTERNAL;
    }

    target.pid = caller.pid - 1;                      /* a distinct, foreign pid */

    /* foreign: no spawner link, caller holds no authority -> denied. */
    caller_cabin.auth_bits   = 0;
    target_cabin.spawner_pid  = PROCESS_INVALID_PID;
    target_cabin.spawner_gen  = 0;
    if (proc_has_authority_over(&caller, &target)) {
        kprintf("[PROCAUTH] FAIL: foreign permitted\n");
        return ERR_INTERNAL;
    }

    /* same cabin: the SAME foreign pid, now sharing the caller's cabin_t, is one
     * trust domain — authority holds with no privilege and no spawner link (the
     * box::tag_scope-from-a-strand case). Only the same-cabin clause can grant
     * here (self fails: pids differ; ensign fails: auth_bits==0; own-child fails:
     * no spawner link), so an ALLOW proves that clause is live and unmasked. */
    target.cabin = &caller_cabin;
    if (!proc_has_authority_over(&caller, &target)) {
        kprintf("[PROCAUTH] FAIL: same-cabin denied\n");
        return ERR_INTERNAL;
    }
    target.cabin = &target_cabin;

    /* own child: target records (caller.pid, caller's live generation). */
    target_cabin.spawner_pid = caller.pid;
    target_cabin.spawner_gen = caller_gen;
    if (!proc_has_authority_over(&caller, &target)) {
        kprintf("[PROCAUTH] FAIL: own child denied\n");
        return ERR_INTERNAL;
    }

    /* stale child: spawner_pid matches but the generation cannot — proves a
     * recycled pid does not inherit a dead spawner's authority. */
    target_cabin.spawner_gen = caller_gen ^ 0xFFFFu;
    if (proc_has_authority_over(&caller, &target)) {
        kprintf("[PROCAUTH] FAIL: stale child permitted (pid-reuse hole)\n");
        return ERR_INTERNAL;
    }

    /* system ensign: god (and system) reach a foreign target with no link. */
    target_cabin.spawner_pid = PROCESS_INVALID_PID;
    target_cabin.spawner_gen = 0;
    caller_cabin.auth_bits   = AUTH_TAG_GOD;
    if (!proc_has_authority_over(&caller, &target)) {
        kprintf("[PROCAUTH] FAIL: god over foreign denied\n");
        return ERR_INTERNAL;
    }
    caller_cabin.auth_bits = AUTH_TAG_SYSTEM;
    if (!proc_has_authority_over(&caller, &target)) {
        kprintf("[PROCAUTH] FAIL: system over foreign denied\n");
        return ERR_INTERNAL;
    }

    kprintf("[PROCAUTH] PASS\n");
    return OK;
}

/* SysProcSpawn loads a process from a caller-supplied physical-address ELF blob
 * with caller-supplied tags. proc_authorize_tag_grant gates those tags so a
 * spawned child can never gain an auth privilege the spawner itself lacks
 * (child auth-level ⊆ spawner auth-level; god may grant anything). Without this
 * a utility caller could spawn a "god" child (a phys address is obtainable via
 * the app-level MEMTAG_INFO base_phys field), i.e. utility→god escalation. */
/* SYSTEM_OP_PROC_SPAWN
 *   params:  [u64 binary_phys][u64 binary_size]
 *   in_crate: tags string (NUL-bounded)
 *   out_crate (optional): u32 new_pid */
static int SysProcSpawn(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)               return ERR_INVALID_ARGUMENT;
    if (op->param_size < 16)              return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint64_t binary_phys, binary_size;
    memcpy(&binary_phys, op->params,     sizeof(uint64_t));
    memcpy(&binary_size, op->params + 8, sizeof(uint64_t));

    if (binary_phys == 0 || binary_size == 0)            return ERR_INVALID_ARGUMENT;
    if (binary_phys & 0xFFF)                              return ERR_ALIGNMENT;
    if (binary_phys > 0x100000000ULL)                     return ERR_INVALID_ARGUMENT;
    if (binary_size > CONFIG_PROC_MAX_BINARY_SIZE)        return ERR_BINARY_TOO_LARGE;
    if (process_get_count() >= PROCESS_MAX_COUNT)         return ERR_PROCESS_LIMIT_EXCEEDED;

    char tags[PROCESS_TAG_SIZE];
    error_t srcrc = sys_crate_string(&crates[op->in_crate], ctx, tags, sizeof(tags));
    if (srcrc != OK) return srcrc;

    /* Gate the child's tags BEFORE creating it or reading the binary: a denied
     * spawn must create no process and dereference no caller memory. */
    error_t gate = proc_authorize_tag_grant(tags, ctx->proc);
    if (gate != OK) return gate;

    process_t *new_proc = process_create(tags);
    if (!new_proc) return ERR_SPAWN_FAILED;
    new_proc->cabin->spawner_pid = ctx->proc->pid;
    new_proc->cabin->spawner_gen = pid_generation(ctx->proc->pid);

    const uint8_t *elf = (const uint8_t *)vmm_phys_to_virt(binary_phys);
    if (binary_size < 16 || elf[0] != 0x7F || elf[1] != 'E' ||
        elf[2] != 'L' || elf[3] != 'F') {
        process_destroy(new_proc);
        return ERR_INVALID_ELF;
    }
    if (process_load_binary(new_proc, (void *)elf, (size_t)binary_size) != 0) {
        process_destroy(new_proc);
        return ERR_SPAWN_FAILED;
    }

    /* Publish process:spawned so monitors / shells can react. Pair with
     * the process:died event in TouchCleanupProcess — together they form
     * the lifecycle stream apps subscribe to instead of polling. */
    {
        struct __attribute__((packed)) {
            uint32_t pid;
            uint32_t parent_pid;
        } ev = { new_proc->pid, ctx->proc->pid };
        TouchPublish("process:spawned", &ev, sizeof(ev));
    }

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(uint32_t)) {
            uint32_t pid = new_proc->pid;
            (void)crate_write(out, ctx, &pid, sizeof(pid));
        }
    }
    return OK;
}

/* SYSTEM_OP_PROC_KILL
 *   params:  [u32 target_pid]            (0 == self exit; 4-byte form, code 0)
 *            [u32 target_pid][i32 code]  (optional 8-byte form; `code` is the
 *                                         self-exit disposition, ignored when
 *                                         killing another process)
 *   out_crate (optional): u32 killed_pid
 *
 * exit_code semantics (proc_exit.h): a self-exit publishes `code` masked to
 * [0, INT32_MAX]; killing another process forces PROC_EXIT_KILLED (-1). */
static int SysProcKill(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;

    uint32_t target_pid;
    memcpy(&target_pid, op->params, sizeof(uint32_t));

    /* Optional 8-byte form carries the self-exit code after the pid; the
     * 4-byte form (and any kill-other) defaults it to a clean 0. */
    int32_t exit_code = 0;
    if (op->param_size >= sizeof(uint32_t) + sizeof(int32_t))
        memcpy(&exit_code, (const uint8_t *)op->params + sizeof(uint32_t),
               sizeof(int32_t));

    bool self_exit = (target_pid == 0);
    if (self_exit) target_pid = ctx->proc->pid;

    if (target_pid == PROCESS_INVALID_PID) return ERR_INVALID_ARGUMENT;
    /* Pin target across the state transition + buffer cleanup so a
     * concurrent destroy on another core cannot recycle target_pid
     * mid-flight. */
    process_t *target = process_find_ref(target_pid);
    if (!target) return ERR_PROCESS_NOT_FOUND;

    /* Internal gate (the op stays OP_AUTH_NONE so every process can self-exit):
     * killing ANOTHER process needs authority over it — self-exit always passes
     * (caller->pid == target->pid); beyond self, a kill needs a same-cabin
     * sibling, the god/system ensign, or the exact child this caller spawned.
     * Without it any app could kill any process. */
    if (!proc_has_authority_over(ctx->proc, target)) {
        process_ref_dec(target);
        return ERR_ACCESS_DENIED;
    }

    /* Publish process:died with the CORRECT disposition and claim the cleanup
     * BEFORE process_set_state exposes this proc to the reaper. If we marked it
     * PROC_DONE/CRASHED first, the reaper's process_destroy (another K-Core)
     * could win the touch_cleaned claim in that window and publish
     * PROC_EXIT_CRASHED for an intentional exit/kill. TouchCleanupProcess reads
     * no proc state and `target` is pinned (process_find_ref above), so claiming
     * first is safe; the claim-guard in TouchClaimSet (touch_cleaned check under
     * subs_lock) already blocks any sub the still-running target might race in.
     * TouchCleanupProcess is idempotent on touch_cleaned, so the later
     * process_destroy call no-ops and this disposition stands.
     *
     * Disposition (proc_exit.h): a self-exit publishes the caller's code with
     * the sign bit cleared so it can never look like a negative sentinel; a
     * kill-other forces PROC_EXIT_KILLED — the victim never chose a code, so
     * any param code is ignored. */
    TouchCleanupProcess(target,
                        self_exit ? (int32_t)(exit_code & 0x7FFFFFFF)
                                  : PROC_EXIT_KILLED);

    process_set_state(target, self_exit ? PROC_DONE : PROC_CRASHED);
    __sync_synchronize();

    BufferRegistryCleanupProcess(target_pid);

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(uint32_t)) {
            (void)crate_write(out, ctx, &target_pid, sizeof(uint32_t));
        }
    }
    process_ref_dec(target);
    return OK;
}

/* SYSTEM_OP_TLS_FSBASE
 *   params: [u64 fsbase] — user-canonical VA (0 clears).
 *
 * Fallback for CPUs without FSGSBASE, where ring-3 WRFSBASE #UDs: store
 * the TLS thread pointer in ProcessContext and flip the MSR-restore gate.
 * Deliberately NO direct WRMSR here — Manifest ops may execute on a
 * K-Core, i.e. a different CPU than the caller; writing IA32_FS_BASE
 * there would program the wrong core. The value materializes on the
 * caller's next context restore — boxcxx follows the op with yield(),
 * making that deterministic before any thread_local access. */
static int SysTlsFsbase(const ManifestOp *op, Crate *crates,
                        uint16_t crate_count, const OpContext *ctx)
{
    (void)crates;
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint64_t)) return ERR_INVALID_ARGUMENT;

    uint64_t base;
    memcpy(&base, op->params, sizeof(base));

    /* User half + canonical only — a kernel-half FS base would let ring 3
     * read kernel memory through fs: overrides on the next switch-in. */
    if (base >= 0x0000800000000000ULL) return ERR_INVALID_ADDRESS;

    /*
     * ‼ RECORDED AS A DEBT, NOT WRITTEN INTO THE SAVED CONTEXT.
     *
     * This op runs in the guide loop on a K-Core — not the caller's CPU — so
     * it cannot program IA32_FS_BASE, and the branch that used to try was
     * guarded by process_get_current() == ctx->proc, which is never true here
     * (that reads the K-Core's own current process, and a K-Core is running
     * the guide loop, not a process). Dead code guarding a hazard.
     *
     * Nor may it write ProcessContext.user_fsbase: that field belongs to the
     * SAVE, which fills it from the live register. Writing it here made two
     * writers of one field, and the loser was this one — see the note in
     * process.h. The request goes somewhere the save never reads, and the
     * caller's next dispatch installs it.
     */
    /* The gate first, then the debt: the restore checks the gate before it
     * writes the MSR, and a core that saw the debt without the gate would
     * clear it having installed nothing. */
    g_user_fsbase_used = 1;
    ctx->proc->fsbase_wanted = base;
    __atomic_store_n(&ctx->proc->fsbase_owed, 1, __ATOMIC_RELEASE);

    return OK;
}

/* SYSTEM_OP_PROC_INFO
 *   params:  [u32 target_pid]   (0 == self)
 *   out_crate (>= 32 bytes): [u32 pid][u32 state][i32 score][u32 _pad]
 *                            [u64 code_start][u64 code_size]
 *                            [char tags[capacity-32]] */
/* SYSTEM_OP_PROC_CPUTIME
 *   params:    none
 *   out_crate (>= 8): [u64 processor microseconds used by the calling cabin]
 *
 * Self only, and that is the design rather than a limitation: there is no pid
 * parameter because another cabin's processor time is not this op's business,
 * and because a question about yourself needs no authority to check — which is
 * what makes OP_AUTH_NONE honest here rather than convenient. */
static int SysProcCpuTime(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

    /* Completed slices, plus the one running right now.
     *
     * The in-flight term is measured from cpu_tsc_stamp, and the base matters
     * more than the arithmetic. An earlier version used last_run_time and was
     * wrong twice over: schedule() re-stamps it on every pass, and it SURVIVES
     * A PARK, so the difference spanned a whole sleep and a 300 ms nap read as
     * 300 ms of processor time. cpu_tsc_stamp exists only while the strand
     * holds the core — set when it takes it, cleared when it leaves — so a
     * strand that is not running contributes nothing here, and one that is
     * gets credited to the cycle.
     *
     * Without this term the answer would only move at a context switch, and on
     * a machine with a spare core a strand can run a long time without one: a
     * 50 ms CPU-bound loop on 16 cores measured as zero. That is what this
     * paragraph is for.
     *
     * The read happens on the core the caller is running on, so the two TSC
     * values come from the same clock. */
    uint64_t us = __atomic_load_n(&ctx->proc->total_cpu_time, __ATOMIC_RELAXED);
    const uint64_t stamp = ctx->proc->cpu_tsc_stamp;
    if (stamp != 0) {
        const uint64_t khz = cpu_get_tsc_freq_khz();
        const uint64_t now = rdtsc();
        if (khz != 0 && now > stamp) us += ((now - stamp) * 1000ULL) / khz;
    }

    if (crate_write(out, ctx, &us, sizeof(us)) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

static int SysProcInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint32_t target_pid;
    memcpy(&target_pid, op->params, sizeof(uint32_t));
    if (target_pid == 0) target_pid = ctx->proc->pid;

    /* Pin target across all field reads + tag snapshot. */
    process_t *target = process_find_ref(target_pid);
    if (!target) return ERR_PROCESS_NOT_FOUND;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 32) {
        process_ref_dec(target);
        return ERR_BUFFER_TOO_SMALL;
    }

    /* out->capacity is attacker-controlled and unvalidated, and this op is
     * unauthenticated — bound the kernel bounce to one page (the 32-byte
     * header plus any realistic tag snapshot), never the claimed capacity, so
     * a giant capacity can't force a giant kmalloc+memset. */
    uint64_t alloc_sz = out->capacity > 4096u ? 4096u : out->capacity;
    uint8_t *kp = crate_out_alloc(out, alloc_sz);
    if (!kp) {
        process_ref_dec(target);
        return ERR_INVALID_ADDRESS;
    }

    uint32_t pid    = target->pid;
    uint32_t state  = (uint32_t)target->state;
    int32_t  score  = target->score;
    uint32_t pad    = 0;
    uint64_t cstart = target->cabin ? target->cabin->code_start : 0;
    uint64_t csize  = target->cabin ? target->cabin->code_size  : 0;

    memcpy(kp +  0, &pid,    sizeof(uint32_t));
    memcpy(kp +  4, &state,  sizeof(uint32_t));
    memcpy(kp +  8, &score,  sizeof(int32_t));
    memcpy(kp + 12, &pad,    sizeof(uint32_t));
    memcpy(kp + 16, &cstart, sizeof(uint64_t));
    memcpy(kp + 24, &csize,  sizeof(uint64_t));

    size_t   tag_room  = (size_t)alloc_sz - 32;
    size_t   copied    = process_snapshot_tags(target, (char *)(kp + 32), tag_room);
    uint64_t out_bytes = 32 + copied;
    if (out_bytes > alloc_sz) out_bytes = alloc_sz;

    int crc = crate_out_commit(out, ctx, kp, out_bytes);
    crate_buf_free(kp);
    process_ref_dec(target);
    if (crc != OK) return crc;
    out->size = out_bytes;
    return OK;
}

/* True iff comma-delimited `list` already contains exactly `token` (element-exact,
 * so "app" never matches inside "apple" or "touch_test"). */
static bool tag_list_contains(const char *list, const char *token)
{
    size_t tlen = strlen(token);
    const char *p = list;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t elen = comma ? (size_t)(comma - p) : strlen(p);
        if (elen == tlen && memcmp(p, token, tlen) == 0) return true;
        if (!comma) break;
        p = comma + 1;
    }
    return false;
}

/* Merge caller `augment` (comma-list) into `dst` (file-tags, already built).
 * Per token: reject if its KEY (before ':') is reserved -> ERR_ACCESS_DENIED;
 * skip if already present (dedup); else append ",token" or ERR_INVALID_ARGUMENT
 * on cap overflow. file-tags are never the casualty (trusted, built first). */
static error_t proc_exec_merge_augment(char *dst, size_t dst_size, const char *augment)
{
    const char *p = augment;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t tlen = comma ? (size_t)(comma - p) : strlen(p);
        if (tlen == 0) { if (!comma) break; p = comma + 1; continue; } /* skip empty token */

        char token[PROCESS_TAG_SIZE];
        if (tlen >= sizeof(token)) return ERR_INVALID_ARGUMENT;
        memcpy(token, p, tlen); token[tlen] = '\0';

        /* key = token up to ':' (value-bearing tags like "k:v") */
        char key[PROCESS_TAG_SIZE];
        const char *colon = strchr(token, ':');
        size_t klen = colon ? (size_t)(colon - token) : tlen;
        memcpy(key, token, klen); key[klen] = '\0';

        if (tagfs_key_is_reserved(key)) return ERR_ACCESS_DENIED;  /* fail-closed: no escalation */

        if (!tag_list_contains(dst, token)) {
            size_t cur = strlen(dst);
            size_t need = cur + (cur ? 1 : 0) + tlen + 1; /* comma + token + NUL */
            if (need > dst_size) return ERR_INVALID_ARGUMENT;
            if (cur) dst[cur++] = ',';
            memcpy(dst + cur, token, tlen); dst[cur + tlen] = '\0';
        }
        if (!comma) break;
        p = comma + 1;
    }
    return OK;
}

/* SYSTEM_OP_PROC_EXEC
 *   in_crate: filename
 *   params (optional): caller-tag augment (comma-list); child = file-tags ∪ augment
 *   out_crate (optional): u32 new_pid */
static int SysProcExec(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)               return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    if (process_get_count() >= PROCESS_MAX_COUNT) return ERR_PROCESS_LIMIT_EXCEEDED;

    char filename[64];
    error_t srcrc = sys_crate_string(&crates[op->in_crate], ctx, filename, sizeof(filename));
    if (srcrc != OK) return srcrc;

    /* Optional caller-tag augment rides in op->params (NUL-free, length-bounded).
     * Empty (param_size == 0) leaves augment "" so the merge below is a no-op and
     * this path stays byte-identical to a plain proc_exec. */
    char augment[PROCESS_TAG_SIZE];
    augment[0] = '\0';
    if (op->param_size > 0) {
        if (op->param_size >= sizeof(augment)) return ERR_INVALID_ARGUMENT;
        memcpy(augment, op->params, op->param_size);
        augment[op->param_size] = '\0';
    }

    /* Locate the file by tag-name + executable tag. */
    #define EXEC_SCAN_MAX 256
    uint32_t *file_ids = kmalloc(EXEC_SCAN_MAX * sizeof(uint32_t));
    if (!file_ids) return ERR_NO_MEMORY;

    int file_count = tagfs_list_all_files(file_ids, EXEC_SCAN_MAX);

    uint32_t found_id = 0;
    char     found_tags[PROCESS_TAG_SIZE];
    found_tags[0] = '\0';

    for (int i = 0; i < file_count; i++) {
        TagFSMetadata meta;
        if (tagfs_get_metadata(file_ids[i], &meta) != 0) continue;
        if (!(meta.flags & TAGFS_FILE_ACTIVE)) {
            tagfs_metadata_free(&meta);
            continue;
        }

        bool has_name = false, has_exec_tag = false;
        for (uint16_t t = 0; t < meta.tag_count; t++) {
            char key[128];
            if (!tagfs_tag_key(meta.tag_ids[t], key, sizeof(key))) continue;
            if (strcmp(key, filename) == 0)                      has_name = true;
            if (strcmp(key, "app") == 0 || strcmp(key, "utility") == 0) has_exec_tag = true;
        }

        if (has_name && has_exec_tag) {
            found_id = file_ids[i];
            size_t pos = 0;
            for (uint16_t t = 0; t < meta.tag_count; t++) {
                char key[128];
                if (!tagfs_tag_key(meta.tag_ids[t], key, sizeof(key))) continue;
                size_t klen = strlen(key);
                if (pos + klen + 2 > PROCESS_TAG_SIZE) break;
                if (pos > 0) found_tags[pos++] = ',';
                memcpy(found_tags + pos, key, klen);
                pos += klen;
            }
            found_tags[pos] = '\0';
            tagfs_metadata_free(&meta);
            break;
        }
        tagfs_metadata_free(&meta);
    }
    kfree(file_ids);

    if (found_id == 0) return ERR_FILE_NOT_FOUND;

    /* Fold the caller augment into found_tags BEFORE any binary I/O, so a
     * reserved/oversize augment costs zero disk reads and never spawns an
     * orphan. found_tags is already NUL-terminated by the build loop above. */
    error_t arc = proc_exec_merge_augment(found_tags, sizeof(found_tags), augment);
    if (arc != OK) return arc;

    TagFSMetadata exec_meta;
    if (tagfs_get_metadata(found_id, &exec_meta) != 0) return ERR_FILE_NOT_FOUND;
    uint64_t file_size = exec_meta.size;
    tagfs_metadata_free(&exec_meta);
    if (file_size == 0 || file_size > CONFIG_PROC_MAX_BINARY_SIZE) return ERR_BINARY_TOO_LARGE;

    size_t pages = (file_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void  *phys  = pmm_alloc_zero(pages);
    if (!phys) return ERR_NO_MEMORY;

    void *virt = vmm_phys_to_virt((uintptr_t)phys);

    TagFSFileHandle *fh = tagfs_open(found_id, TAGFS_HANDLE_READ);
    if (!fh) {
        pmm_free(phys, pages);
        return ERR_SPAWN_FAILED;
    }
    /* The whole image or none of it — see the same check in autostart.c. A
     * short read leaves the rest of the buffer as the zeros it was allocated
     * with, and a process started on those runs `add [rax], al` off the first
     * page and dies writing to address zero. */
    int rd = tagfs_read(fh, virt, file_size);
    tagfs_close(fh);
    if (rd < 0 || (uint64_t)rd != file_size) {
        kprintf("[Spawn] not starting file %u: %d of %llu bytes came back\n",
                found_id, rd, (unsigned long long)file_size);
        pmm_free(phys, pages);
        return ERR_SPAWN_FAILED;
    }

    process_t *new_proc = process_create(found_tags);
    if (!new_proc) {
        pmm_free(phys, pages);
        return ERR_SPAWN_FAILED;
    }
    new_proc->cabin->spawner_pid = ctx->proc->pid;
    new_proc->cabin->spawner_gen = pid_generation(ctx->proc->pid);

    int load = process_load_binary(new_proc, virt, (size_t)file_size);
    pmm_free(phys, pages);
    if (load != 0) {
        process_destroy(new_proc);
        return ERR_SPAWN_FAILED;
    }

    /* Snapshot the child's identity BEFORE process_set_state exposes it to the
     * scheduler/reaper: once PROC_WORKING the child may exit and be reaped on
     * another core, turning new_proc into a dangling read. The spawned event and
     * the out-crate below both use these locals, never new_proc, post-WORKING. */
    uint32_t child_pid = new_proc->pid;
    uint32_t child_gen = new_proc->generation;

    __sync_synchronize();
    process_set_state(new_proc, PROC_WORKING);

    /* Publish process:spawned (mirrors SysProcSpawn). proc_exec is the
     * primary userspace entry — without this hook subscribers see only
     * binary-from-memory spawns and miss every shell-invoked one. */
    {
        struct __attribute__((packed)) {
            uint32_t pid;
            uint32_t parent_pid;
        } ev = { child_pid, ctx->proc->pid };
        TouchPublish("process:spawned", &ev, sizeof(ev));
    }

    /* Out crate is capacity-gated: an 8-byte reader (proc_exec_gen) receives
     * {pid, generation}; a legacy 4-byte reader (proc_exec / proc_exec_tagged)
     * receives just the pid. Both stay correct. */
    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= 8) {
            uint32_t blob[2] = { child_pid, child_gen };
            (void)crate_write(out, ctx, blob, 8);
        } else if (out->capacity >= 4) {
            (void)crate_write(out, ctx, &child_pid, 4);
        }
    }
    return OK;
}

/* =========================================================================
 *  SYSTEM_OP_STRAND_SPAWN — spawn an additional strand in the caller's cabin
 *
 *  params: [u64 entry_va][u64 arg]   (16 bytes)
 *  out crate (optional): u32 new strand pid
 *
 *  Creates a second+ execution context that shares the caller's address
 *  space (CR3 / rings / tags / heap) — the kernel substrate for
 *  std::thread.  The strand begins at entry_va with `arg` in rdi (System V
 *  first argument); userspace passes a trampoline that runs the thread
 *  function then terminates the strand.  OP_AUTH_APP: a cabin may always
 *  spawn strands into itself (no new address space is created, unlike
 *  proc.spawn), so this needs no elevated capability.
 * ========================================================================= */
static int SysStrandSpawn(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc || !ctx->proc->cabin) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 16) return ERR_INVALID_ARGUMENT;

    uint64_t entry_va, arg;
    memcpy(&entry_va, op->params,     sizeof(uint64_t));
    memcpy(&arg,      op->params + 8, sizeof(uint64_t));

    /* Optional 3rd param: joinable (low byte of a u64). std::thread passes 1 so
     * the strand is zombie-until-join (pid held until SYSTEM_OP_STRAND_RELEASE);
     * a raw strand_spawn worker omits it (param_size==16) and spawns joinable=0
     * (eager reap, as before). */
    uint8_t joinable = (op->param_size >= 17) ? op->params[16] : 0u;

    process_t *strand = strand_spawn(ctx->proc->cabin, (uintptr_t)entry_va, arg,
                                     joinable != 0);
    if (!strand) return ERR_SPAWN_FAILED;

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(uint32_t)) {
            uint32_t pid = strand->pid;
            (void)crate_write(out, ctx, &pid, sizeof(pid));
        }
    }

    return OK;
}

/* =========================================================================
 *  SYSTEM_OP_STRAND_RELEASE — clear a joinable strand's reap-block so the P5b
 *  reaper may reclaim it. Called by std::thread join()/detach() once the strand
 *  is done being referenced as an id. Cabin-scoped: a cabin may release only its
 *  OWN strands. Idempotent and safe on an unknown / already-released / non-
 *  joinable pid (no-op) — so a benign double call or a race with the reaper
 *  cannot corrupt anything.
 * ========================================================================= */
static int SysStrandRelease(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                            const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc || !ctx->proc->cabin) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t))        return ERR_INVALID_ARGUMENT;

    uint32_t pid;
    memcpy(&pid, op->params, sizeof(uint32_t));

    process_t *target = process_find_ref(pid);
    if (!target) return OK;   /* already reaped / never existed — nothing to do */

    /* Only release a strand of THIS cabin (a cabin cannot poke siblings'
     * cabins' strands). Clearing reap_blocked lets the reaper reclaim it on the
     * next tick (it is, or will become, PROC_DONE). */
    if (target->cabin == ctx->proc->cabin)
        __atomic_store_n(&target->reap_blocked, 0u, __ATOMIC_SEQ_CST);

    process_ref_dec(target);
    return OK;
}

/* =========================================================================
 *  SYSTEM_OP_STRAND_POOL_BIND — register the caller strand's boxlib StrandPool
 *  slab slot for crash-orphan reclaim (Ф20e).
 *
 *  params: [u64 pool_va][u32 gen]   (12 bytes)
 *
 *  boxlib calls this once, right after it claims a slab slot and writes the
 *  slot (so the page is present and resolvable). We validate that the VA is in
 *  the caller's user range, StrandPool-aligned, and currently mapped, then stash
 *  the VA (not a phys) plus the bound generation on the process_t. process_destroy
 *  RE-RESOLVES the VA through the live cabin page tables at death, so a page that
 *  was unmapped/recycled between bind and death can never make us stamp a phys
 *  that now belongs to a different cabin. There is no unbind op: an orderly flush
 *  bumps the generation, which makes the death-stamp CAS miss. OP_AUTH_APP — a
 *  strand only ever binds a pool inside its own cabin. */
static int SysStrandPoolBind(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                             const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc || !ctx->proc->cabin || !ctx->proc->cabin->vmm)
        return ERR_INVALID_ARGUMENT;
    if (op->param_size < 20) return ERR_INVALID_ARGUMENT;

    uint64_t pool_va;
    uint32_t gen;
    uint64_t orphan_pending_va;
    memcpy(&pool_va,           op->params,      sizeof(uint64_t));
    memcpy(&gen,               op->params + 8,  sizeof(uint32_t));
    memcpy(&orphan_pending_va, op->params + 12, sizeof(uint64_t));

    if (pool_va <= CABIN_NULL_TRAP_END || pool_va >= CABIN_USER_VA_CANONICAL_END)
        return ERR_INVALID_ARGUMENT;
    if (pool_va % _Alignof(StrandPool) != 0)
        return ERR_INVALID_ARGUMENT;
    if (vmm_virt_to_phys(ctx->proc->cabin->vmm, (uintptr_t)pool_va) == 0)
        return ERR_INVALID_ADDRESS;

    if (orphan_pending_va <= CABIN_NULL_TRAP_END || orphan_pending_va >= CABIN_USER_VA_CANONICAL_END)
        return ERR_INVALID_ARGUMENT;
    if (orphan_pending_va % 4 != 0)
        return ERR_INVALID_ARGUMENT;
    if (vmm_virt_to_phys(ctx->proc->cabin->vmm, (uintptr_t)orphan_pending_va) == 0)
        return ERR_INVALID_ADDRESS;

    ctx->proc->strand_pool_va         = pool_va;
    ctx->proc->strand_pool_gen        = gen;
    ctx->proc->strand_pool_orphan_va  = orphan_pending_va;
    return OK;
}

/* =========================================================================
 *  Context (use)
 * ========================================================================= */

static int ctx_use_parse(const char *input, char tags[][CTX_TAG_LENGTH],
                         uint32_t *count_out)
{
    *count_out = 0;
    if (input[0] == '\0') return OK;

    char buf[CTX_USE_PARSE_BUF];
    size_t ilen = strlen(input);
    if (ilen >= sizeof(buf)) return ERR_INVALID_ARGUMENT;
    memcpy(buf, input, ilen + 1);

    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok) {
        while (*tok == ' ' || *tok == '\t') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) *(--end) = '\0';
        if (*tok == '\0') { tok = strtok_r(NULL, ",", &saveptr); continue; }

        if (*count_out >= MAX_CTX_USE_TAGS)        return ERR_TAG_LIMIT_EXCEEDED;
        if (strlen(tok) >= CTX_TAG_LENGTH)         return ERR_INVALID_ARGUMENT;

        memcpy(tags[*count_out], tok, strlen(tok) + 1);
        (*count_out)++;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    return OK;
}

/* SYSTEM_OP_CTX_USE  in_crate: context string (NUL-bounded).
 * Empty string clears the current context. */
static int SysCtxUse(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)               return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    char input[CTX_USE_PARSE_BUF];
    Crate *src = &crates[op->in_crate];
    if (src->size == 0) {
        UseContextClear();
        return OK;
    }
    if (src->size >= sizeof(input)) return ERR_INVALID_ARGUMENT;
    error_t read_rc = crate_read(src, ctx, input, src->size);
    if (read_rc != OK) return read_rc;
    input[src->size] = '\0';

    char     parsed[MAX_CTX_USE_TAGS][CTX_TAG_LENGTH];
    uint32_t count = 0;
    memset(parsed, 0, sizeof(parsed));

    int parse_rc = ctx_use_parse(input, parsed, &count);
    if (parse_rc != OK) return parse_rc;

    if (count == 0) {
        UseContextClear();
        return OK;
    }

    const char *ptrs[MAX_CTX_USE_TAGS];
    for (uint32_t i = 0; i < count; i++) ptrs[i] = parsed[i];
    return UseContextSet(ptrs, count);
}

/* =========================================================================
 *  Buffers
 * ========================================================================= */

/* SYSTEM_OP_BUF_ALLOC
 *   params:  [u64 size][u32 flags]    (12 bytes; flags ignored for now)
 *   out_crate (32 bytes): [u64 handle][u64 phys][u64 actual][u64 virt] */
static int SysBufAlloc(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint64_t)) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint64_t size;
    memcpy(&size, op->params, sizeof(uint64_t));

    BufferAllocResult r = BufferRegistryAlloc(ctx->proc, size);
    if (r.err != OK) return r.err;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 32) return ERR_BUFFER_TOO_SMALL;

    uint8_t blob[32];
    memcpy(blob +  0, &r.handle,      sizeof(uint64_t));
    memcpy(blob +  8, &r.phys_addr,   sizeof(uint64_t));
    memcpy(blob + 16, &r.actual_size, sizeof(uint64_t));
    memcpy(blob + 24, &r.virt_addr,   sizeof(uint64_t));
    if (crate_write(out, ctx, blob, 32) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* SYSTEM_OP_BUF_FREE  params:[u64 handle] */
static int SysBufFree(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                      const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint64_t)) return ERR_INVALID_ARGUMENT;

    uint64_t handle;
    memcpy(&handle, op->params, sizeof(uint64_t));
    return BufferRegistryFree(ctx->proc->pid, handle);
}

/* SYSTEM_OP_BUF_RESIZE
 *   params:  [u64 handle][u64 new_size]
 *   out_crate (16 bytes): [u64 handle][u64 actual_size] */
static int SysBufResize(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 16) return ERR_INVALID_ARGUMENT;

    uint64_t handle, new_size;
    memcpy(&handle,   op->params,     sizeof(uint64_t));
    memcpy(&new_size, op->params + 8, sizeof(uint64_t));

    uint64_t actual = 0;
    error_t  rc = BufferRegistryResize(ctx->proc, handle, new_size, &actual, NULL);
    if (rc != OK) return rc;

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= 16) {
            uint8_t blob[16];
            memcpy(blob,     &handle, sizeof(uint64_t));
            memcpy(blob + 8, &actual, sizeof(uint64_t));
            (void)crate_write(out, ctx, blob, 16);
        }
    }
    return OK;
}

/* =========================================================================
 *  Tags
 * ========================================================================= */

/* On OK, *out_target carries a pinned reference; the caller MUST
 * release it via process_ref_dec when done. On error, no ref is held. */
static error_t sys_tag_target(const ManifestOp *op, Crate *crates,
                              const OpContext *ctx,
                              process_t **out_target,
                              char *tag_out, size_t tag_out_size)
{
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE)  return ERR_INVALID_ARGUMENT;

    uint32_t target_pid;
    memcpy(&target_pid, op->params, sizeof(uint32_t));
    if (target_pid == PROCESS_INVALID_PID) return ERR_INVALID_ARGUMENT;
    if (target_pid == 0) target_pid = ctx->proc->pid;

    process_t *t = process_find_ref(target_pid);
    if (!t) return ERR_PROCESS_NOT_FOUND;

    error_t srcrc = sys_crate_string(&crates[op->in_crate], ctx, tag_out, tag_out_size);
    if (srcrc != OK) {
        process_ref_dec(t);
        return srcrc;
    }

    *out_target = t;
    return OK;
}

static int SysTagAdd(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    process_t *target = NULL;
    char tag[64];
    error_t rc = sys_tag_target(op, crates, ctx, &target, tag, sizeof(tag));
    if (rc != OK) return rc;

    /* Mutating another process's tags needs authority over it (self / same-cabin
     * / god|system / own child); without this any app could freeze or
     * de-privilege any process by pid. */
    if (!proc_has_authority_over(ctx->proc, target)) {
        process_ref_dec(target);
        return ERR_ACCESS_DENIED;
    }
    /* And the granted tag must not exceed the CALLER's own authority — reuse the
     * spawn grant gate so an app cannot self-grant "god"/"system" (escalation).
     * Non-privilege tags (auth bit 0) and "stopped" pass freely. */
    error_t grant = proc_authorize_tag_grant(tag, ctx->proc);
    if (grant != OK) {
        process_ref_dec(target);
        return grant;
    }

    int result = OK;
    if (process_has_tag(target, tag)) {
        result = ERR_ALREADY_EXISTS;
    } else if (process_add_tag(target, tag) != 0) {
        result = ERR_TAG_LIMIT_EXCEEDED;
    } else if (strcmp(tag, "stopped") == 0) {
        process_state_t s = process_get_state(target);
        if (s == PROC_WORKING || s == PROC_CREATED) {
            process_set_state(target, PROC_STOPPED);
        }
    }
    process_ref_dec(target);
    return result;
}

static int SysTagRemove(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    process_t *target = NULL;
    char tag[64];
    error_t rc = sys_tag_target(op, crates, ctx, &target, tag, sizeof(tag));
    if (rc != OK) return rc;

    /* Same authority gate as tag.add: only self / same-cabin / god|system /
     * own-child may strip a target's tags (no grant gate — dropping a tag never
     * escalates). */
    if (!proc_has_authority_over(ctx->proc, target)) {
        process_ref_dec(target);
        return ERR_ACCESS_DENIED;
    }

    int result = OK;
    if (!process_has_tag(target, tag)) {
        result = ERR_TAG_NOT_FOUND;
    } else if (process_remove_tag(target, tag) != 0) {
        result = ERR_INVALID_ARGUMENT;
    } else if (strcmp(tag, "stopped") == 0) {
        if (process_get_state(target) == PROC_STOPPED) {
            process_set_state(target, PROC_WORKING);
        }
    }
    process_ref_dec(target);
    return result;
}

/* SYSTEM_OP_TAG_CHECK   out_crate: u8 has_tag (0/1) */
static int SysTagCheck(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    process_t *target = NULL;
    char tag[64];
    error_t rc = sys_tag_target(op, crates, ctx, &target, tag, sizeof(tag));
    if (rc != OK) return rc;

    bool has = process_has_tag(target, tag);

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= 1) {
            uint8_t v = has ? 1 : 0;
            (void)crate_write(out, ctx, &v, 1);
        }
    }
    process_ref_dec(target);
    return OK;
}

/* =========================================================================
 *  Filesystem maintenance
 * ========================================================================= */

/* SYSTEM_OP_DEFRAG_FILE  params:[u32 file_id][u32 target_block]
 *                        out_crate (optional): u32 frag_score */
static int SysDefragFile(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    uint32_t file_id, target_block;
    memcpy(&file_id,      op->params,     sizeof(uint32_t));
    memcpy(&target_block, op->params + 4, sizeof(uint32_t));

    int rc = tagfs_defrag_file(file_id, target_block);
    if (rc != 0) return ERR_INVALID_ARGUMENT;

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(uint32_t)) {
            uint32_t score = tagfs_get_fragmentation_score();
            (void)crate_write(out, ctx, &score, sizeof(uint32_t));
        }
    }
    return OK;
}

/* SYSTEM_OP_FRAG_SCORE  out_crate:[u32 score][u32 total_files][u32 total_gaps] */
static int SysFragScore(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 12) return ERR_BUFFER_TOO_SMALL;

    uint32_t score = tagfs_get_fragmentation_score();
    uint32_t total_files = 0;
    uint32_t total_gaps  = 0;

    TagFSState *fs = tagfs_get_state();
    if (fs && fs->initialized) {
        uint32_t max_id = fs->ledger.next_file_id;
        for (uint32_t i = 1; i < max_id; i++) {
            TagFSMetadata meta;
            if (tagfs_get_metadata(i, &meta) == 0) {
                if (meta.flags & TAGFS_FILE_ACTIVE) total_files++;
                tagfs_metadata_free(&meta);
            }
        }
    }

    uint8_t blob[12];
    memcpy(blob +  0, &score,        sizeof(uint32_t));
    memcpy(blob +  4, &total_files,  sizeof(uint32_t));
    memcpy(blob +  8, &total_gaps,   sizeof(uint32_t));
    if (crate_write(out, ctx, blob, 12) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  System info — real-time snapshot (no hardcoded values)
 *
 *  Layout written into out_crate (must be ≥ 96 bytes, matches userspace
 *  system_info_t):
 *
 *    [00..31]  char     version[32]
 *    [32..39]  uint64_t uptime_ns
 *    [40..47]  uint64_t total_memory
 *    [48..55]  uint64_t used_memory
 *    [56..63]  uint64_t free_memory
 *    [64..71]  uint64_t tsc_freq_khz
 *    [72..75]  uint32_t cpu_total
 *    [76..79]  uint32_t cpu_k_cores
 *    [80..83]  uint32_t cpu_app_cores
 *    [84..87]  uint32_t process_count
 *    [88..91]  uint32_t pit_freq_hz
 *    [92]      uint8_t  multicore_active
 *    [93]      uint8_t  has_invariant_tsc
 *    [94]      uint8_t  has_waitpkg
 *    [95]      uint8_t  reserved
 * ========================================================================= */

#define SYSINFO_BLOB_SIZE  96

static int SysInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                   const OpContext *ctx)
{
    (void)op; (void)crate_count;
    if (!ctx)                              return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < SYSINFO_BLOB_SIZE) return ERR_BUFFER_TOO_SMALL;

    uint8_t blob[SYSINFO_BLOB_SIZE];

    /* Version string. Pinned here for now (kernel_config.h has no version
     * macro yet); migrate to a single source when the version policy lands. */
    static const char kver[] = "BoxOS v0.2.0";
    size_t vlen = sizeof(kver) - 1;
    if (vlen > 31) vlen = 31;
    memset(blob, 0, 32);
    memcpy(blob, kver, vlen);

    /* Memory in bytes — PMM tracks pages; convert with PAGE_SIZE. */
    uint64_t total_pages = (uint64_t)pmm_total_pages();
    uint64_t used_pages  = (uint64_t)pmm_used_pages();
    uint64_t free_pages  = total_pages > used_pages ? total_pages - used_pages : 0;
    uint64_t total_b     = total_pages * PMM_PAGE_SIZE;
    uint64_t used_b      = used_pages  * PMM_PAGE_SIZE;
    uint64_t free_b      = free_pages  * PMM_PAGE_SIZE;

    uint64_t uptime_ns   = rtc_get_uptime_ns();
    uint64_t tsc_khz     = cpu_get_tsc_freq_khz();

    uint32_t cpu_total   = (uint32_t)g_amp.total_cores;
    uint32_t cpu_k       = (uint32_t)g_amp.k_count;
    uint32_t cpu_app     = (uint32_t)g_amp.app_count;
    uint32_t proc_count  = process_get_count();
    uint32_t pit_hz      = pit_get_frequency();

    uint8_t mc_active    = g_amp.multicore_active   ? 1 : 0;
    uint8_t inv_tsc      = g_cpu_caps.has_invariant_tsc ? 1 : 0;
    uint8_t waitpkg      = g_cpu_caps.has_waitpkg       ? 1 : 0;

    memcpy(blob + 32, &uptime_ns, sizeof(uint64_t));
    memcpy(blob + 40, &total_b,   sizeof(uint64_t));
    memcpy(blob + 48, &used_b,    sizeof(uint64_t));
    memcpy(blob + 56, &free_b,    sizeof(uint64_t));
    memcpy(blob + 64, &tsc_khz,   sizeof(uint64_t));
    memcpy(blob + 72, &cpu_total, sizeof(uint32_t));
    memcpy(blob + 76, &cpu_k,     sizeof(uint32_t));
    memcpy(blob + 80, &cpu_app,   sizeof(uint32_t));
    memcpy(blob + 84, &proc_count,sizeof(uint32_t));
    memcpy(blob + 88, &pit_hz,    sizeof(uint32_t));
    blob[92] = mc_active;
    blob[93] = inv_tsc;
    blob[94] = waitpkg;
    blob[95] = 0;

    if (crate_write(out, ctx, blob, SYSINFO_BLOB_SIZE) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  Telemetry
 * ========================================================================= */

static int SysPerfDump(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    perf_dump();
    ManifestStageDumpAll();
    {
        /* Dispatch transport split — the runtime witness that enclosed
         * Manifests actually ride in their envelopes (guide.c). */
        uint64_t d[2];
        guide_dispatch_stats(d);
        kprintf("[GUIDE] dispatches: enclosed=%lu addressed=%lu\n",
                (unsigned long)d[0], (unsigned long)d[1]);
    }
    return OK;
}

/* =========================================================================
 *  Manifest compile-and-reuse — "prepared statement" pattern.
 *
 *  COMPILE:  in_crate  = raw Manifest bytes (any size up to MANIFEST_RAW_MAX_SIZE)
 *            out_crate = uint64 handle (capacity ≥ 8)
 *
 *            The kernel copies the raw bytes via ManifestCompile (page-walked
 *            user-PT copy is built into ManifestCompile's is_kernel_ptr=false
 *            path), validates each op, resolves and caches per-op handler
 *            pointers, allocates a slot in the global ManifestTable, and
 *            returns a (gen << 32 | slot) handle. Ownership is recorded
 *            against ctx->proc->pid so process_destroy can auto-release
 *            any leaked handles.
 *
 *  RELEASE:  in_crate  = uint64 handle (size == 8)
 *
 *            Decrements the handle's refcount. On 0 the CompiledManifest is
 *            freed. Ownership check: only the cabin that compiled the
 *            handle may release it (ERR_ACCESS_DENIED otherwise). Concurrent
 *            ManifestExecute on the same handle stays safe — its internal
 *            Resolve holds the form alive past this Release.
 * ========================================================================= */

static int SysManifestCompile(const ManifestOp *op, Crate *crates,
                               uint16_t crate_count, const OpContext *ctx)
{
    if (!ctx || !ctx->proc || !ctx->proc->cabin) return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE)        return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE)       return ERR_INVALID_ARGUMENT;
    if (op->in_crate >= crate_count || op->out_crate >= crate_count)
        return ERR_INVALID_ARGUMENT;

    Crate *in  = &crates[op->in_crate];
    Crate *out = &crates[op->out_crate];
    if (!CrateIsValid(in) || !CrateIsValid(out)) return ERR_INVALID_BUFFER_ID;
    if (in->size == 0 || in->size > MANIFEST_RAW_MAX_SIZE)
        return ERR_INVALID_ARGUMENT;
    if (out->capacity < sizeof(ManifestHandle))  return ERR_BUFFER_TOO_SMALL;

    ManifestHandle handle = MANIFEST_HANDLE_INVALID;
    error_t rc = ManifestCompile(ctx->proc,
                                  (const void *)(uintptr_t)in->addr,
                                  (uint32_t)in->size,
                                  /*is_kernel_ptr=*/false,
                                  &handle);
    if (rc != OK) return (int)rc;

    /* Publish handle into the user's out crate payload via page-walked
     * commit_out. The Crate descriptor itself (out->size) is mutated in
     * the staged kbuf and write-back by guide.c's commit-and-release. */
    error_t commit_rc = vmm_user_buf_commit_out(ctx->proc->cabin->vmm,
                                                 (uintptr_t)out->addr,
                                                 &handle,
                                                 sizeof(handle));
    if (commit_rc != OK) {
        /* Roll back the compile — userspace will never see this handle. */
        ManifestRelease(handle);
        return (int)commit_rc;
    }
    out->size = sizeof(handle);
    return OK;
}

static int SysManifestRelease(const ManifestOp *op, Crate *crates,
                               uint16_t crate_count, const OpContext *ctx)
{
    if (!ctx || !ctx->proc || !ctx->proc->cabin) return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE)        return ERR_INVALID_ARGUMENT;
    if (op->in_crate >= crate_count)             return ERR_INVALID_ARGUMENT;

    Crate *in = &crates[op->in_crate];
    if (!CrateIsValid(in))                       return ERR_INVALID_BUFFER_ID;
    if (in->size != sizeof(ManifestHandle))      return ERR_INVALID_ARGUMENT;

    /* Read the handle out of the user payload via the page-walked snapshot —
     * an 8-byte read still straddles when it sits within 7 bytes of a page
     * end, so the single-page map cannot be assumed safe. */
    ManifestHandle handle;
    error_t read_rc = crate_read(in, ctx, &handle, sizeof(handle));
    if (read_rc != OK) return read_rc;

    /* Ownership check via Resolve (which pins the form, preventing concurrent
     * free during the verification). Balance with one Release. */
    CompiledManifest *cm = ManifestResolve(handle);
    if (!cm) return ERR_INVALID_ARGUMENT;
    if (cm->owner_pid != ctx->proc->pid) {
        ManifestRelease(handle);
        return ERR_ACCESS_DENIED;
    }
    /* Drop the Resolve's ref. */
    ManifestRelease(handle);
    /* Drop the user's initial ref (from compile). Concurrent Execute holds
     * its own Resolve ref so the form survives until that completes. */
    return (int)ManifestRelease(handle);
}

/* =========================================================================
 *  EFI runtime / Secure Boot / ESRT — read-only introspection ops.
 *
 *  Replaces the previous "subscribe to secureboot:on/off at boot" surface
 *  with a synchronous query so userspace can poll state on demand. The
 *  kernel-side state is cached in efi_secureboot.c / efi_esrt.c; these
 *  handlers just marshal it into out_crate.
 * ========================================================================= */

#include "efi.h"
#include "efi_esrt.h"
#include "efi_secureboot.h"
#include "efi_authenticode.h"

static int SysEfiInfo(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx)                              return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < EFI_INFO_BLOB_SIZE) return ERR_BUFFER_TOO_SMALL;
    uint8_t blob[EFI_INFO_BLOB_SIZE];
    memset(blob, 0, EFI_INFO_BLOB_SIZE);

    EfiSecureBootState sb = {0};
    efi_secureboot_get_state(&sb);

    uint32_t version       = EFI_INFO_VERSION;
    uint8_t  rt_available  = efi_runtime_available() ? 1 : 0;
    uint8_t  esrt_available= efi_esrt_available()    ? 1 : 0;
    uint8_t  sb_available  = sb.available            ? 1 : 0;
    uint8_t  sb_enforced   = sb.enforced             ? 1 : 0;
    uint8_t  sb_setup      = sb.setup_mode           ? 1 : 0;
    uint8_t  sb_audit      = sb.audit_mode           ? 1 : 0;
    uint8_t  sb_deployed   = sb.deployed_mode        ? 1 : 0;
    uint32_t esrt_count    = efi_esrt_count();
    uint32_t cert_count    = sb.cert_count;
    uint32_t hash_count    = sb.hash_count;

    /* Layout (offset-aligned, packed):
     *   +0   u32 version
     *   +4   u8  rt_available
     *   +5   u8  esrt_available
     *   +6   u8  sb_available
     *   +7   u8  sb_enforced
     *   +8   u8  sb_setup_mode
     *   +9   u8  sb_audit_mode
     *   +10  u8  sb_deployed_mode
     *   +11  u8  _pad
     *   +12  u32 esrt_count
     *   +16  u32 cert_count_total
     *   +20  u32 hash_count_total
     *   +24  u32 cert_count_by_db[6]   (PK,KEK,db,dbx,dbt,dbr)
     *   +48  u32 hash_count_by_db[6]
     *   +72  56  reserved (zero) */
    memcpy(blob + 0,  &version,        4);
    blob[4]  = rt_available;
    blob[5]  = esrt_available;
    blob[6]  = sb_available;
    blob[7]  = sb_enforced;
    blob[8]  = sb_setup;
    blob[9]  = sb_audit;
    blob[10] = sb_deployed;
    memcpy(blob + 12, &esrt_count,    4);
    memcpy(blob + 16, &cert_count,    4);
    memcpy(blob + 20, &hash_count,    4);
    memcpy(blob + 24, sb.cert_count_by_db, 6 * 4);
    memcpy(blob + 48, sb.hash_count_by_db, 6 * 4);

    if (crate_write(out, ctx, blob, EFI_INFO_BLOB_SIZE) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* SYSTEM_OP_EFI_ESRT_GET — fetch one EFI_SYSTEM_RESOURCE_ENTRY by index.
 * Index travels in op->params as a u32; entry written to out_crate. */
static int SysEfiEsrtGet(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx)                              return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;

    uint32_t idx;
    memcpy(&idx, op->params, sizeof(idx));

    const EfiSystemResourceEntry *e = efi_esrt_get(idx);
    if (!e) return ERR_OUT_OF_RANGE;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(EfiSystemResourceEntry)) return ERR_BUFFER_TOO_SMALL;
    if (crate_write(out, ctx, e, sizeof(EfiSystemResourceEntry)) != OK)
        return ERR_INVALID_ADDRESS;
    return OK;
}

/* SYSTEM_OP_EFI_VERIFY_PE — verify an Authenticode-signed PE against
 * the platform's db/dbx. in_crate carries the entire PE image; out_crate
 * receives a 80-byte blob: { u32 result, u32 pe_size,
 *                             u8 pe_sha256[32], u8 signer_sha256[32],
 *                             u8 _pad[8] }. */
#define EFI_VERIFY_PE_OUT_SIZE  80u
static int SysEfiVerifyPe(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                           const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx)                              return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE)  return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *in  = &crates[op->in_crate];
    Crate *out = &crates[op->out_crate];
    if (in->size == 0)                     return ERR_INVALID_ARGUMENT;
    if (in->size > (16u * 1024u * 1024u))  return ERR_INVALID_ARGUMENT;  /* 16 MB cap */
    if (out->capacity < EFI_VERIFY_PE_OUT_SIZE) return ERR_BUFFER_TOO_SMALL;

    /* Bounce the whole PE through a page-walked kernel buffer. The image is
     * always multi-page, so the old single-page map fed efi_authenticode a
     * pointer that read a foreign frame past the first page — wrong hashes
     * and a stray-frame read. */
    uint8_t *pe = crate_in_buf(in, ctx);
    if (!pe) return ERR_INVALID_ADDRESS;

    uint8_t pe_hash[32]     = {0};
    uint8_t signer_hash[32] = {0};
    EfiAuthenticodeResult r = efi_authenticode_verify_pe(
        pe, (uint32_t)in->size, pe_hash, signer_hash);
    crate_buf_free(pe);

    uint8_t blob[EFI_VERIFY_PE_OUT_SIZE];
    memset(blob, 0, EFI_VERIFY_PE_OUT_SIZE);
    uint32_t result = (uint32_t)r;
    uint32_t pe_size = (uint32_t)in->size;
    memcpy(blob + 0, &result,      4);
    memcpy(blob + 4, &pe_size,     4);
    memcpy(blob + 8, pe_hash,      32);
    memcpy(blob + 40, signer_hash, 32);
    if (crate_write(out, ctx, blob, EFI_VERIFY_PE_OUT_SIZE) != OK) return ERR_INVALID_ADDRESS;
    return OK;
}

/* =========================================================================
 *  Registration
 * ========================================================================= */

error_t SystemDeckRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        /* IPC: app+ — IPC is the lifeblood of any process. */
        { SYSTEM_OP_ROUTE,        SysRoute,       OP_AUTH_APP,    "system.route"      },
        { SYSTEM_OP_ROUTE_TAG,    SysBroadcast,   OP_AUTH_APP,    "system.broadcast"  },
        /* Process lifecycle: spawn/exec are utility+; info/kill/exit are open
         * (kill of pid==0 is self-exit, used by every process). */
        { SYSTEM_OP_PROC_SPAWN,   SysProcSpawn,   OP_AUTH_UTILITY,"system.proc.spawn" },
        { SYSTEM_OP_PROC_KILL,    SysProcKill,    OP_AUTH_NONE,   "system.proc.kill"  },
        { SYSTEM_OP_PROC_INFO,    SysProcInfo,    OP_AUTH_NONE,   "system.proc.info"  },
        { SYSTEM_OP_PROC_CPUTIME, SysProcCpuTime, OP_AUTH_NONE,   "system.proc.cputime"},
        { SYSTEM_OP_TLS_FSBASE,   SysTlsFsbase,   OP_AUTH_NONE,   "system.tls.fsbase" },
        { SYSTEM_OP_PROC_EXEC,    SysProcExec,    OP_AUTH_UTILITY,"system.proc.exec"  },
        { SYSTEM_OP_STRAND_SPAWN, SysStrandSpawn, OP_AUTH_APP,    "system.strand.spawn"},
        { SYSTEM_OP_STRAND_RELEASE, SysStrandRelease, OP_AUTH_APP, "system.strand.release"},
        { SYSTEM_OP_STRAND_POOL_BIND, SysStrandPoolBind, OP_AUTH_APP, "system.strand.pool.bind"},
        { SYSTEM_OP_INFO,         SysInfo,        OP_AUTH_NONE,   "system.info"       },
        /* Context, tags, buffers: app+. */
        { SYSTEM_OP_CTX_USE,      SysCtxUse,      OP_AUTH_APP,    "system.ctx.use"    },
        { SYSTEM_OP_BUF_ALLOC,    SysBufAlloc,    OP_AUTH_APP,    "system.buf.alloc"  },
        { SYSTEM_OP_BUF_FREE,     SysBufFree,     OP_AUTH_APP,    "system.buf.free"   },
        { SYSTEM_OP_BUF_RESIZE,   SysBufResize,   OP_AUTH_APP,    "system.buf.resize" },
        { SYSTEM_OP_TAG_ADD,      SysTagAdd,      OP_AUTH_APP,    "system.tag.add"    },
        { SYSTEM_OP_TAG_REMOVE,   SysTagRemove,   OP_AUTH_APP,    "system.tag.remove" },
        { SYSTEM_OP_TAG_CHECK,    SysTagCheck,    OP_AUTH_NONE,   "system.tag.check"  },
        /* FS maintenance: utility+. */
        { SYSTEM_OP_DEFRAG_FILE,  SysDefragFile,  OP_AUTH_UTILITY,"system.fs.defrag"  },
        { SYSTEM_OP_FRAG_SCORE,   SysFragScore,   OP_AUTH_NONE,   "system.fs.score"   },
        /* Telemetry: admin only. */
        { SYSTEM_OP_PERF_DUMP,    SysPerfDump,    OP_AUTH_SYSTEM, "system.perf.dump"  },

        /* Manifest compile-and-reuse — prepared-statement pattern. */
        { SYSTEM_OP_MANIFEST_COMPILE, SysManifestCompile, OP_AUTH_APP,
          "system.manifest.compile" },
        { SYSTEM_OP_MANIFEST_RELEASE, SysManifestRelease, OP_AUTH_APP,
          "system.manifest.release" },
        /* EFI introspection: open to any process (read-only state). */
        { SYSTEM_OP_EFI_INFO,     SysEfiInfo,     OP_AUTH_NONE,   "system.efi.info"   },
        { SYSTEM_OP_EFI_ESRT_GET, SysEfiEsrtGet,  OP_AUTH_NONE,   "system.efi.esrt"   },
        /* PE verification gated to utility-or-better — it can be expensive
         * (RSA modexp + SHA-256 stream) so we keep it out of the unauth
         * lane to prevent a non-app process from DOSing the kernel with
         * bogus PE buffers. */
        { SYSTEM_OP_EFI_VERIFY_PE,SysEfiVerifyPe, OP_AUTH_UTILITY,"system.efi.verifype"},
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_SYSTEM, table[i].opcode),
                                        table[i].handler,
                                        table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[SystemDeck] register %s failed: %s\n",
                    table[i].name, ErrorString(rc));
            return rc;
        }
    }

    error_t touch_rc = TouchOpsRegister();
    if (touch_rc != OK) return touch_rc;

    error_t bay_rc = BayOpsRegister();
    if (bay_rc != OK) return bay_rc;

    error_t brook_rc = BrookOpsRegister();
    if (brook_rc != OK) return brook_rc;

    error_t memtag_rc = MemTagOpsRegister();
    if (memtag_rc != OK) return memtag_rc;

    error_t hw_rc = HwOpsRegister();
    if (hw_rc != OK) return hw_rc;

    error_t sync_rc = SyncOpsRegister();
    if (sync_rc != OK) return sync_rc;

    error_t turnin_rc = TurnInOpsRegister();
    if (turnin_rc != OK) return turnin_rc;

    debug_printf("[SystemDeck] registered %zu ops (full surface, gated)\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}
