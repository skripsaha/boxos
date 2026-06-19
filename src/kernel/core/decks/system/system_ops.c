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
#include "manifest_stage.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "system_deck.h"
#include "buffer_registry.h"
#include "touch.h"
#include "result.h"
#include "result_ring.h"
#include "kring.h"
#include "kresult.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "tagfs.h"
#include "use_context.h"
#include "perf_trace.h"
#include "kernel_config.h"
#include "amp.h"
#include "rtc.h"
#include "pit.h"
#include "cpu_calibrate.h"
#include "cpuid.h"
#include "fpu.h"   /* g_user_fsbase_used — TLS FS-base context-switch gate */
#include "sync_ops.h"

#define MAX_BROADCAST_TARGETS  256u
#define BROADCAST_TAG_MAX      64u

#define MAX_CTX_USE_TAGS       3u
#define CTX_TAG_LENGTH         64u
#define CTX_USE_PARSE_BUF      512u

/* -------------------------------------------------------------------------
 * Crate translation
 * ------------------------------------------------------------------------- */

static const void *SysCrateRead(const Crate *c, const OpContext *ctx)
{
    if (!c || c->size == 0)          return NULL;
    if (ctx && ctx->proc && ctx->proc->cabin) {
        return vmm_translate_user_addr(ctx->proc->cabin->vmm,
                                       (uintptr_t)c->addr, (size_t)c->size);
    }
    return (const void *)(uintptr_t)c->addr;
}

static void *SysCrateWrite(const Crate *c, const OpContext *ctx, uint64_t bytes)
{
    if (!c || bytes == 0)            return NULL;
    if (bytes > c->capacity)         return NULL;
    if (ctx && ctx->proc && ctx->proc->cabin) {
        return vmm_translate_user_addr(ctx->proc->cabin->vmm,
                                       (uintptr_t)c->addr, (size_t)bytes);
    }
    return (void *)(uintptr_t)c->addr;
}

/* Read a NUL-bounded copy of in_crate into a caller-supplied buffer.
 * Returns OK / ERR_INVALID_ARGUMENT. */
static error_t sys_crate_string(const Crate *c, const OpContext *ctx,
                                char *dst, size_t dst_size)
{
    if (!c || c->size == 0 || dst_size == 0) return ERR_INVALID_ARGUMENT;
    const char *src = SysCrateRead(c, ctx);
    if (!src) return ERR_INVALID_ADDRESS;

    size_t copy = c->size < dst_size - 1 ? c->size : dst_size - 1;
    memcpy(dst, src, copy);
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
        if (length > 0 && !SysCrateRead(src, ctx)) return ERR_INVALID_ADDRESS;
    }

    /* Resolve the tag to its registry id once, outside the process-list
     * lock — sys_proc_has_tag's string/wildcard path would re-acquire
     * process_lock through process_snapshot_tags and self-deadlock. */
    TagFSState *fs = tagfs_get_state();
    if (!fs || !fs->registry) return ERR_ROUTE_NO_SUBSCRIBERS;
    char key[256], value[256];
    tagfs_parse_tag(tag, key, sizeof(key), value, sizeof(value));
    uint16_t tid = tag_registry_lookup(fs->registry,
                                       key, value[0] ? value : NULL);
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

    process_t *new_proc = process_create(tags);
    if (!new_proc) return ERR_SPAWN_FAILED;
    new_proc->cabin->spawner_pid = ctx->proc->pid;

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
            void *kp = SysCrateWrite(out, ctx, sizeof(uint32_t));
            if (kp) {
                uint32_t pid = new_proc->pid;
                memcpy(kp, &pid, sizeof(uint32_t));
                out->size = sizeof(uint32_t);
            }
        }
    }
    return OK;
}

/* SYSTEM_OP_PROC_KILL
 *   params:  [u32 target_pid]   (0 == self exit)
 *   out_crate (optional): u32 killed_pid */
