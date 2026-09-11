
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
#include "pid_allocator.h"
#include "tagfs.h"
#include "use_context.h"
#include "perf_trace.h"
#include "kernel_config.h"
#include "amp.h"
#include "guide.h"
#include "atomics.h"

extern uint64_t cpu_get_tsc_freq_khz(void);
#include "rtc.h"
#include "pit.h"
#include "cpu_calibrate.h"
#include "cpuid.h"
#include "fpu.h"
#include "sync_ops.h"
#include "cabin.h"
#include "cabin_info.h"
#include "cabin_layout.h"
#include "strand_pool_abi.h"

#define BROADCAST_TAG_MAX      64u



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


static int SysRoute(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                    const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)            return ERR_INVALID_ARGUMENT;
    if (ctx->target_pid == 0)          return ERR_INVALID_ARGUMENT;
    if (ctx->target_pid == ctx->proc->pid) return ERR_ROUTE_SELF;

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
        if (length > 0) {
            void *probe = crate_in_buf(src, ctx);
            if (!probe) return ERR_INVALID_ADDRESS;
            crate_buf_free(probe);
        }
    }

    uint16_t tid = tagfs_tag_lookup(tag);
    if (tid == TAGFS_INVALID_TAG_ID) return ERR_ROUTE_NO_SUBSCRIBERS;

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

        if (pid_count <= pid_cap) break;
        if (pid_list) kfree(pid_list);
        pid_cap  = pid_count;
        pid_list = (uint32_t *)kmalloc(pid_cap * sizeof(uint32_t));
        if (!pid_list) return ERR_NO_MEMORY;
    }
    if (pid_count == 0) {
        if (pid_list) kfree(pid_list);
        return ERR_ROUTE_NO_SUBSCRIBERS;
    }

    uint32_t delivered = 0;
    for (uint32_t i = 0; i < pid_count; i++) {
        process_t *target = process_find_ref(pid_list[i]);
        if (!target) continue;

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
    }
    kfree(pid_list);
    return delivered > 0 ? OK : ERR_ROUTE_NO_SUBSCRIBERS;
}


static error_t proc_authorize_tag_grant(const char *tags, const process_t *spawner)
{
    uint32_t caller = spawner->cabin->auth_bits;
    if (caller & AUTH_TAG_GOD) return OK;

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
    if (requested == 0) return OK;
    if (requested & AUTH_TAG_GOD) return ERR_ACCESS_DENIED;

    const uint32_t levels[] = { OP_AUTH_APP, OP_AUTH_UTILITY, OP_AUTH_SYSTEM, OP_AUTH_NETWORK };
    for (size_t i = 0; i < sizeof(levels)/sizeof(levels[0]); i++) {
        uint32_t mask = auth_mask_for_level(levels[i]);
        if ((requested & mask) && !(caller & mask)) return ERR_ACCESS_DENIED;
    }
    return OK;
}

