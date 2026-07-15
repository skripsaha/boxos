/*
 * system.c — userspace process/buffer/tag/perf wrappers (Phase 12: Manifest-only).
 */

#include "box/system.h"
#include "box/core/manifest.h"
#include "box/core/notify.h"
#include "box/core/cabin.h"
#include "box/debug.h"
#include "box/print.h"
#include "box/core/result.h"
#include "box/string.h"
#include "box/error.h"
#include "box/memory.h"   /* strand_pool_flush_self — main-pool flush at exit */
#include "boxos_decks.h"  /* SYSTEM_OP_* opcodes — single source */

/* SYSTEM_OP_* opcodes come from boxos_decks.h (included via box headers).
 * Local aliases below keep call sites readable without redefining the
 * numeric values. */
#define SYS_PROC_SPAWN  SYSTEM_OP_PROC_SPAWN
#define SYS_PROC_KILL   SYSTEM_OP_PROC_KILL
#define SYS_PROC_INFO   SYSTEM_OP_PROC_INFO
#define SYS_CTX_USE     SYSTEM_OP_CTX_USE
#define SYS_PROC_EXEC   SYSTEM_OP_PROC_EXEC
#define SYS_DEFRAG      SYSTEM_OP_DEFRAG_FILE
#define SYS_FRAG_SCORE  SYSTEM_OP_FRAG_SCORE
#define EFI_INFO_BLOB_SIZE       128u
#define EFI_VERIFY_PE_OUT_SIZE   80u
#define EFI_ESRT_ENTRY_SIZE      40u
#define SYS_TAG_ADD     0x20
#define SYS_TAG_REMOVE  0x21
#define SYS_TAG_CHECK   0x22
#define SYS_PERF_DUMP   0x50

#define HW_SYSTEM_REBOOT    0x80
#define HW_SYSTEM_SHUTDOWN  0x81

#define SYS_TIMEOUT_MS  5000u

/* Upper bound on a caller-supplied tag augment for proc_exec_tagged. Mirrors
 * the kernel PROCESS_TAG_SIZE (256) — the child's tag string can hold at most
 * that many bytes — and fits inside the 256-byte MfCall1 param region. */
#define PROC_EXEC_TAGS_MAX 256

/* =========================================================================
 *  Process lifecycle
 * ========================================================================= */

int proc_info(uint16_t pid, proc_info_t *info)
{
    if (!info) return -ERR_INVALID_ARGUMENT;

    uint32_t pid32 = pid;
    uint8_t  out[256] = {0};
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_SYSTEM, SYS_PROC_INFO,
                     &pid32, sizeof(pid32),
                     NULL, 0,
                     out, sizeof(out), &out_actual,
                     SYS_TIMEOUT_MS, NULL);
    if (rc != 0) return box_fail(rc);
    if (out_actual < 32) return -ERR_INTERNAL;

    /* Layout: [u32 pid][u32 state][i32 score][u32 _pad][u64 cstart][u64 csize][char tags[]] */
    uint32_t blob_pid, state;
    int32_t  score;
    memcpy(&blob_pid, out + 0,  4);
    memcpy(&state,    out + 4,  4);
    memcpy(&score,    out + 8,  4);

    info->pid          = (uint16_t)blob_pid;
    info->state        = (uint8_t)state;
    info->priority     = (uint8_t)((score < 0) ? 0 : (score > 255 ? 255 : score));
    info->memory_usage = 0;  /* not exposed by the new op */
    return OK;
}

int tls_set_fsbase(uint64_t base)
{
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_TLS_FSBASE,
                     &base, sizeof(base),
                     NULL, 0, NULL, 0, NULL,
                     SYS_TIMEOUT_MS, NULL);
    return box_fail(rc);
}

/* runtime_init.c — .fini_array + __cxa_finalize teardown (idempotent). */
void __box_runtime_fini(void);

