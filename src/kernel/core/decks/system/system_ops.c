/*
 * System Deck — Manifest-native handlers (Phase 8 complete).
 *
 * Surface (all opcodes already defined in system_deck.h):
 *
 *   IPC                 route, broadcast, listen
 *   Process lifecycle   spawn, kill, info, exec
 *   Use Context         use.set, use.get, use.clear
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
#include "write_job.h"      /* the defrag is a WriteJob: it holds the file's Helm */
#include "use_context.h"
#include "settle_ops.h"   /* SettleUseRemembered — `use` waits through its Chit */
#include "perf_trace.h"
#include "run.h"            /* TagFSDataCostSay — what the files' bytes cost the medium */
#include "kernel_config.h"
#include "amp.h"
#include "guide.h"   /* guide_dispatch_stats — enclosed/addressed split for perf.dump */
#include "atomics.h"        /* rdtsc — system.proc.cputime's in-flight term */

/* TSC frequency, for turning a cycle delta into microseconds. */
extern uint64_t cpu_get_tsc_freq_khz(void);
#include "rtc.h"
#include "pit.h"
#include "hpet.h"   /* hpet_tick_active/hpet_tick_hz — who really drives IRQ0 */
#include "cpu_calibrate.h"
#include "cpuid.h"
#include "fpu.h"   /* g_user_fsbase_used — TLS FS-base context-switch gate */
#include "sync_ops.h"
#include "cabin.h"          /* cabin_t.vmm — for StrandPool bind virt->phys walk */
#include "cabin_info.h"     /* CabinInfo — where a child's Luggage is written */
#include "cabin_layout.h"   /* CABIN_USER_VA_CANONICAL_END — bind VA range check */
#include "strand_pool_abi.h" /* StrandPool — _Alignof for the bind alignment check */


/* -------------------------------------------------------------------------
 * Crate I/O
 *
 * Fixed payloads move through crate_io.{h,c} (crate_read / crate_write) and
 * variable payloads through crate_in_buf / crate_out_alloc / crate_out_commit.
 * Each walks the user page table page-by-page, so a Crate whose payload
 * crosses a page boundary is copied across every backing frame instead of
 * being clipped to its first page (the vmm_translate_user_addr straddle bug).
 * ------------------------------------------------------------------------- */

/* The whole of in_crate as a NUL-terminated string in the heap, *out the
 * caller's to kfree — never cut to a buffer of this deck's size, so a tag is
 * never read as a shorter one. A caller that holds the text to a width judges
 * the crate's size before this allocates. Returns OK / ERR_INVALID_ARGUMENT /
 * ERR_NO_MEMORY / ERR_INVALID_ADDRESS. */
static error_t sys_crate_text(const Crate *c, const OpContext *ctx, char **out)
{
    *out = NULL;
    if (!c || c->size == 0) return ERR_INVALID_ARGUMENT;

    char *text = kmalloc((size_t)c->size + 1);
    if (!text) return ERR_NO_MEMORY;
    error_t rc = crate_read(c, ctx, text, c->size);
    if (rc != OK) {
        kfree(text);
        return rc;
    }
    text[c->size] = '\0';
    *out = text;
    return OK;
}

static bool target_alive(process_t *t)
{
    if (!t) return false;
    process_state_t s = process_get_state(t);
    return s != PROC_CRASHED && s != PROC_DONE;
}

/* Hand the message to the target — into its reply ring if there is a slot, and
 * onto its Owed queue if there is not.
 *
 * A refusal here used to be the end of the message AND the end of the payload:
 * ipc_copy_to_heap has already deposited it into the target's own cabin heap
 * by the time this is called, and the abandoned deposit was mapped, paid for,
 * and pointed at by nothing. KResultPushOrOwe (kring.c) takes the answer as a
 * DEBT instead, so a full ring costs the message nothing and the payload stays
 * attached to it until it is delivered.
 *
 * True means the message was TAKEN — in the ring, or held for the target's
 * next entry into the kernel. False means it was not taken at all, and the
 * caller still owns the deposit and must give it back. */
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
    return KResultPushOrOwe(target, &r);
}

/* Give back a deposit whose message was not taken. The inverse of
 * cabin_heap_deposit (system_deck.c): one frame per page, translated, unmapped
 * and freed. The bump-allocated VA is deliberately NOT reclaimed — buf_heap_next
 * only goes forward, and a reused deposit address would alias a payload the
 * guest may still be reading through an older Result. */