static bool proc_has_authority_over(const process_t *caller, const process_t *target)
{
    if (!caller || !target) return false;
    if (caller->pid == target->pid) return true;
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

    caller.pid = PID_MAX_COUNT;
    uint32_t caller_gen = pid_generation(caller.pid);

    target.pid = caller.pid;
    if (!proc_has_authority_over(&caller, &target)) {
        kprintf("[PROCAUTH] FAIL: self denied\n");
        return ERR_INTERNAL;
    }

    target.pid = caller.pid - 1;

    caller_cabin.auth_bits   = 0;
    target_cabin.spawner_pid  = PROCESS_INVALID_PID;
    target_cabin.spawner_gen  = 0;
    if (proc_has_authority_over(&caller, &target)) {
        kprintf("[PROCAUTH] FAIL: foreign permitted\n");
        return ERR_INTERNAL;
    }

    target.cabin = &caller_cabin;
    if (!proc_has_authority_over(&caller, &target)) {
        kprintf("[PROCAUTH] FAIL: same-cabin denied\n");
        return ERR_INTERNAL;
    }
    target.cabin = &target_cabin;

    target_cabin.spawner_pid = caller.pid;
    target_cabin.spawner_gen = caller_gen;
    if (!proc_has_authority_over(&caller, &target)) {
        kprintf("[PROCAUTH] FAIL: own child denied\n");
        return ERR_INTERNAL;
    }

    target_cabin.spawner_gen = caller_gen ^ 0xFFFFu;
    if (proc_has_authority_over(&caller, &target)) {
        kprintf("[PROCAUTH] FAIL: stale child permitted (pid-reuse hole)\n");
        return ERR_INTERNAL;
    }

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

static int SysProcKill(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t)) return ERR_INVALID_ARGUMENT;

    uint32_t target_pid;
    memcpy(&target_pid, op->params, sizeof(uint32_t));

    int32_t exit_code = 0;
    if (op->param_size >= sizeof(uint32_t) + sizeof(int32_t))
        memcpy(&exit_code, (const uint8_t *)op->params + sizeof(uint32_t),
               sizeof(int32_t));

    uint32_t want_gen = 0;
    if (op->param_size >= 3 * sizeof(uint32_t))
        memcpy(&want_gen, (const uint8_t *)op->params + 2 * sizeof(uint32_t),
               sizeof(uint32_t));

    bool self_exit = (target_pid == 0);
    if (self_exit) target_pid = ctx->proc->pid;

    if (target_pid == PROCESS_INVALID_PID) return ERR_INVALID_ARGUMENT;
    process_t *target = process_find_ref(target_pid);
    if (!target) return ERR_PROCESS_NOT_FOUND;

    if (want_gen != 0 && target->generation != want_gen) {
        process_ref_dec(target);
        return ERR_PROCESS_NOT_FOUND;
    }

    if (!proc_has_authority_over(ctx->proc, target)) {
        process_ref_dec(target);
        return ERR_ACCESS_DENIED;
    }

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

static int SysTlsFsbase(const ManifestOp *op, Crate *crates,
                        uint16_t crate_count, const OpContext *ctx)
{
    (void)crates;
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint64_t)) return ERR_INVALID_ARGUMENT;

    uint64_t base;
    memcpy(&base, op->params, sizeof(base));

    if (base >= 0x0000800000000000ULL) return ERR_INVALID_ADDRESS;

    g_user_fsbase_used = 1;
    ctx->proc->fsbase_wanted = base;
    __atomic_store_n(&ctx->proc->fsbase_owed, 1, __ATOMIC_RELEASE);

    return OK;
}

static int SysProcCpuTime(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < sizeof(uint64_t)) return ERR_BUFFER_TOO_SMALL;

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

    process_t *target = process_find_ref(target_pid);
    if (!target) return ERR_PROCESS_NOT_FOUND;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 32) {
        process_ref_dec(target);
        return ERR_BUFFER_TOO_SMALL;
    }

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

#define PROC_CREW_ANSWER_MAX  262144u
#define PROC_CREW_HEADER      8u
#define PROC_CREW_FIXED       24u

