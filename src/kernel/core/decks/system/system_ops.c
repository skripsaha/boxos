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
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "system_deck.h"
#include "buffer_registry.h"
#include "listen_table.h"
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
        return vmm_translate_user_addr(ctx->proc->cabin,
                                       (uintptr_t)c->addr, (size_t)c->size);
    }
    return (const void *)(uintptr_t)c->addr;
}

static void *SysCrateWrite(const Crate *c, const OpContext *ctx, uint64_t bytes)
{
    if (!c || bytes == 0)            return NULL;
    if (bytes > c->capacity)         return NULL;
    if (ctx && ctx->proc && ctx->proc->cabin) {
        return vmm_translate_user_addr(ctx->proc->cabin,
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

static bool tag_str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static bool sys_proc_has_tag(process_t *p, const char *tag)
{
    char tags[PROCESS_TAG_SIZE];
    process_snapshot_tags(p, tags, sizeof(tags));
    const char *pos = tags;
    while (*pos) {
        const char *comma = strchr(pos, ',');
        size_t len = comma ? (size_t)(comma - pos) : strlen(pos);
        char cur[PROCESS_TAG_SIZE];
        size_t cl = len < sizeof(cur) - 1 ? len : sizeof(cur) - 1;
        memcpy(cur, pos, cl);
        cur[cl] = '\0';
        if (tag_str_eq(cur, tag)) return true;
        if (!comma) break;
        pos = comma + 1;
    }
    return false;
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

    uint32_t delivered = 0;
    for (process_t *iter = process_get_first(); iter; iter = iter->next) {
        if (iter->pid == ctx->proc->pid) continue;
        if (!target_alive(iter))          continue;
        if (!sys_proc_has_tag(iter, tag))  continue;

        uint64_t target_addr = 0;
        if (length > 0 && src) {
            target_addr = ipc_copy_to_heap(ctx->proc, iter,
                                           (uint64_t)src->addr, length);
            if (target_addr == 0) continue;
        }
        if (push_ipc_result(iter, ctx->proc->pid, target_addr, length)) {
            delivered++;
            if (delivered >= MAX_BROADCAST_TARGETS) break;
        }
    }
    return delivered > 0 ? OK : ERR_ROUTE_NO_SUBSCRIBERS;
}

static int SysListen(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 9) return ERR_INVALID_ARGUMENT;

    uint64_t required_tags;
    memcpy(&required_tags, op->params, sizeof(uint64_t));
    uint8_t flags = op->params[8];

    return listen_table_add(ctx->proc->pid, required_tags, flags);
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
    new_proc->spawner_pid = ctx->proc->pid;

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
    process_t *target = process_find(target_pid);
    if (!target) return ERR_PROCESS_NOT_FOUND;

    process_set_state(target, self_exit ? PROC_DONE : PROC_CRASHED);
    __sync_synchronize();

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

    process_t *target = process_find(target_pid);
    if (!target) return ERR_PROCESS_NOT_FOUND;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 32) return ERR_BUFFER_TOO_SMALL;

    uint8_t *kp = SysCrateWrite(out, ctx, out->capacity);
    if (!kp) return ERR_INVALID_ADDRESS;

    uint32_t pid    = target->pid;
    uint32_t state  = (uint32_t)target->state;
    int32_t  score  = target->score;
    uint32_t pad    = 0;
    uint64_t cstart = target->code_start;
    uint64_t csize  = target->code_size;

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
    new_proc->spawner_pid = ctx->proc->pid;

    int load = process_load_binary(new_proc, virt, (size_t)file_size);
    pmm_free(phys, pages);
    if (load != 0) {
        process_destroy(new_proc);
        return ERR_SPAWN_FAILED;
    }

    __sync_synchronize();
    process_set_state(new_proc, PROC_WORKING);

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

    process_t *t = process_find(target_pid);
    if (!t) return ERR_PROCESS_NOT_FOUND;

    error_t srcrc = sys_crate_string(&crates[op->in_crate], ctx, tag_out, tag_out_size);
    if (srcrc != OK) return srcrc;

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

    if (process_has_tag(target, tag)) return ERR_ALREADY_EXISTS;
    if (process_add_tag(target, tag) != 0) return ERR_TAG_LIMIT_EXCEEDED;

    if (strcmp(tag, "stopped") == 0) {
        process_state_t s = process_get_state(target);
        if (s == PROC_WORKING || s == PROC_CREATED) {
            process_set_state(target, PROC_STOPPED);
        }
    }
    return OK;
}

static int SysTagRemove(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                        const OpContext *ctx)
{
    (void)crate_count;
    process_t *target = NULL;
    char tag[64];
    error_t rc = sys_tag_target(op, crates, ctx, &target, tag, sizeof(tag));
    if (rc != OK) return rc;

    if (!process_has_tag(target, tag))           return ERR_TAG_NOT_FOUND;
    if (process_remove_tag(target, tag) != 0)    return ERR_INVALID_ARGUMENT;

    if (strcmp(tag, "stopped") == 0) {
        if (process_get_state(target) == PROC_STOPPED) {
            process_set_state(target, PROC_WORKING);
        }
    }
    return OK;
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
        { SYSTEM_OP_LISTEN,       SysListen,      OP_AUTH_APP,    "system.listen"     },
        /* Process lifecycle: spawn/exec are utility+; info/kill/exit are open
         * (kill of pid==0 is self-exit, used by every process). */
        { SYSTEM_OP_PROC_SPAWN,   SysProcSpawn,   OP_AUTH_UTILITY,"system.proc.spawn" },
        { SYSTEM_OP_PROC_KILL,    SysProcKill,    OP_AUTH_NONE,   "system.proc.kill"  },
        { SYSTEM_OP_PROC_INFO,    SysProcInfo,    OP_AUTH_NONE,   "system.proc.info"  },
        { SYSTEM_OP_PROC_EXEC,    SysProcExec,    OP_AUTH_UTILITY,"system.proc.exec"  },
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
    debug_printf("[SystemDeck] registered %zu ops (full surface, gated)\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}