static int SysProcKill(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;

    uint32_t target_pid;
    memcpy(&target_pid, op->params, sizeof(uint32_t));

    bool self_exit = (target_pid == 0);
    if (self_exit) target_pid = ctx->proc->pid;

    if (target_pid == PROCESS_INVALID_PID) return ERR_INVALID_ARGUMENT;
    /* Pin target across the state transition + buffer cleanup so a
     * concurrent destroy on another core cannot recycle target_pid
     * mid-flight. */
    process_t *target = process_find_ref(target_pid);
    if (!target) return ERR_PROCESS_NOT_FOUND;

    process_set_state(target, self_exit ? PROC_DONE : PROC_CRASHED);
    __sync_synchronize();

    /* Publish process.died and release touch claims before the process
     * disappears.  TouchCleanupProcess is idempotent (sets claim_table=NULL
     * after the first call) so the later process_destroy call is safe. */
    TouchCleanupProcess(target);

    BufferRegistryCleanupProcess(target_pid);

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(uint32_t)) {
            void *kp = SysCrateWrite(out, ctx, sizeof(uint32_t));
            if (kp) {
                memcpy(kp, &target_pid, sizeof(uint32_t));
                out->size = sizeof(uint32_t);
            }
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

    ctx->proc->context.user_fsbase = base;
    g_user_fsbase_used = 1;

    /* Synchronous dispatch (the op runs on the caller's own core inside
     * its syscall window — the iretq back skips task_restore_context):
     * program the MSR right here so TLS is live on return. On the async
     * K-Core path proc != current and the value lands at the caller's
     * next context restore — guaranteed, because its pocket is only
     * processed after the context was saved into the ready queue. */
    if (process_get_current() == ctx->proc) {
        uint32_t lo = (uint32_t)base;
        uint32_t hi = (uint32_t)(base >> 32);
        __asm__ volatile("wrmsr" :: "c"(0xC0000100u), "a"(lo), "d"(hi));
    }
    return OK;
}

/* SYSTEM_OP_PROC_INFO
 *   params:  [u32 target_pid]   (0 == self)
 *   out_crate (>= 32 bytes): [u32 pid][u32 state][i32 score][u32 _pad]
 *                            [u64 code_start][u64 code_size]
 *                            [char tags[capacity-32]] */
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

    uint8_t *kp = SysCrateWrite(out, ctx, out->capacity);
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

    size_t tag_room = out->capacity - 32;
    size_t copied   = process_snapshot_tags(target, (char *)(kp + 32), tag_room);
    out->size = 32 + copied;
    if (out->size > out->capacity) out->size = out->capacity;
    process_ref_dec(target);
    return OK;
}