static int SysProcCrew(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size == 0 || op->param_size > BROADCAST_TAG_MAX) {
        return ERR_INVALID_ARGUMENT;
    }
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    char   tag[BROADCAST_TAG_MAX];
    size_t plen = op->param_size < sizeof(tag) ? op->param_size : sizeof(tag) - 1;
    memcpy(tag, op->params, plen);
    tag[plen] = '\0';
    if (tag[0] == '\0') return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < PROC_CREW_HEADER) return ERR_BUFFER_TOO_SMALL;

    uint64_t alloc_sz = out->capacity > PROC_CREW_ANSWER_MAX
                        ? PROC_CREW_ANSWER_MAX : out->capacity;
    uint8_t *kp = crate_out_alloc(out, alloc_sz);
    if (!kp) return ERR_INVALID_ADDRESS;

    uint32_t delivered = 0;
    uint32_t total     = 0;
    uint64_t used      = PROC_CREW_HEADER;

    uint16_t tid = tagfs_tag_lookup(tag);
    if (tid != TAGFS_INVALID_TAG_ID) {
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

        for (uint32_t i = 0; i < total; i++) {
            if (used + PROC_CREW_FIXED >= alloc_sz) break;

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

static error_t proc_exec_merge_augment(char *dst, size_t dst_size, const char *augment)
{
    const char *p = augment;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t tlen = comma ? (size_t)(comma - p) : strlen(p);
        if (tlen == 0) { if (!comma) break; p = comma + 1; continue; }

        char token[PROCESS_TAG_SIZE];
        if (tlen >= sizeof(token)) return ERR_INVALID_ARGUMENT;
        memcpy(token, p, tlen); token[tlen] = '\0';

        char key[PROCESS_TAG_SIZE];
        const char *colon = strchr(token, ':');
        size_t klen = colon ? (size_t)(colon - token) : tlen;
        memcpy(key, token, klen); key[klen] = '\0';

        if (tagfs_key_is_reserved(key)) return ERR_ACCESS_DENIED;

        if (!tag_list_contains(dst, token)) {
            size_t cur = strlen(dst);
            size_t need = cur + (cur ? 1 : 0) + tlen + 1;
            if (need > dst_size) return ERR_INVALID_ARGUMENT;
            if (cur) dst[cur++] = ',';
            memcpy(dst + cur, token, tlen); dst[cur + tlen] = '\0';
        }
        if (!comma) break;
        p = comma + 1;
    }
    return OK;
}

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
                            const char *filename,
                            const uint8_t *luggage, uint32_t luggage_len);

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

    uint64_t name_len = 0;
    while (name_len < src->size && raw[name_len] != '\0') name_len++;

    char filename[64];
    if (name_len == 0 || name_len >= sizeof(filename)) {
        crate_buf_free(raw);
        return ERR_INVALID_ARGUMENT;
    }
    memcpy(filename, raw, name_len);
    filename[name_len] = '\0';

    const uint8_t *luggage     = NULL;
    uint32_t       luggage_len = 0;
    if (name_len < src->size) {
        luggage     = raw + name_len + 1;
        luggage_len = (uint32_t)(src->size - name_len - 1);
    }

    int rc = SysProcExecNamed(op, crates, ctx, filename, luggage, luggage_len);
    crate_buf_free(raw);
    return rc;
}

static int SysProcExecNamed(const ManifestOp *op, Crate *crates, const OpContext *ctx,
                            const char *filename,
                            const uint8_t *luggage, uint32_t luggage_len)
{
    char augment[PROCESS_TAG_SIZE];
    augment[0] = '\0';
    if (op->param_size > 0) {
        if (op->param_size >= sizeof(augment)) return ERR_INVALID_ARGUMENT;
        memcpy(augment, op->params, op->param_size);
        augment[op->param_size] = '\0';
    }

    uint32_t room = tagfs_file_ceiling();
    if (room == 0) return ERR_FILE_NOT_FOUND;
    uint32_t *file_ids = kmalloc(room * sizeof(uint32_t));
    if (!file_ids) return ERR_NO_MEMORY;

    const char *by_name[1] = { filename };
    int file_count = tagfs_query_files(by_name, 1, file_ids, room);

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

        bool has_exec_tag = false;
        for (uint16_t t = 0; t < meta.tag_count; t++) {
            char key[128];
            if (!tagfs_tag_key(meta.tag_ids[t], key, sizeof(key))) continue;
            if (strcmp(key, "app") == 0 || strcmp(key, "utility") == 0) has_exec_tag = true;
        }

        if (has_exec_tag) {
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

    error_t lrc = cabin_luggage_give(new_proc, luggage, luggage_len);
    if (lrc != OK) {
        process_destroy(new_proc);
        return lrc;
    }

    uint32_t child_pid = new_proc->pid;
    uint32_t child_gen = new_proc->generation;

    __sync_synchronize();
    process_set_state(new_proc, PROC_WORKING);

    {
        struct __attribute__((packed)) {
            uint32_t pid;
            uint32_t parent_pid;
        } ev = { child_pid, ctx->proc->pid };
        TouchPublish("process:spawned", &ev, sizeof(ev));
    }

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

static int SysStrandSpawn(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                          const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc || !ctx->proc->cabin) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 16) return ERR_INVALID_ARGUMENT;

    uint64_t entry_va, arg;
    memcpy(&entry_va, op->params,     sizeof(uint64_t));
    memcpy(&arg,      op->params + 8, sizeof(uint64_t));

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

static int SysStrandRelease(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                            const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc || !ctx->proc->cabin) return ERR_INVALID_ARGUMENT;
    if (op->param_size < sizeof(uint32_t))        return ERR_INVALID_ARGUMENT;

    uint32_t pid;
    memcpy(&pid, op->params, sizeof(uint32_t));

    process_t *target = process_find_ref(pid);
    if (!target) return OK;

    if (target->cabin == ctx->proc->cabin)
        __atomic_store_n(&target->reap_blocked, 0u, __ATOMIC_SEQ_CST);

    process_ref_dec(target);
    return OK;
}

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