void exit(uint32_t exit_code)
{
    /* Static destructors / atexit callbacks may still print — run them
     * BEFORE the final io_flush so their output reaches the console. */
    __box_runtime_fini();

    /* Ф20e — return the main strand's StrandPool cache to the global heap after
     * all destructors have run (they may still free), so heap leak diagnostics
     * see a quiesced heap with no blocks held out in the magazine. */
    strand_pool_flush_self();

    io_flush();

    /* params:[u32 target_pid][i32 exit_code] — target 0 means self exit; the
     * code rides into SysProcKill's self-exit disposition so process:died
     * carries the real exit code (masked to [0, INT32_MAX] kernel-side). This
     * is the ONLY exit signal now: observers watch the process:died Touch
     * (which also fires on crash), so exit() sends the spawner nothing. */
    struct __attribute__((packed)) { uint32_t target; int32_t code; } kill_param = {
        0, (int32_t)exit_code
    };

    /* Retry SYS_PROC_KILL — the syscall is supposed to be terminal but a
     * transient kernel-side failure (ring full mid-burst, deck dispatcher
     * busy) would otherwise drop us straight into the spin-pause loop
     * below with a still-alive process. Spinning forever as a zombie
     * holds onto kernel resources (file table entries, ring pages,
     * touch claims). Three retries with a yield between is enough to
     * out-wait any short-lived contention; if it still fails we kdbg the
     * failure so the next run carries a paper trail. */
    int kill_rc = -1;
    for (int attempt = 0; attempt < 3 && kill_rc != 0; attempt++) {
        kill_rc = MfCall1(DECK_SYSTEM, SYS_PROC_KILL,
                          &kill_param, sizeof(kill_param),
                          NULL, 0, NULL, 0, NULL,
                          SYS_TIMEOUT_MS, NULL);
        if (kill_rc != 0) yield();
    }
    if (kill_rc != 0) {
        kdbg("[boxlib] exit(): SYS_PROC_KILL failed 3x; halting.");
    }

    /* If the kernel honoured kill we will not run another instruction.
     * If it didn't (or returned and somehow rescheduled us), park the
     * process indefinitely so we don't fall through to undefined code.
     * cpu_pause keeps the core friendly under contention. */
    while (1) {
        __asm__ volatile("pause");
    }
}

int proc_exec_gen(const char *filename, const char *tags, uint32_t *out_gen)
{
    if (out_gen) *out_gen = 0;
    if (!filename || filename[0] == '\0') return -ERR_INVALID_ARGUMENT;
    size_t name_len = strlen(filename);
    if (name_len >= 64) return -ERR_INVALID_ARGUMENT;

    const void *pbuf = NULL; uint16_t psize = 0;
    if (tags && tags[0]) {
        size_t tlen = strlen(tags);
        if (tlen >= PROC_EXEC_TAGS_MAX) return -ERR_INVALID_ARGUMENT;
        pbuf = tags; psize = (uint16_t)tlen;   /* strlen, no NUL — kernel bounds + NUL-terminates */
    }
    /* 8-byte out: the kernel writes {pid, generation} when the crate fits both.
     * out_actual tells us whether the generation half actually arrived. */
    uint32_t out_blob[2] = { 0, 0 };
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_SYSTEM, SYS_PROC_EXEC,
                     pbuf, psize,                  /* params = caller-tag augment */
                     filename, (uint32_t)name_len, /* in_crate = filename (unchanged) */
                     out_blob, sizeof(out_blob), &out_actual,
                     SYS_TIMEOUT_MS, NULL);
    if (rc != 0) return box_fail(rc);
    if (out_gen) *out_gen = (out_actual >= 8) ? out_blob[1] : 0;
    return (int)out_blob[0];
}

/* proc_exec_tagged / proc_exec are the generation-agnostic spellings: the wire
 * is identical (the kernel still pid-gates the 8-byte out), they just discard
 * the generation. Existing callers keep their int-pid return unchanged. */
int proc_exec_tagged(const char *filename, const char *tags)
{
    return proc_exec_gen(filename, tags, NULL);
}

int proc_exec(const char *filename) { return proc_exec_tagged(filename, NULL); }

int proc_kill(uint32_t pid)
{
    if (pid == 0)                 return -ERR_INVALID_ARGUMENT; /* 0 == self-exit in kernel; use exit() */
    if (pid == cabin_info()->pid) return -ERR_INVALID_ARGUMENT; /* self-termination is exit()'s job */
    uint32_t target = pid;                                      /* 4-byte form -> kill-other -> PROC_EXIT_KILLED */
    int rc = MfCall1(DECK_SYSTEM, SYS_PROC_KILL,
                     &target, (uint16_t)sizeof(target),
                     NULL, 0, NULL, 0, NULL,
                     SYS_TIMEOUT_MS, NULL);
    return box_fail(rc);
}

/* =========================================================================
 *  Tags
 * ========================================================================= */