static void ipc_deposit_release(process_t *target, uint64_t addr, uint32_t length)
{
    if (!target || !target->cabin || addr == 0 || length == 0) return;

    uint32_t pages = (length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    for (uint32_t i = 0; i < pages; i++) {
        uintptr_t va   = (uintptr_t)addr + (uintptr_t)i * PMM_PAGE_SIZE;
        uintptr_t phys = vmm_virt_to_phys(target->cabin->vmm, va);
        vmm_unmap_page(target->cabin->vmm, va);
        if (phys) pmm_free((void *)(phys & ~(uintptr_t)(PMM_PAGE_SIZE - 1)), 1);
    }
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

    /* A full ring is no longer a refusal: the kernel takes the message as a
     * debt the target settles itself at its next syscall gate. What is left to
     * fail is the debt itself — kmalloc refusing the node — and that is
     * ERR_NO_MEMORY, not ERR_ROUTE_TARGET_FULL, because the target's ring is
     * not what ran out. The deposit goes back rather than being abandoned in a
     * cabin nobody will ever point at it from. */
    if (rc == OK && !push_ipc_result(target, ctx->proc->pid, target_addr, length)) {
        ipc_deposit_release(target, target_addr, length);
        rc = ERR_NO_MEMORY;
    }

    process_ref_dec(target);
    return rc;
}

static int SysBroadcast(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;

    /* The tag, spelled in place in the op's params — however long they run. */
    TagSpelling tag;
    error_t spelled = TagSpell((const char *)op->params, op->param_size, &tag);
    if (spelled != OK) return spelled;

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
    uint16_t tid = TagFSLookupSpelling(&tag);
    if (tid == TAGFS_INVALID_TAG_ID) return ERR_ROUTE_NO_SUBSCRIBERS;

    /* Phase 1: snapshot matching pids under process_list_lock so the
     * iter->next chain cannot mutate mid-walk (process_destroy unlinks
     * under the same lock). Tag check is the inline bitfield path —
     * no nested lock acquisition, and nothing is allocated under the lock
     * either: the walk counts first, the room is made for that count, and
     * the walk fills it — if the list grew past the count in between, room
     * is made again for what it is now. What stood here was a stack tray of
     * 256 and a walk that stopped when it was full: the 257th carrier of the
     * tag was never told, and nothing said so. */
    uint32_t *pid_list  = NULL;
    uint32_t  pid_cap   = 0;
    uint32_t  pid_count = 0;
    for (;;) {
        pid_count = 0;
        process_list_lock();
        for (process_t *iter = process_get_first(); iter; iter = iter->next)
        {
            if (iter->pid == ctx->proc->pid)        continue;
            process_state_t s = iter->state;
            if (s == PROC_CRASHED || s == PROC_DONE) continue;
            if (!process_has_tag_id(iter, tid))      continue;
            if (pid_count < pid_cap) pid_list[pid_count] = iter->pid;
            pid_count++;
        }
        process_list_unlock();

        if (pid_count <= pid_cap) break;          /* the room held every one */
        if (pid_list) kfree(pid_list);
        pid_cap  = pid_count;
        pid_list = (uint32_t *)kmalloc(pid_cap * sizeof(uint32_t));
        if (!pid_list) return ERR_NO_MEMORY;
    }
    if (pid_count == 0) {
        if (pid_list) kfree(pid_list);
        return ERR_ROUTE_NO_SUBSCRIBERS;
    }

    /* Phase 2: pin each target via process_find_ref before any cabin or
     * result_ring dereference; release the ref before moving on. This
     * is the same UAF-closing pattern as SysRoute. */
    uint32_t delivered = 0;
    for (uint32_t i = 0; i < pid_count; i++) {
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
        } else {
            /* Same leak, same fix: this subscriber's copy of the payload is
             * already in its cabin heap and nothing will ever name it again. */
            ipc_deposit_release(target, target_addr, length);
        }
        process_ref_dec(target);
    }
    kfree(pid_list);
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
 * The key of the one spelling keeps "god:foo" detected as a god request, and a
 * tag that does not spell refuses the grant with the spelling's code. */
static error_t proc_authorize_tag_grant(const char *tags, const process_t *spawner)
{
    uint32_t requested = 0;
    size_t   len       = strlen(tags);
    size_t   at        = 0;
    for (;;) {
        TagSpelling tag;
        error_t rc = TagListNext(tags, len, &at, &tag);
        if (rc != OK) return rc;
        if (!tag.key) break;
        requested |= AuthBitForKey(tag.key, tag.key_len);
    }

    uint32_t caller = spawner->cabin->auth_bits;
    if (caller & AUTH_TAG_GOD) return OK;       /* god may grant anything */
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

/* One voice for the refusal, because the spawn asks the same question twice —
 * once of the image's head before it commits any memory, once of the whole
 * range afterwards — and a caller that is told "not yours" should hear it said
 * the same way whichever ask caught it. kprintf, not debug_printf: on a board
 * this is the only trace that ring 3 reached for memory it does not hold. A
 * heap page of its own that could not be had (ERR_NO_MEMORY) is a shortage,
 * not a reach, and is answered without the line. */
static error_t SpawnRefuseForeignMemory(const process_t *proc, uint64_t va,
                                        uint64_t size, error_t why)
{
    if (why == ERR_INVALID_ADDRESS)
        kprintf("[Spawn] pid %u may not spawn from 0x%llx..0x%llx: that is not "
                "its cabin's memory\n",
                proc->pid, (unsigned long long)va,
                (unsigned long long)(va + size));
    return why;
}

/* SysProcSpawn starts a process from an ELF image the caller is holding IN ITS
 * OWN CABIN, with caller-supplied tags. Two gates, and each answers a different
 * question about the same call:
 *
 *   MAY YOU CONFER THIS?  proc_authorize_tag_grant, so a spawned child can never
 *   gain an auth privilege the spawner itself lacks (child auth-level ⊆ spawner
 *   auth-level; god may grant anything) — otherwise a utility caller mints a
 *   "god" child and that is utility→god escalation.
 *
 *   IS THIS YOURS?  the calling cabin's own page tables. The image is named by a
 *   VIRTUAL address in that cabin and taken through vmm_user_buf_in_into, which
 *   walks those tables leaf by leaf and stops on the first leaf the cabin does
 *   not hold with the USER bit and cannot have on first touch (only its heap
 *   grows that way). What ring 3 cannot reach by running, it cannot name here
 *   either — it reaches untouched heap by touching it — and the check needs no
 *   list and cannot go stale, because it consults the same structure the
 *   hardware consults.
 *
 * ‼ WHY THE ADDRESS IS NO LONGER PHYSICAL. This op used to take a physical
 * address straight out of the op parameters, check only its page alignment and
 * a 4 GiB ceiling, and hand vmm_phys_to_virt() of it to the ELF parser. Nothing
 * asked whose memory it was. Ring 3 can read any region's base_phys through the
 * app-level MEMTAG_INFO, so a utility process could aim the spawn at kernel
 * memory and receive it mapped into a child it owns: kernel memory read out of
 * ring 3 in three operations. A physical address is not a name ring 3 is
 * entitled to speak. A virtual address in its own cabin is the only naming it
 * owns, and that naming is self-limiting, which is why the parameter is now one.
 *
 * MemTag was the other candidate and answers a different question. Cabin pages
 * reach the registry through MemTagApplyByPhys, which creates regions with
 * ctx == NULL and MEMTAG_REGION_FLAG_PHYSICAL (memtag.c), so MemRegion.ctx never
 * names a cabin and could not be compared against this caller's. And
 * MemRegionAccessAllowed is guard-tag policy, which by construction permits
 * everything while no tag is a guard ("zero guards → zero enforcement",
 * memtag.h) — fail-open, i.e. on a stock boot it would be a check that refuses
 * nothing. The page tables are the record of ownership that is always populated
 * and always fails closed. */
/* SYSTEM_OP_PROC_SPAWN
 *   params:  [u64 binary_va][u64 binary_size] — binary_va is an address in the
 *            CALLING cabin, at any alignment; the whole range must be memory
 *            the cabin holds, or heap it would get on first touch. Not
 *            page-aligned by requirement: the walk copies a partial
 *            first and last leaf, so alignment buys nothing and would only
 *            force callers to move an image they already hold.
 *   in_crate: tags string (NUL-bounded), read whole
 *   out_crate (optional): u32 new_pid
 *   A blob spawn carries no Luggage: nobody types a line at a binary in
 *   memory. The child's CabinInfo says so (luggage_length 0). */
static int SysProcSpawnTagged(const OpContext *ctx, uint64_t binary_va, uint64_t binary_size,
                              const char *tags, const ManifestOp *op, Crate *crates);

static int SysProcSpawn(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc || !ctx->proc->cabin || !ctx->proc->cabin->vmm)
        return ERR_INVALID_ARGUMENT;
    if (op->param_size < 16)              return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint64_t binary_va, binary_size;
    memcpy(&binary_va,   op->params,     sizeof(uint64_t));
    memcpy(&binary_size, op->params + 8, sizeof(uint64_t));

    if (binary_size == 0)                          return ERR_INVALID_ARGUMENT;
    if (binary_size > CONFIG_PROC_MAX_BINARY_SIZE) return ERR_BINARY_TOO_LARGE;
    /* The whole image lies inside the cabin's canonical user half — the same
     * range SysStrandPoolBind holds a pool VA to, and bounded the same way:
     * the address is pinned inside the half ON ITS OWN, before anything is
     * added to it. Capping only the size would not do it — 0xFFFF...FFC0 plus
     * 64 is 0, which is below every ceiling, so a sum-only test hands ring 3
     * a kernel address it chose. The length is then asked as a subtraction
     * from the ceiling, which cannot wrap because binary_va is already under
     * it. */
    if (binary_va <= CABIN_NULL_TRAP_END ||
        binary_va >= CABIN_USER_VA_CANONICAL_END)              return ERR_INVALID_ADDRESS;
    if (binary_size > CABIN_USER_VA_CANONICAL_END - binary_va) return ERR_INVALID_ADDRESS;
    if (process_get_count() >= PROCESS_MAX_COUNT)  return ERR_PROCESS_LIMIT_EXCEEDED;

    char *tags = NULL;
    error_t srcrc = sys_crate_text(&crates[op->in_crate], ctx, &tags);
    if (srcrc != OK) return srcrc;

    /* Gate the child's tags BEFORE creating it or reading the binary: a denied
     * spawn must create no process and dereference no caller memory. */
    error_t gate = proc_authorize_tag_grant(tags, ctx->proc);
    if (gate == OK)
        gate = SysProcSpawnTagged(ctx, binary_va, binary_size, tags, op, crates);
    kfree(tags);
    return gate;
}

/* The spawn itself, once the child's tags have passed the grant gate. */
static int SysProcSpawnTagged(const OpContext *ctx, uint64_t binary_va, uint64_t binary_size,
                              const char *tags, const ManifestOp *op, Crate *crates)
{
    /* LOOK AT THE HEAD BEFORE MAKING ROOM FOR THE WHOLE. Both questions this
     * answers cost one page-table walk and no kernel buffer: does the caller
     * hold the address it named, and is what it holds an ELF at all? If the
     * head lies on a heap page the caller has not touched yet, the walk brings
     * that one page in, as the caller's own first touch would, and it stays
     * mapped. Asked in this order, a caller cannot make the kernel commit up
     * to 64 MiB of contiguous pages against a size it invented at an address
     * it neither holds nor can have on first touch — the staging below is
     * reached only by a call that has already shown an image. */
    uint8_t head[16];   /* an ELF opens with a 16-byte e_ident */
    if (binary_size < sizeof(head)) return ERR_INVALID_ELF;

    error_t peek = vmm_user_buf_in_into(ctx->proc->cabin->vmm,
                                        (uintptr_t)binary_va,
                                        sizeof(head), head);
    if (peek != OK) return SpawnRefuseForeignMemory(ctx->proc, binary_va, binary_size, peek);

    if (head[0] != 0x7F || head[1] != 'E' || head[2] != 'L' || head[3] != 'F')
        return ERR_INVALID_ELF;

    /* Stage the image out of the caller's cabin. PMM pages rather than kmalloc:
     * the ceiling is 64 MiB, which is pages, not a slab — the same staging the
     * volume path (SysProcExecNamed) uses for a file of the same size. The walk
     * is where ownership is decided, and it happens before any process exists,
     * so a refusal leaves nothing behind. */
    size_t   pages      = (size_t)((binary_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    void    *stage_phys = pmm_alloc_zero(pages);
    if (!stage_phys) return ERR_NO_MEMORY;
    uint8_t *elf = (uint8_t *)vmm_phys_to_virt((uintptr_t)stage_phys);

    /* The head translated, but the tail is a separate claim, walked page by
     * page like the head: heap the cabin has not touched is brought in as its
     * first touch would bring it, and anything else it does not hold refuses
     * the call and frees the staging. */
    error_t take = vmm_user_buf_in_into(ctx->proc->cabin->vmm,
                                        (uintptr_t)binary_va,
                                        (size_t)binary_size, elf);
    if (take != OK) {
        pmm_free(stage_phys, pages);
        return SpawnRefuseForeignMemory(ctx->proc, binary_va, binary_size, take);
    }

    process_t *new_proc = process_create(tags);
    if (!new_proc) {
        pmm_free(stage_phys, pages);
        return ERR_SPAWN_FAILED;
    }
    new_proc->cabin->spawner_pid = ctx->proc->pid;
    new_proc->cabin->spawner_gen = pid_generation(ctx->proc->pid);

    int load = process_load_binary(new_proc, elf, (size_t)binary_size);
    pmm_free(stage_phys, pages);
    if (load != 0) {
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
 *            [u32 target_pid][i32 code][u32 generation]
 *                                        (12-byte form: kill that exact
 *                                         incarnation, or nothing)
 *   out_crate (optional): u32 killed_pid
 *
 * exit_code semantics (proc_exit.h): a self-exit publishes `code` masked to
 * [0, INT32_MAX]; killing another process forces PROC_EXIT_KILLED (-1).
 *
 * WHY THE 12-BYTE FORM. A pid is a seat, and seats are re-let. Between
 * deciding to end a process and saying so, that process can end on its own
 * and its pid be handed to another — and a killer with the system ensign has
 * authority over that one too, so the kill lands, on the wrong process, and
 * reports success. Naming the generation closes it: the incarnation is
 * either still in the seat or the kill is refused as ERR_PROCESS_NOT_FOUND,
 * which is the truth. Generation 0 in this form means "any" (never a real
 * generation), so a caller that has no generation to name is not forced to
 * invent one. */
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

    /* 12-byte form: the exact incarnation, or none. */
    uint32_t want_gen = 0;
    if (op->param_size >= 3 * sizeof(uint32_t))
        memcpy(&want_gen, (const uint8_t *)op->params + 2 * sizeof(uint32_t),
               sizeof(uint32_t));

    bool self_exit = (target_pid == 0);
    if (self_exit) target_pid = ctx->proc->pid;

    if (target_pid == PROCESS_INVALID_PID) return ERR_INVALID_ARGUMENT;
    /* Pin target across the state transition + buffer cleanup so a
     * concurrent destroy on another core cannot recycle target_pid
     * mid-flight. */
    process_t *target = process_find_ref(target_pid);
    if (!target) return ERR_PROCESS_NOT_FOUND;

    /* The seat is taken — but by whom? A named generation that does not match
     * is a process that already left; say so rather than end its successor. */
    if (want_gen != 0 && target->generation != want_gen) {
        process_ref_dec(target);
        return ERR_PROCESS_NOT_FOUND;
    }

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

/* SYSTEM_OP_PROC_CREW — who wears this tag.
 *   params:    the tag, as text; param_size is its length, no NUL; any
 *              length a tag can be (tag_spelling.h)
 *   out_crate: [u32 delivered][u32 total] then `delivered` records, each
 *              [u32 pid][u32 generation][u32 state][u32 tags_len]
 *              [u64 cpu_us][char tags[tags_len]]   (no NUL, no padding)
 *
 * WHY THIS EXISTS. `system.broadcast` has always walked the carriers of a tag
 * — it is how one word reaches three hundred of them — but it only ever spoke
 * to them, and told the asker nothing about who they were. Anything that
 * needed to KNOW had one door: system.proc.info, one pid at a time, so
 * "who is running?" was four thousand syscalls and a race in the middle
 * (a pid freed and re-let between two of them names a different process).
 *
 * The same walk, answering instead of speaking. `total` is how many wear the
 * tag and `delivered` how many fitted the crate, so a caller with too little
 * room learns the true count and asks again with room for it — the shape
 * storage.tag.query settled on, for the same reason.
 *
 * The caller is IN its own answer if it wears the tag. A list of who is
 * aboard that leaves out the one asking is a list somebody has to correct.
 *
 * Each record carries `generation` because a pid is a seat and seats are
 * re-let: the pair (pid, generation) is what a later kill must be aimed at,
 * and without it the gap between deciding and doing cannot be closed.
 */
/* The kernel bounce for one answer. Sized so that every seat on the machine
 * (MAX_PROCESSES) can be in it with its tags — the crew of `utility` on a
 * busy machine is not an edge case, and an answer that silently stopped at a
 * round number would be the tray headcount already caught once. */
#define PROC_CREW_ANSWER_MAX  262144u
#define PROC_CREW_HEADER      8u
#define PROC_CREW_FIXED       24u

static int SysProcCrew(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    /* The tag, spelled in place in the op's params — however long they run. */
    TagSpelling tag;
    error_t spelled = TagSpell((const char *)op->params, op->param_size, &tag);
    if (spelled != OK) return spelled;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < PROC_CREW_HEADER) return ERR_BUFFER_TOO_SMALL;

    uint64_t alloc_sz = out->capacity > PROC_CREW_ANSWER_MAX
                        ? PROC_CREW_ANSWER_MAX : out->capacity;
    uint8_t *kp = crate_out_alloc(out, alloc_sz);
    if (!kp) return ERR_INVALID_ADDRESS;

    uint32_t delivered = 0;
    uint32_t total     = 0;
    uint64_t used      = PROC_CREW_HEADER;

    /* Resolved once, outside the list lock: process_has_tag's string path
     * re-enters process_lock through process_snapshot_tags and would
     * self-deadlock (the same rule SysBroadcast keeps). An unknown tag is
     * nobody wearing it, not a failure — the answer is an honest zero. */
    uint16_t tid = TagFSLookupSpelling(&tag);
    if (tid != TAGFS_INVALID_TAG_ID) {
        /* Phase 1: the pids, under the list lock, counted then made room for
         * then filled — never a fixed tray that silently drops the last one. */
        uint32_t *pid_list = NULL;
        uint32_t  pid_cap  = 0;
        for (;;) {
            total = 0;
            process_list_lock();
            for (process_t *iter = process_get_first(); iter; iter = iter->next) {
                process_state_t s = iter->state;
                if (s == PROC_CRASHED || s == PROC_DONE) continue;
                if (!process_has_tag_id(iter, tid))      continue;
                if (total < pid_cap) pid_list[total] = iter->pid;
                total++;
            }
            process_list_unlock();

            if (total <= pid_cap) break;
            if (pid_list) kfree(pid_list);
            pid_cap  = total;
            pid_list = (uint32_t *)kmalloc(pid_cap * sizeof(uint32_t));
            if (!pid_list) { crate_buf_free(kp); return ERR_NO_MEMORY; }
        }

        /* Phase 2: each one pinned while its record is read, outside the list
         * lock — process_snapshot_tags takes process_lock itself. */
        for (uint32_t i = 0; i < total; i++) {
            if (used + PROC_CREW_FIXED >= alloc_sz) break;   /* no room left */

            process_t *mate = process_find_ref(pid_list[i]);
            if (!mate) continue;

            uint32_t pid   = mate->pid;
            uint32_t gen   = mate->generation;
            uint32_t state = (uint32_t)mate->state;
            uint64_t cpu   = mate->total_cpu_time;

            uint64_t room  = alloc_sz - used - PROC_CREW_FIXED;
            size_t   tlen  = process_snapshot_tags(mate,
                                                   (char *)(kp + used + PROC_CREW_FIXED),
                                                   (size_t)room);
            process_ref_dec(mate);

            /* A list that filled the room to its last byte may have been cut
             * there — process_snapshot_tags stops when the next tag will not
             * fit and does not say that it did. A cut list is worse than a
             * missing record: the caller would judge a cabin by tags it does
             * not know it is missing, and narrowing by one of them would drop
             * a cabin that wears it. So the record is not delivered at all,
             * `total` still counts it, and the shortfall sends the caller back
             * with more room. */
            if (tlen + 1 >= (size_t)room) break;

            uint32_t tags_len = (uint32_t)tlen;
            memcpy(kp + used +  0, &pid,      sizeof(uint32_t));
            memcpy(kp + used +  4, &gen,      sizeof(uint32_t));
            memcpy(kp + used +  8, &state,    sizeof(uint32_t));
            memcpy(kp + used + 12, &tags_len, sizeof(uint32_t));
            memcpy(kp + used + 16, &cpu,      sizeof(uint64_t));
            used += PROC_CREW_FIXED + tags_len;
            delivered++;
        }
        if (pid_list) kfree(pid_list);
    }

    memcpy(kp + 0, &delivered, sizeof(uint32_t));
    memcpy(kp + 4, &total,     sizeof(uint32_t));

    int crc = crate_out_commit(out, ctx, kp, used);
    crate_buf_free(kp);
    if (crc != OK) return crc;
    out->size = used;
    return OK;
}

/* Whether a comma list already holds `tag`, element by element and spelled
 * (so "app" never matches inside "apple" or "touch_test"). */
static bool tag_list_holds(const char *list, const TagSpelling *tag)
{
    size_t len = strlen(list);
    size_t at  = 0;
    for (;;) {
        TagSpelling held;
        if (TagListNext(list, len, &at, &held) != OK || !held.key) return false;
        if (held.key_len == tag->key_len && memcmp(held.key, tag->key, tag->key_len) == 0 &&
            (held.value ? (tag->value && held.value_len == tag->value_len &&
                           memcmp(held.value, tag->value, tag->value_len) == 0)
                        : !tag->value))
            return true;
    }
}

/* The child's tags: the file's keys (TagFSKeyList) with the caller's augment —
 * the `aug_len` bytes at `augment`, a comma list — folded in. Per tag: one
 * whose KEY is reserved is refused (ERR_ACCESS_DENIED: no escalation), one
 * that does not spell is refused with the spelling's code, one already there
 * is kept once; the file's tags are never the casualty (trusted, first).
 * *out is the caller's to kfree. */
static error_t proc_exec_merge_augment(const char *file_tags, const char *augment,
                                       size_t aug_len, char **out)
{
    *out = NULL;
    size_t have = strlen(file_tags);
    char  *dst  = kmalloc(have + 1 + aug_len + 1);   /* a spelled tag is never longer than its text */
    if (!dst) return ERR_NO_MEMORY;
    memcpy(dst, file_tags, have + 1);

    size_t at = 0;
    for (;;) {
        TagSpelling tag;
        error_t rc = TagListNext(augment, aug_len, &at, &tag);
        if (rc == OK && tag.key && tagfs_key_is_reserved(tag.key, tag.key_len))
            rc = ERR_ACCESS_DENIED;
        if (rc != OK) {
            kfree(dst);
            return rc;
        }
        if (!tag.key) break;
        if (tag_list_holds(dst, &tag)) continue;
        if (have) dst[have++] = ',';
        TagSpellingText(&tag, dst + have);
        have += TagSpellingTextLength(&tag);
    }
    *out = dst;
    return OK;
}

/*
 * Hand a child its Luggage — what the spawner said to it at boarding, the
 * command line as the person typed it. It rides in the child's own cabin: in
 * the CabinInfo page right after the header when it fits there, in the
 * child's buffer heap when it does not, so a line has no ceiling but memory.
 * Written before the child's first dispatch, so it is there from the child's
 * first instruction — nothing to wait for, nothing that can arrive late.
 */
static error_t cabin_luggage_give(process_t *child, const uint8_t *bytes, uint32_t length)
{
    CabinInfo *ci = (CabinInfo *)vmm_phys_to_virt(child->cabin->cabin_info_phys);
    if (length == 0) {
        ci->luggage_addr   = 0;
        ci->luggage_length = 0;
        return OK;
    }
    if (length <= CABIN_INFO_SIZE - CABIN_LUGGAGE_INLINE_OFFSET) {
        memcpy((uint8_t *)ci + CABIN_LUGGAGE_INLINE_OFFSET, bytes, length);
        ci->luggage_addr = CABIN_INFO_ADDR + CABIN_LUGGAGE_INLINE_OFFSET;
    } else {
        uint64_t va = cabin_heap_deposit(child, bytes, length);
        if (va == 0) return ERR_NO_MEMORY;
        ci->luggage_addr = va;
    }
    ci->luggage_length = length;
    return OK;
}

static int SysProcExecNamed(const ManifestOp *op, Crate *crates, const OpContext *ctx,
                            const TagSpelling *name,
                            const uint8_t *luggage, uint32_t luggage_len);
static int SysProcExecFound(const ManifestOp *op, Crate *crates, const OpContext *ctx,
                            uint32_t found_id, const char *child_tags,
                            const uint8_t *luggage, uint32_t luggage_len);

/* SYSTEM_OP_PROC_EXEC
 *   in_crate: [program name][NUL][luggage bytes…] — the name the program is
 *             filed under, then, after one NUL, the command line as typed
 *             (the program's own name included, as its first word). A crate
 *             that is only a name, no NUL, starts the program with no luggage.
 *   params (optional): caller-tag augment (comma-list); child = file-tags ∪ augment
 *   out_crate (optional): u32 new_pid, or {u32 pid, u32 generation} */
static int SysProcExec(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)               return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    if (process_get_count() >= PROCESS_MAX_COUNT) return ERR_PROCESS_LIMIT_EXCEEDED;

    Crate *src = &crates[op->in_crate];
    if (src->size == 0) return ERR_INVALID_ARGUMENT;
    uint8_t *raw = crate_in_buf(src, ctx);
    if (!raw) return ERR_INVALID_ADDRESS;

    /* The name ends at the first NUL, or at the end of the crate, and is the
     * name's tag: a key, spelled in place, as a created file's stem is. */
    uint64_t name_len = 0;
    while (name_len < src->size && raw[name_len] != '\0') name_len++;

    TagSpelling name;
    error_t spelled = TagSpellKey((const char *)raw, (size_t)name_len, &name);
    if (spelled != OK) {
        crate_buf_free(raw);
        return spelled == ERR_TAGFS_INVALID_TAG ? ERR_INVALID_ARGUMENT : spelled;
    }

    const uint8_t *luggage     = NULL;
    uint32_t       luggage_len = 0;
    if (name_len < src->size) {
        luggage     = raw + name_len + 1;
        luggage_len = (uint32_t)(src->size - name_len - 1);
    }

    int rc = SysProcExecNamed(op, crates, ctx, &name, luggage, luggage_len);
    crate_buf_free(raw);
    return rc;
}

/* Whether the file is a program — ACTIVE, and wearing app or utility — and
 * then its keys, whole (NULL in *out_tags when there was no memory for them). */
static bool SysExecCandidate(uint32_t file_id, char **out_tags)
{
    TagFSMetadata meta;
    if (tagfs_get_metadata(file_id, &meta) != 0) return false;

    bool has_exec_tag = false;
    for (uint32_t t = 0; (meta.flags & TAGFS_FILE_ACTIVE) && t < meta.tag_count; t++) {
        char key[128];
        if (!tagfs_tag_key(meta.tag_ids[t], key, sizeof(key))) continue;
        if (strcmp(key, "app") == 0 || strcmp(key, "utility") == 0) has_exec_tag = true;
    }
    if (has_exec_tag)
        *out_tags = TagFSKeyList(meta.tag_ids, meta.tag_count);
    tagfs_metadata_free(&meta);
    return has_exec_tag;
}

static int SysProcExecNamed(const ManifestOp *op, Crate *crates, const OpContext *ctx,
                            const TagSpelling *name,
                            const uint8_t *luggage, uint32_t luggage_len)
{
    /* Optional caller-tag augment rides in op->params, a comma list of any
     * length, read in place. Empty (param_size == 0) makes the merge below a
     * no-op, so this path stays byte-identical to a plain proc_exec. */
    const char *augment = (const char *)op->params;
    size_t      aug_len = op->param_size;

    /* Locate the file by tag-name + executable tag: every file that carries
     * the name as a tag, asked of the index a page at a time until one is a
     * program. What stood here first was a scan of the first 256 files of the
     * volume, so the 257th program was "not found" — and nothing said so; and
     * then one array as long as every id the volume had ever issued. */
    uint32_t  page_ids[32];
    TagFSPage page;
    uint32_t  from       = 1;
    uint32_t  found_id   = 0;
    char     *found_tags = NULL;
    do {
        error_t asked = TagFSQueryPage(name, 1, from, page_ids, ARRAY_SIZE(page_ids), false, &page);
        if (asked == ERR_TAGFS_NOT_INITIALIZED) return ERR_FILE_NOT_FOUND;
        if (asked != OK) return asked;
        for (uint32_t i = 0; i < page.delivered && found_id == 0; i++) {
            if (SysExecCandidate(page_ids[i], &found_tags))
                found_id = page_ids[i];
        }
        from = page.next;
    } while (found_id == 0 && from != 0);

    if (found_id == 0) return ERR_FILE_NOT_FOUND;
    if (!found_tags) return ERR_NO_MEMORY;

    /* Fold the caller augment into the file's tags BEFORE any binary I/O, so
     * a reserved or unspellable augment costs zero disk reads and never
     * spawns an orphan. */
    char   *child_tags = NULL;
    error_t arc = proc_exec_merge_augment(found_tags, augment, aug_len, &child_tags);
    kfree(found_tags);
    if (arc != OK) return arc;
    int rc = SysProcExecFound(op, crates, ctx, found_id, child_tags, luggage, luggage_len);
    kfree(child_tags);
    return rc;
}

/* The found program read in, made a process wearing `child_tags`, given its
 * Luggage and started. */
static int SysProcExecFound(const ManifestOp *op, Crate *crates, const OpContext *ctx,
                            uint32_t found_id, const char *child_tags,
                            const uint8_t *luggage, uint32_t luggage_len)
{
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
    uint64_t got = 0;
    error_t  rd  = tagfs_read(fh, virt, file_size, &got);
    tagfs_close(fh);
    if (rd != OK || got != file_size) {
        kprintf("[Spawn] not starting file %u: %llu of %llu bytes came back (error %u)\n",
                found_id, (unsigned long long)got, (unsigned long long)file_size,
                (unsigned)rd);
        pmm_free(phys, pages);
        return ERR_SPAWN_FAILED;
    }

    process_t *new_proc = process_create(child_tags);
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

    /* The luggage goes aboard after the cabin is built and before the child
     * is ever dispatched: a child that cannot be given its line is not started
     * without it. */
    error_t lrc = cabin_luggage_give(new_proc, luggage, luggage_len);
    if (lrc != OK) {
        process_destroy(new_proc);
        return lrc;
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
 *  Use Context (use) — what the user is doing, said in tags
 * ========================================================================= */

/* After a set or a clear: the volume is told, and the caller learns whether
 * it listened. out_crate (optional): u8 — 1 when the volume now remembers
 * exactly this context, 0 when it does not (no volume up, no ground on it for
 * the context, a medium that will not take the write, a later word already
 * said to it). The context itself is set either way; the person is told,
 * never refused. A word the medium does not hold yet is answered when the
 * Settle cycle that carries it settles: the caller waits behind its Chit,
 * never inside the kernel. */
static int UseSayRemembered(const ManifestOp *op, Crate *crates, const OpContext *ctx)
{
    Crate *out = (op->out_crate != CRATE_INDEX_NONE && crates[op->out_crate].capacity >= 1)
                     ? &crates[op->out_crate]
                     : NULL;
    uint64_t saying = 0;
    UseText *said   = UseContextSaying(&saying);
    int      rc     = SettleUseRemembered(said, saying, ctx, crates, out);
    UseTextDrop(said);
    return rc;
}

/* SYSTEM_OP_USE_SET  in_crate: comma-separated tag list. No crate, or an
 * empty one, clears — the same thing use.clear says. The list rides whole:
 * copied off the caller's pages at its own length, no ceiling of this deck's
 * choosing, and handed to the context, which spells every tag canonically,
 * refuses one the volume registry could not hold, and only looks the volume's
 * numbers up — saying a context issues none.
 * out_crate (optional): u8 remembered — see UseSayRemembered. */
static int SysUseSet(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;

    if (op->in_crate == CRATE_INDEX_NONE || crates[op->in_crate].size == 0) {
        UseContextClear();
        return UseSayRemembered(op, crates, ctx);
    }

    Crate *src = &crates[op->in_crate];
    void  *raw = crate_in_buf(src, ctx);
    if (!raw) return ERR_INVALID_ADDRESS;

    /* Read at the crate's own length: the context copies what it keeps. */
    error_t rc = UseContextSet(raw, src->size);
    crate_buf_free(raw);
    if (rc != OK) return rc;   /* refused: the context is as it was, nothing to tell */
    return UseSayRemembered(op, crates, ctx);
}

/* SYSTEM_OP_USE_CLEAR  nothing in; out_crate (optional): u8 remembered — see
 * UseSayRemembered. */
static int SysUseClear(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    UseContextClear();
    return UseSayRemembered(op, crates, ctx);
}

/* SYSTEM_OP_USE_GET  out_crate: [u32 count][u32 needed][(u16 len)(char tag[len])]*
 * Every tag that fits, in the context's order. count is what was written, so
 * a caller whose crate was too small sees fewer tags rather than torn ones;
 * needed is the byte length of the whole context comma-joined, NUL included,
 * so that caller can tell a short answer from a full one and size a buffer. */
static int SysUseGet(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 8) return ERR_BUFFER_TOO_SMALL;

    /* The context as it stands at one look, read in place: its canonical
     * text is the comma-joined list, so `needed` is its length and a NUL. */
    UseList *use   = UseContextHold();
    uint32_t count = UseListCount(use);

    uint8_t *kp = crate_out_alloc(out, out->capacity);
    if (!kp) {
        UseListDrop(use);
        return ERR_INVALID_ADDRESS;
    }

    uint32_t needed  = use ? (uint32_t)UseListText(use)->bytes + 1u : 1u;
    uint64_t pos     = 8;
    uint32_t written = 0;
    for (uint32_t i = 0; i < count; i++) {
        TagSpelling tag = UseListTag(use, i);
        size_t      l   = TagSpellingTextLength(&tag);
        if (pos + 2u + l > out->capacity) break;   /* the order kept whole: no later tag */
        uint16_t l16 = (uint16_t)l;
        memcpy(kp + pos, &l16, 2);        pos += 2;
        memcpy(kp + pos, tag.key, l);     pos += l;   /* key, ':' and value lie together */
        written++;
    }
    memcpy(kp,     &written, 4);
    memcpy(kp + 4, &needed,  4);
    UseListDrop(use);

    int crc = crate_out_commit(out, ctx, kp, pos);
    crate_buf_free(kp);
    if (crc != OK) return crc;   /* fail closed: leave out->size unset */
    out->size = pos;
    return OK;
}

/* =========================================================================
 *  Tags
 * ========================================================================= */

/* On OK, *out_target carries a pinned reference and *out_tag the tag's text
 * whole, spelled into *out_spelled; the caller MUST release the reference via
 * process_ref_dec and kfree the text. A text wider than any tag is refused on
 * its size before it is copied, and one that does not spell is refused with
 * the spelling's code — so a process is never handed a different, shorter tag
 * than the one it was asked for. On error, nothing is held. */
static error_t sys_tag_target(const ManifestOp *op, Crate *crates,
                              const OpContext *ctx,
                              process_t **out_target,
                              char **out_tag, TagSpelling *out_spelled)
{
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE)  return ERR_INVALID_ARGUMENT;
    if (crates[op->in_crate].size > TAG_TEXT_WIDTH) return ERR_TAGFS_TAG_TOO_LONG;

    uint32_t target_pid;
    memcpy(&target_pid, op->params, sizeof(uint32_t));
    if (target_pid == PROCESS_INVALID_PID) return ERR_INVALID_ARGUMENT;
    if (target_pid == 0) target_pid = ctx->proc->pid;

    char   *tag   = NULL;
    error_t srcrc = sys_crate_text(&crates[op->in_crate], ctx, &tag);
    if (srcrc == OK)
        srcrc = TagSpell(tag, strlen(tag), out_spelled);
    if (srcrc != OK) {
        kfree(tag);
        return srcrc;
    }

    process_t *t = process_find_ref(target_pid);
    if (!t) {
        kfree(tag);
        return ERR_PROCESS_NOT_FOUND;
    }

    *out_target = t;
    *out_tag    = tag;
    return OK;
}

/* Whether a spelled tag is the bare `stopped`, the one tag a process's state
 * follows. */
static bool tag_is_stopped(const TagSpelling *tag)
{
    return !tag->value && TagSpanIs("stopped", tag->key, tag->key_len);
}

static int SysTagAdd(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    process_t  *target = NULL;
    char       *tag    = NULL;
    TagSpelling spelled;
    error_t rc = sys_tag_target(op, crates, ctx, &target, &tag, &spelled);
    if (rc != OK) return rc;

    int result = OK;
    /* Mutating another process's tags needs authority over it (self / same-cabin
     * / god|system / own child); without this any app could freeze or
     * de-privilege any process by pid. And the granted tag must not exceed the
     * CALLER's own authority — reuse the spawn grant gate so an app cannot
     * self-grant "god"/"system" (escalation). Non-privilege tags (auth bit 0)
     * and "stopped" pass freely. */
    if (!proc_has_authority_over(ctx->proc, target)) {
        result = ERR_ACCESS_DENIED;
    } else if ((result = proc_authorize_tag_grant(tag, ctx->proc)) != OK) {
        /* the grant gate's refusal stands */
    } else if (process_has_tag(target, tag)) {
        result = ERR_ALREADY_EXISTS;
    } else if ((result = process_add_tag(target, tag)) != OK) {
        /* the process does not wear it, and the answer says why */
    } else if (tag_is_stopped(&spelled)) {
        process_state_t s = process_get_state(target);
        if (s == PROC_WORKING || s == PROC_CREATED) {
            process_set_state(target, PROC_STOPPED);
        }
    }
    process_ref_dec(target);
    kfree(tag);
    return result;
}

static int SysTagRemove(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    process_t  *target = NULL;
    char       *tag    = NULL;
    TagSpelling spelled;
    error_t rc = sys_tag_target(op, crates, ctx, &target, &tag, &spelled);
    if (rc != OK) return rc;

    /* Same authority gate as tag.add: only self / same-cabin / god|system /
     * own-child may strip a target's tags (no grant gate — dropping a tag never
     * escalates). */
    int result = OK;
    if (!proc_has_authority_over(ctx->proc, target)) {
        result = ERR_ACCESS_DENIED;
    } else if (!process_has_tag(target, tag)) {
        result = ERR_TAG_NOT_FOUND;
    } else if (process_remove_tag(target, tag) != 0) {
        result = ERR_INVALID_ARGUMENT;
    } else if (tag_is_stopped(&spelled)) {
        if (process_get_state(target) == PROC_STOPPED) {
            process_set_state(target, PROC_WORKING);
        }
    }
    process_ref_dec(target);
    kfree(tag);
    return result;
}

/* SYSTEM_OP_TAG_CHECK   out_crate: u8 has_tag (0/1) */
static int SysTagCheck(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    process_t  *target = NULL;
    char       *tag    = NULL;
    TagSpelling spelled;
    error_t rc = sys_tag_target(op, crates, ctx, &target, &tag, &spelled);
    if (rc != OK) return rc;

    bool has = process_has_tag(target, tag);
    kfree(tag);

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

/* SYSTEM_OP_DEFRAG_FILE  params:[u32 file_id]
 *                        out_crate (optional): u32 frag_score
 * A WriteJob (write_job.h): the file's Helm is held for the whole gather, and
 * a caller whose file another holds is parked until it is handed on. Where the
 * gathered run lies is the volume's to choose — near where the file begins —
 * and no program names a block. A file the system keeps is not moved, warrant
 * or none (PlotGather). */
static int SysDefragFile(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                         const OpContext *ctx)
{
    (void)crate_count;
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;

    WriteJobAsk ask;
    memset(&ask, 0, sizeof(ask));
    ask.kind    = WRITE_JOB_DEFRAG;
    ask.warrant = StorageCallWarrant(ctx);
    memcpy(&ask.file_id, op->params, sizeof(uint32_t));
    if (op->out_crate != CRATE_INDEX_NONE)
        ask.out_crate = &crates[op->out_crate];
    return WriteJobSubmit(&ask, crates, ctx);
}

/* SYSTEM_OP_FRAG_SCORE  out_crate:[u32 score][u32 total_files][u32 total_gaps]
 * — all three from one walk of the volume's files (tagfs_get_fragmentation_score). */
static int SysFragScore(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 12) return ERR_BUFFER_TOO_SMALL;

    uint32_t total_files = 0;
    uint32_t total_gaps  = 0;
    uint32_t score = tagfs_get_fragmentation_score(&total_files, &total_gaps);

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
 *    [88..91]  uint32_t tick_freq_hz
 *    [92]      uint8_t  multicore_active
 *    [93]      uint8_t  has_invariant_tsc
 *    [94]      uint8_t  has_waitpkg
 *    [95]      uint8_t  tick_source
 *
 *  Offsets 88 and 95 travel together and must be read together. A frequency
 *  on its own named the wrong silicon for years: the field was called
 *  pit_freq_hz and `me` printed "PIT: 250 Hz" about an 8254 that pit_init()
 *  deliberately never wrote a byte into, because the HPET had taken IRQ0
 *  through LegacyReplacement and, per HPET 1.0a 2.4.2.1, "the 8254 timer will
 *  not cause any interrupts". The number was true; the name on it was not.
 *  The report now carries the pair {who is ticking, how fast}, and the
 *  frequency comes from whichever part is actually ticking.
 *
 *  These values are duplicated by hand in boxlib's box/system.h, exactly as
 *  the offsets above are — keep the two in step.
 * ========================================================================= */

#define SYSINFO_BLOB_SIZE  96

#define SYSINFO_TICK_SOURCE_NONE          0u  /* nothing is driving IRQ0 */
#define SYSINFO_TICK_SOURCE_HPET_LEGACY   1u  /* HPET timer 0, LegacyReplacement */
#define SYSINFO_TICK_SOURCE_8254          2u  /* 8254 channel 0, divisor ours */

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

    /* Ask who is ticking before asking how fast. When the HPET holds IRQ0 the
     * rate comes from the HPET, derived from the interval written into its
     * comparator; pit_get_frequency() in that mode returns only the number the
     * scheduler was configured with, and no 8254 anywhere agreed to it. */
    uint32_t tick_hz;
    uint8_t  tick_src;
    if (hpet_tick_active()) {
        tick_hz  = hpet_tick_hz();
        tick_src = SYSINFO_TICK_SOURCE_HPET_LEGACY;
    } else {
        tick_hz  = pit_get_frequency();
        tick_src = tick_hz ? SYSINFO_TICK_SOURCE_8254
                           : SYSINFO_TICK_SOURCE_NONE;
    }

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
    memcpy(blob + 88, &tick_hz,   sizeof(uint32_t));
    blob[92] = mc_active;
    blob[93] = inv_tsc;
    blob[94] = waitpkg;
    blob[95] = tick_src;

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
    /* The medium commands the files' bytes went out in, a write and a
     * megabyte (step 1.11): the trace a run of writes is measured by. */
    TagFSDataCostSay();
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
        /* crew answers about others, but says nothing system.proc.info would
         * not say about each of them one at a time — so it is open too. */
        { SYSTEM_OP_PROC_CREW,    SysProcCrew,    OP_AUTH_NONE,   "system.proc.crew"  },
        { SYSTEM_OP_PROC_CPUTIME, SysProcCpuTime, OP_AUTH_NONE,   "system.proc.cputime"},
        { SYSTEM_OP_TLS_FSBASE,   SysTlsFsbase,   OP_AUTH_NONE,   "system.tls.fsbase" },
        { SYSTEM_OP_PROC_EXEC,    SysProcExec,    OP_AUTH_UTILITY,"system.proc.exec"  },
        { SYSTEM_OP_STRAND_SPAWN, SysStrandSpawn, OP_AUTH_APP,    "system.strand.spawn"},
        { SYSTEM_OP_STRAND_RELEASE, SysStrandRelease, OP_AUTH_APP, "system.strand.release"},
        { SYSTEM_OP_STRAND_POOL_BIND, SysStrandPoolBind, OP_AUTH_APP, "system.strand.pool.bind"},
        { SYSTEM_OP_INFO,         SysInfo,        OP_AUTH_NONE,   "system.info"       },
        /* Use Context: what the user is doing, said in tags. set and clear
         * speak for the user, so they take system authority — the shell, or a
         * program wearing `system`. get is anyone's: any program may ask what
         * the user is doing. */
        { SYSTEM_OP_USE_SET,      SysUseSet,      OP_AUTH_SYSTEM, "system.use.set"    },
        { SYSTEM_OP_USE_GET,      SysUseGet,      OP_AUTH_NONE,   "system.use.get"    },
        { SYSTEM_OP_USE_CLEAR,    SysUseClear,    OP_AUTH_SYSTEM, "system.use.clear"  },
        /* Tags: app+. */
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