static int UseSayRemembered(const ManifestOp *op, Crate *crates, const OpContext *ctx)
{
    bool kept = false;
    UseContextRemember(&kept);
    if (op->out_crate != CRATE_INDEX_NONE && crates[op->out_crate].capacity >= 1) {
        uint8_t byte = kept ? 1u : 0u;
        (void)crate_write(&crates[op->out_crate], ctx, &byte, 1);
    }
    return OK;
}

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

    char *list = kmalloc(src->size + 1);
    if (!list) {
        crate_buf_free(raw);
        return ERR_NO_MEMORY;
    }
    memcpy(list, raw, src->size);
    list[src->size] = '\0';
    crate_buf_free(raw);

    error_t rc = UseContextSet(list, true);
    kfree(list);
    if (rc != OK) return rc;
    return UseSayRemembered(op, crates, ctx);
}

static int SysUseClear(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    UseContextClear();
    return UseSayRemembered(op, crates, ctx);
}

static int SysUseGet(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                     const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    Crate *out = &crates[op->out_crate];
    if (out->capacity < 8) return ERR_BUFFER_TOO_SMALL;

    char        *block = NULL;
    const char **names = NULL;
    uint32_t     count = 0;
    error_t arc = UseContextTagArray(&block, &names, &count);
    if (arc != OK) return arc;

    uint8_t *kp = crate_out_alloc(out, out->capacity);
    if (!kp) {
        kfree(block);
        kfree(names);
        return ERR_INVALID_ADDRESS;
    }

    uint32_t needed  = 1;
    uint64_t pos     = 8;
    uint32_t written = 0;
    for (uint32_t i = 0; i < count; i++) {
        size_t l = strlen(names[i]);
        needed += (uint32_t)l + (i ? 1u : 0u);
        if (pos + 2u + l > out->capacity) continue;
        if (written != i) continue;
        uint16_t l16 = (uint16_t)l;
        memcpy(kp + pos, &l16, 2);       pos += 2;
        memcpy(kp + pos, names[i], l);   pos += l;
        written++;
    }
    memcpy(kp,     &written, 4);
    memcpy(kp + 4, &needed,  4);
    kfree(block);
    kfree(names);

    int crc = crate_out_commit(out, ctx, kp, pos);
    crate_buf_free(kp);
    if (crc != OK) return crc;
    out->size = pos;
    return OK;
}


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

    if (!proc_has_authority_over(ctx->proc, target)) {
        process_ref_dec(target);
        return ERR_ACCESS_DENIED;
    }
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

    static const char kver[] = "BoxOS v0.2.0";
    size_t vlen = sizeof(kver) - 1;
    if (vlen > 31) vlen = 31;
    memset(blob, 0, 32);
    memcpy(blob, kver, vlen);

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


static int SysPerfDump(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                       const OpContext *ctx)
{
    (void)op; (void)crates; (void)crate_count; (void)ctx;
    perf_dump();
    ManifestStageDumpAll();
    {
        uint64_t d[2];
        guide_dispatch_stats(d);
        kprintf("[GUIDE] dispatches: enclosed=%lu addressed=%lu\n",
                (unsigned long)d[0], (unsigned long)d[1]);
    }
    return OK;
}


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
                                  false,
                                  &handle);
    if (rc != OK) return (int)rc;

    error_t commit_rc = vmm_user_buf_commit_out(ctx->proc->cabin->vmm,
                                                 (uintptr_t)out->addr,
                                                 &handle,
                                                 sizeof(handle));
    if (commit_rc != OK) {
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

    ManifestHandle handle;
    error_t read_rc = crate_read(in, ctx, &handle, sizeof(handle));
    if (read_rc != OK) return read_rc;

    CompiledManifest *cm = ManifestResolve(handle);
    if (!cm) return ERR_INVALID_ARGUMENT;
    if (cm->owner_pid != ctx->proc->pid) {
        ManifestRelease(handle);
        return ERR_ACCESS_DENIED;
    }
    ManifestRelease(handle);
    return (int)ManifestRelease(handle);
}


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
    if (in->size > (16u * 1024u * 1024u))  return ERR_INVALID_ARGUMENT;
    if (out->capacity < EFI_VERIFY_PE_OUT_SIZE) return ERR_BUFFER_TOO_SMALL;

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