static int proc_tag_op(const char *tag, uint16_t opcode, uint8_t *out_byte)
{
    if (!tag) return -ERR_INVALID_ARGUMENT;
    size_t tlen = strlen(tag);
    if (tlen == 0 || tlen >= 64) return -ERR_INVALID_ARGUMENT;

    uint32_t my_pid = cabin_info()->pid;
    uint8_t out_storage = 0;
    int rc = MfCall1(DECK_SYSTEM, opcode,
                     &my_pid, sizeof(my_pid),
                     tag, (uint32_t)tlen,
                     out_byte ? &out_storage : NULL,
                     out_byte ? 1 : 0,
                     NULL,
                     SYS_TIMEOUT_MS, NULL);
    if (out_byte) *out_byte = out_storage;
    return box_fail(rc);
}

int proc_tag_add(const char *tag)    { return proc_tag_op(tag, SYS_TAG_ADD,    NULL); }
int proc_tag_remove(const char *tag) { return proc_tag_op(tag, SYS_TAG_REMOVE, NULL); }

int proc_tag_check(const char *tag, bool *has_tag)
{
    if (!tag || !has_tag) return -ERR_INVALID_ARGUMENT;
    uint8_t flag = 0;
    int rc = proc_tag_op(tag, SYS_TAG_CHECK, &flag);
    if (rc != 0) return box_fail(rc);
    *has_tag = (flag != 0);
    return OK;
}

/* =========================================================================
 *  System power
 * ========================================================================= */

int reboot(void)
{
    /* hw.system.reboot is noreturn on success; reaching the return is always
     * a failure, so surface the real cause (or ERR_INTERNAL if the call came
     * back OK yet the machine did not reboot). */
    int rc = MfCall1(DECK_HARDWARE, HW_SYSTEM_REBOOT,
                     NULL, 0, NULL, 0, NULL, 0, NULL,
                     SYS_TIMEOUT_MS, NULL);
    return rc != 0 ? box_fail(rc) : -ERR_INTERNAL;
}

int shutdown(void)
{
    int rc = MfCall1(DECK_HARDWARE, HW_SYSTEM_SHUTDOWN,
                     NULL, 0, NULL, 0, NULL, 0, NULL,
                     SYS_TIMEOUT_MS, NULL);
    return rc != 0 ? box_fail(rc) : -ERR_INTERNAL;
}

int sysinfo(system_info_t *info)
{
    if (!info) return -ERR_INVALID_ARGUMENT;

    uint8_t  blob[96] = {0};
    uint32_t got      = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_INFO,
                     NULL, 0, NULL, 0,
                     blob, sizeof(blob), &got, SYS_TIMEOUT_MS, NULL);
    if (rc != 0)            return box_fail(rc);
    if (got < sizeof(blob)) return -ERR_INTERNAL;

    memcpy(info->version, blob, 32);
    info->version[31] = '\0';
    memcpy(&info->uptime_ns,    blob + 32, sizeof(uint64_t));
    memcpy(&info->total_memory, blob + 40, sizeof(uint64_t));
    memcpy(&info->used_memory,  blob + 48, sizeof(uint64_t));
    memcpy(&info->free_memory,  blob + 56, sizeof(uint64_t));
    memcpy(&info->tsc_freq_khz, blob + 64, sizeof(uint64_t));
    memcpy(&info->cpu_total,    blob + 72, sizeof(uint32_t));
    memcpy(&info->cpu_k_cores,  blob + 76, sizeof(uint32_t));
    memcpy(&info->cpu_app_cores,blob + 80, sizeof(uint32_t));
    memcpy(&info->process_count,blob + 84, sizeof(uint32_t));
    memcpy(&info->pit_freq_hz,  blob + 88, sizeof(uint32_t));
    info->multicore_active  = blob[92];
    info->has_invariant_tsc = blob[93];
    info->has_waitpkg       = blob[94];
    info->reserved          = 0;
    return 0;
}

/* =========================================================================
 *  Filesystem maintenance / telemetry
 * ========================================================================= */

int defrag(uint32_t file_id, uint32_t target_block)
{
    uint8_t params[8];
    memcpy(params,     &file_id,      4);
    memcpy(params + 4, &target_block, 4);

    uint32_t score = 0;
    int rc = MfCall1(DECK_SYSTEM, SYS_DEFRAG,
                     params, sizeof(params),
                     NULL, 0,
                     &score, sizeof(score), NULL,
                     SYS_TIMEOUT_MS, NULL);
    if (rc != 0) return box_fail(rc);
    return (int)score;
}