/* SYSTEM_OP_PROC_EXEC
 *   in_crate: filename
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

    /* Locate the file by tag-name + executable tag. */
    #define EXEC_SCAN_MAX 256
    uint32_t *file_ids = kmalloc(EXEC_SCAN_MAX * sizeof(uint32_t));
    if (!file_ids) return ERR_NO_MEMORY;

    int file_count = tagfs_list_all_files(file_ids, EXEC_SCAN_MAX);
    TagFSState *tfs = tagfs_get_state();

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
            const char *key = tfs ? tag_registry_key(tfs->registry, meta.tag_ids[t]) : NULL;
            if (!key) continue;
            if (strcmp(key, filename) == 0)                      has_name = true;
            if (strcmp(key, "app") == 0 || strcmp(key, "utility") == 0) has_exec_tag = true;
        }

        if (has_name && has_exec_tag) {
            found_id = file_ids[i];
            size_t pos = 0;
            for (uint16_t t = 0; t < meta.tag_count; t++) {
                const char *key = tfs ? tag_registry_key(tfs->registry, meta.tag_ids[t]) : NULL;
                if (!key) continue;
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
    int rd = tagfs_read(fh, virt, file_size);
    tagfs_close(fh);
    if (rd < 0) {
        pmm_free(phys, pages);
        return ERR_SPAWN_FAILED;
    }

    process_t *new_proc = process_create(found_tags);
    if (!new_proc) {
        pmm_free(phys, pages);
        return ERR_SPAWN_FAILED;
    }
    new_proc->cabin->spawner_pid = ctx->proc->pid;

    int load = process_load_binary(new_proc, virt, (size_t)file_size);
    pmm_free(phys, pages);
    if (load != 0) {
        process_destroy(new_proc);
        return ERR_SPAWN_FAILED;
    }

    __sync_synchronize();
    process_set_state(new_proc, PROC_WORKING);

    /* Publish process:spawned (mirrors SysProcSpawn). proc_exec is the
     * primary userspace entry — without this hook subscribers see only
     * binary-from-memory spawns and miss every shell-invoked one. */
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
            void *kp = SysCrateWrite(out, ctx, sizeof(uint32_t));
            if (kp) {
                uint32_t new_pid = new_proc->pid;
                memcpy(kp, &new_pid, sizeof(uint32_t));
                out->size = sizeof(uint32_t);
            }
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

    process_t *strand = strand_spawn(ctx->proc->cabin, (uintptr_t)entry_va, arg);
    if (!strand) return ERR_SPAWN_FAILED;

    if (op->out_crate != CRATE_INDEX_NONE) {
        Crate *out = &crates[op->out_crate];
        if (out->capacity >= sizeof(uint32_t)) {
            void *kp = SysCrateWrite(out, ctx, sizeof(uint32_t));
            if (kp) {
                uint32_t pid = strand->pid;
                memcpy(kp, &pid, sizeof(uint32_t));
                out->size = sizeof(uint32_t);
            }
        }
    }

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
    const char *src_kp = SysCrateRead(src, ctx);
    if (!src_kp) return ERR_INVALID_ADDRESS;
    memcpy(input, src_kp, src->size);
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
    uint8_t *kp = SysCrateWrite(out, ctx, 32);
    if (!kp) return ERR_INVALID_ADDRESS;

    memcpy(kp +  0, &r.handle,      sizeof(uint64_t));
    memcpy(kp +  8, &r.phys_addr,   sizeof(uint64_t));
    memcpy(kp + 16, &r.actual_size, sizeof(uint64_t));
    memcpy(kp + 24, &r.virt_addr,   sizeof(uint64_t));
    out->size = 32;
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
            uint8_t *kp = SysCrateWrite(out, ctx, 16);
            if (kp) {
                memcpy(kp,     &handle, sizeof(uint64_t));
                memcpy(kp + 8, &actual, sizeof(uint64_t));
                out->size = 16;
            }
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
            uint8_t *kp = SysCrateWrite(out, ctx, 1);
            if (kp) { kp[0] = has ? 1 : 0; out->size = 1; }
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
            void *kp = SysCrateWrite(out, ctx, sizeof(uint32_t));
            if (kp) {
                memcpy(kp, &score, sizeof(uint32_t));
                out->size = sizeof(uint32_t);
            }
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
        uint32_t max_id = fs->superblock.next_file_id;
        for (uint32_t i = 1; i < max_id; i++) {
            TagFSMetadata meta;
            if (tagfs_get_metadata(i, &meta) == 0) {
                if (meta.flags & TAGFS_FILE_ACTIVE) total_files++;
                tagfs_metadata_free(&meta);
            }
        }
    }

    uint8_t *kp = SysCrateWrite(out, ctx, 12);
    if (!kp) return ERR_INVALID_ADDRESS;
    memcpy(kp +  0, &score,        sizeof(uint32_t));
    memcpy(kp +  4, &total_files,  sizeof(uint32_t));
    memcpy(kp +  8, &total_gaps,   sizeof(uint32_t));
    out->size = 12;
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

    uint8_t *kp = SysCrateWrite(out, ctx, SYSINFO_BLOB_SIZE);
    if (!kp) return ERR_INVALID_ADDRESS;

    /* Version string. Pinned here for now (kernel_config.h has no version
     * macro yet); migrate to a single source when the version policy lands. */
    static const char kver[] = "BoxOS v0.2.0";
    size_t vlen = sizeof(kver) - 1;
    if (vlen > 31) vlen = 31;
    memset(kp, 0, 32);
    memcpy(kp, kver, vlen);

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

    memcpy(kp + 32, &uptime_ns, sizeof(uint64_t));
    memcpy(kp + 40, &total_b,   sizeof(uint64_t));
    memcpy(kp + 48, &used_b,    sizeof(uint64_t));
    memcpy(kp + 56, &free_b,    sizeof(uint64_t));
    memcpy(kp + 64, &tsc_khz,   sizeof(uint64_t));
    memcpy(kp + 72, &cpu_total, sizeof(uint32_t));
    memcpy(kp + 76, &cpu_k,     sizeof(uint32_t));
    memcpy(kp + 80, &cpu_app,   sizeof(uint32_t));
    memcpy(kp + 84, &proc_count,sizeof(uint32_t));
    memcpy(kp + 88, &pit_hz,    sizeof(uint32_t));
    kp[92] = mc_active;
    kp[93] = inv_tsc;
    kp[94] = waitpkg;
    kp[95] = 0;

    out->size = SYSINFO_BLOB_SIZE;
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

    /* Read the handle out of the user payload. Single page by construction
     * (8 bytes), so vmm_translate_user_addr is safe. */
    ManifestHandle *src = (ManifestHandle *)vmm_translate_user_addr(
        ctx->proc->cabin->vmm, (uintptr_t)in->addr, sizeof(ManifestHandle));
    if (!src) return ERR_INVALID_ADDRESS;
    ManifestHandle handle = *src;

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
    uint8_t *kp = SysCrateWrite(out, ctx, EFI_INFO_BLOB_SIZE);
    if (!kp) return ERR_INVALID_ADDRESS;
    memset(kp, 0, EFI_INFO_BLOB_SIZE);

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
    memcpy(kp + 0,  &version,        4);
    kp[4]  = rt_available;
    kp[5]  = esrt_available;
    kp[6]  = sb_available;
    kp[7]  = sb_enforced;
    kp[8]  = sb_setup;
    kp[9]  = sb_audit;
    kp[10] = sb_deployed;
    memcpy(kp + 12, &esrt_count,    4);
    memcpy(kp + 16, &cert_count,    4);
    memcpy(kp + 20, &hash_count,    4);
    memcpy(kp + 24, sb.cert_count_by_db, 6 * 4);
    memcpy(kp + 48, sb.hash_count_by_db, 6 * 4);

    out->size = EFI_INFO_BLOB_SIZE;
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
    uint8_t *kp = SysCrateWrite(out, ctx, sizeof(EfiSystemResourceEntry));
    if (!kp) return ERR_INVALID_ADDRESS;
    memcpy(kp, e, sizeof(EfiSystemResourceEntry));
    out->size = sizeof(EfiSystemResourceEntry);
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

    const uint8_t *pe = SysCrateRead(in, ctx);
    if (!pe) return ERR_INVALID_ADDRESS;

    uint8_t pe_hash[32]     = {0};
    uint8_t signer_hash[32] = {0};
    EfiAuthenticodeResult r = efi_authenticode_verify_pe(
        pe, (uint32_t)in->size, pe_hash, signer_hash);

    uint8_t *kp = SysCrateWrite(out, ctx, EFI_VERIFY_PE_OUT_SIZE);
    if (!kp) return ERR_INVALID_ADDRESS;
    memset(kp, 0, EFI_VERIFY_PE_OUT_SIZE);
    uint32_t result = (uint32_t)r;
    uint32_t pe_size = (uint32_t)in->size;
    memcpy(kp + 0, &result,      4);
    memcpy(kp + 4, &pe_size,     4);
    memcpy(kp + 8, pe_hash,      32);
    memcpy(kp + 40, signer_hash, 32);
    out->size = EFI_VERIFY_PE_OUT_SIZE;
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
        { SYSTEM_OP_TLS_FSBASE,   SysTlsFsbase,   OP_AUTH_NONE,   "system.tls.fsbase" },
        { SYSTEM_OP_PROC_EXEC,    SysProcExec,    OP_AUTH_UTILITY,"system.proc.exec"  },
        { SYSTEM_OP_STRAND_SPAWN, SysStrandSpawn, OP_AUTH_APP,    "system.strand.spawn"},
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

    debug_printf("[SystemDeck] registered %zu ops (full surface, gated)\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}