error_t SystemDeckRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        { SYSTEM_OP_ROUTE,        SysRoute,       OP_AUTH_APP,    "system.route"      },
        { SYSTEM_OP_ROUTE_TAG,    SysBroadcast,   OP_AUTH_APP,    "system.broadcast"  },
        { SYSTEM_OP_PROC_SPAWN,   SysProcSpawn,   OP_AUTH_UTILITY,"system.proc.spawn" },
        { SYSTEM_OP_PROC_KILL,    SysProcKill,    OP_AUTH_NONE,   "system.proc.kill"  },
        { SYSTEM_OP_PROC_INFO,    SysProcInfo,    OP_AUTH_NONE,   "system.proc.info"  },
        { SYSTEM_OP_PROC_CREW,    SysProcCrew,    OP_AUTH_NONE,   "system.proc.crew"  },
        { SYSTEM_OP_PROC_CPUTIME, SysProcCpuTime, OP_AUTH_NONE,   "system.proc.cputime"},
        { SYSTEM_OP_TLS_FSBASE,   SysTlsFsbase,   OP_AUTH_NONE,   "system.tls.fsbase" },
        { SYSTEM_OP_PROC_EXEC,    SysProcExec,    OP_AUTH_UTILITY,"system.proc.exec"  },
        { SYSTEM_OP_STRAND_SPAWN, SysStrandSpawn, OP_AUTH_APP,    "system.strand.spawn"},
        { SYSTEM_OP_STRAND_RELEASE, SysStrandRelease, OP_AUTH_APP, "system.strand.release"},
        { SYSTEM_OP_STRAND_POOL_BIND, SysStrandPoolBind, OP_AUTH_APP, "system.strand.pool.bind"},
        { SYSTEM_OP_INFO,         SysInfo,        OP_AUTH_NONE,   "system.info"       },
        { SYSTEM_OP_USE_SET,      SysUseSet,      OP_AUTH_SYSTEM, "system.use.set"    },
        { SYSTEM_OP_USE_GET,      SysUseGet,      OP_AUTH_NONE,   "system.use.get"    },
        { SYSTEM_OP_USE_CLEAR,    SysUseClear,    OP_AUTH_SYSTEM, "system.use.clear"  },
        { SYSTEM_OP_TAG_ADD,      SysTagAdd,      OP_AUTH_APP,    "system.tag.add"    },
        { SYSTEM_OP_TAG_REMOVE,   SysTagRemove,   OP_AUTH_APP,    "system.tag.remove" },
        { SYSTEM_OP_TAG_CHECK,    SysTagCheck,    OP_AUTH_NONE,   "system.tag.check"  },
        { SYSTEM_OP_DEFRAG_FILE,  SysDefragFile,  OP_AUTH_UTILITY,"system.fs.defrag"  },
        { SYSTEM_OP_FRAG_SCORE,   SysFragScore,   OP_AUTH_NONE,   "system.fs.score"   },
        { SYSTEM_OP_PERF_DUMP,    SysPerfDump,    OP_AUTH_SYSTEM, "system.perf.dump"  },

        { SYSTEM_OP_MANIFEST_COMPILE, SysManifestCompile, OP_AUTH_APP,
          "system.manifest.compile" },
        { SYSTEM_OP_MANIFEST_RELEASE, SysManifestRelease, OP_AUTH_APP,
          "system.manifest.release" },
        { SYSTEM_OP_EFI_INFO,     SysEfiInfo,     OP_AUTH_NONE,   "system.efi.info"   },
        { SYSTEM_OP_EFI_ESRT_GET, SysEfiEsrtGet,  OP_AUTH_NONE,   "system.efi.esrt"   },
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