int fragmentation(void)
{
    uint8_t out[12] = {0};
    int rc = MfCall1(DECK_SYSTEM, SYS_FRAG_SCORE,
                     NULL, 0, NULL, 0,
                     out, sizeof(out), NULL,
                     SYS_TIMEOUT_MS, NULL);
    if (rc != 0) return box_fail(rc);
    uint32_t score = 0;
    memcpy(&score, out, 4);
    return (int)score;
}

int perf_dump(void)
{
    int rc = MfCall1(DECK_SYSTEM, SYS_PERF_DUMP,
                     NULL, 0, NULL, 0, NULL, 0, NULL,
                     SYS_TIMEOUT_MS, NULL);
    return box_fail(rc);
}

/* =========================================================================
 *  EFI / Secure Boot introspection
 * ========================================================================= */

int efi_info(efi_info_t *out)
{
    if (!out) return -ERR_INVALID_ARGUMENT;
    uint8_t  blob[EFI_INFO_BLOB_SIZE] = {0};
    uint32_t got = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_EFI_INFO,
                     NULL, 0, NULL, 0,
                     blob, sizeof(blob), &got, SYS_TIMEOUT_MS, NULL);
    if (rc != 0)            return box_fail(rc);
    if (got < sizeof(blob)) return -ERR_INTERNAL;

    memcpy(&out->version,         blob + 0,  4);
    out->rt_available     = blob[4];
    out->esrt_available   = blob[5];
    out->sb_available     = blob[6];
    out->sb_enforced      = blob[7];
    out->sb_setup_mode    = blob[8];
    out->sb_audit_mode    = blob[9];
    out->sb_deployed_mode = blob[10];
    out->_pad             = 0;
    memcpy(&out->esrt_count,       blob + 12, 4);
    memcpy(&out->cert_count_total, blob + 16, 4);
    memcpy(&out->hash_count_total, blob + 20, 4);
    memcpy(out->cert_count_by_db,  blob + 24, 6 * 4);
    memcpy(out->hash_count_by_db,  blob + 48, 6 * 4);
    return 0;
}

int efi_esrt_entry(uint32_t idx, efi_esrt_entry_t *out)
{
    if (!out) return -ERR_INVALID_ARGUMENT;
    uint8_t  params[4];
    memcpy(params, &idx, 4);
    uint8_t  blob[EFI_ESRT_ENTRY_SIZE] = {0};
    uint32_t got = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_EFI_ESRT_GET,
                     NULL, 0, params, sizeof(params),
                     blob, sizeof(blob), &got, SYS_TIMEOUT_MS, NULL);
    if (rc != 0)            return box_fail(rc);
    if (got < sizeof(blob)) return -ERR_INTERNAL;
    memcpy(out->fw_class,                       blob + 0,  16);
    memcpy(&out->fw_type,                       blob + 16, 4);
    memcpy(&out->fw_version,                    blob + 20, 4);
    memcpy(&out->lowest_supported_fw_version,   blob + 24, 4);
    memcpy(&out->capsule_flags,                 blob + 28, 4);
    memcpy(&out->last_attempt_version,          blob + 32, 4);
    memcpy(&out->last_attempt_status,           blob + 36, 4);
    return 0;
}

int efi_verify_pe(const void *pe_buf, uint32_t pe_size, efi_verify_t *out)
{
    if (!out || !pe_buf || pe_size == 0) return -ERR_INVALID_ARGUMENT;
    uint8_t  blob[EFI_VERIFY_PE_OUT_SIZE] = {0};
    uint32_t got = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_EFI_VERIFY_PE,
                     pe_buf, pe_size, NULL, 0,
                     blob, sizeof(blob), &got, SYS_TIMEOUT_MS, NULL);
    if (rc != 0)            return box_fail(rc);
    if (got < sizeof(blob)) return -ERR_INTERNAL;
    uint32_t result_u32;
    uint32_t pe_size_u32;
    memcpy(&result_u32,  blob + 0,  4);
    memcpy(&pe_size_u32, blob + 4,  4);
    out->result  = (efi_verify_result_t)result_u32;
    out->pe_size = pe_size_u32;
    memcpy(out->pe_sha256,     blob + 8,  32);
    memcpy(out->signer_sha256, blob + 40, 32);
    return 0;
}
