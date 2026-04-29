/*
 * system.c — userspace process/buffer/tag/perf wrappers (Phase 12: Manifest-only).
 */

#include "box/system.h"
#include "box/manifest.h"
#include "box/notify.h"
#include "box/ipc.h"
#include "box/print.h"
#include "box/result.h"
#include "box/string.h"
#include "box/error.h"

#define SYS_PROC_SPAWN  0x01
#define SYS_PROC_KILL   0x02
#define SYS_PROC_INFO   0x03
#define SYS_CTX_USE     0x04
#define SYS_PROC_EXEC   0x06
#define SYS_DEFRAG      0x18
#define SYS_FRAG_SCORE  0x19
#define SYS_TAG_ADD     0x20
#define SYS_TAG_REMOVE  0x21
#define SYS_TAG_CHECK   0x22
#define SYS_PERF_DUMP   0x50

#define HW_SYSTEM_REBOOT    0x80
#define HW_SYSTEM_SHUTDOWN  0x81

#define SYS_TIMEOUT_MS  5000u

/* =========================================================================
 *  Process lifecycle
 * ========================================================================= */

int proc_info(uint16_t pid, proc_info_t *info)
{
    if (!info) return ERR_INVALID_ARGS;

    uint32_t pid32 = pid;
    uint8_t  out[256] = {0};
    uint32_t out_actual = 0;
    int rc = MfCall1(DECK_SYSTEM, SYS_PROC_INFO,
                     &pid32, sizeof(pid32),
                     NULL, 0,
                     out, sizeof(out), &out_actual,
                     SYS_TIMEOUT_MS, NULL);
    if (rc != 0) return rc;
    if (out_actual < 32) return ERR_RESULT_INVALID;

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

void exit(uint32_t exit_code)
{
    io_flush();

    CabinInfo *ci = cabin_info();
    if (ci->spawner_pid != 0) {
        uint8_t msg[2] = { 0xFE, (uint8_t)(exit_code & 0xFF) };
        send(ci->spawner_pid, msg, 2);
    }

    /* params:[u32 target_pid] — 0 means self exit. */
    uint32_t target = 0;
    (void)MfCall1(DECK_SYSTEM, SYS_PROC_KILL,
                  &target, sizeof(target),
                  NULL, 0, NULL, 0, NULL,
                  SYS_TIMEOUT_MS, NULL);

    while (1) {
        __asm__ volatile("pause");
    }
}

int proc_exec(const char *filename)
{
    if (!filename || filename[0] == '\0') return -1;
    size_t name_len = strlen(filename);
    if (name_len >= 64) return -1;

    uint32_t new_pid = 0;
    int rc = MfCall1(DECK_SYSTEM, SYS_PROC_EXEC,
                     NULL, 0,
                     filename, (uint32_t)name_len,
                     &new_pid, sizeof(new_pid), NULL,
                     SYS_TIMEOUT_MS, NULL);
    if (rc != 0) return -1;
    return (int)new_pid;
}

/* =========================================================================
 *  Tags
 * ========================================================================= */

static int proc_tag_op(const char *tag, uint16_t opcode, uint8_t *out_byte)
{
    if (!tag) return ERR_INVALID_ARGS;
    size_t tlen = strlen(tag);
    if (tlen == 0 || tlen >= 64) return ERR_INVALID_ARGS;

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
    return rc;
}

int proc_tag_add(const char *tag)    { return proc_tag_op(tag, SYS_TAG_ADD,    NULL); }
int proc_tag_remove(const char *tag) { return proc_tag_op(tag, SYS_TAG_REMOVE, NULL); }

int proc_tag_check(const char *tag, bool *has_tag)
{
    if (!tag || !has_tag) return ERR_INVALID_ARGS;
    uint8_t flag = 0;
    int rc = proc_tag_op(tag, SYS_TAG_CHECK, &flag);
    if (rc != 0) return rc;
    *has_tag = (flag != 0);
    return OK;
}

/* =========================================================================
 *  System power
 * ========================================================================= */

int reboot(void)
{
    /* hw.system.reboot is noreturn; if the call returns we return -1. */
    (void)MfCall1(DECK_HARDWARE, HW_SYSTEM_REBOOT,
                  NULL, 0, NULL, 0, NULL, 0, NULL,
                  SYS_TIMEOUT_MS, NULL);
    return -1;
}

int shutdown(void)
{
    (void)MfCall1(DECK_HARDWARE, HW_SYSTEM_SHUTDOWN,
                  NULL, 0, NULL, 0, NULL, 0, NULL,
                  SYS_TIMEOUT_MS, NULL);
    return -1;
}

int sysinfo(system_info_t *info)
{
    /* No sysinfo op exists in the Manifest path — fill with defaults so old
     * callers don't trip. Future work: add hw.cpu.info / system.health ops. */
    if (!info) return -1;
    memcpy(info->version, "BoxOS v0.1.0", 13);
    info->version[12]    = '\0';
    info->uptime_seconds = 0;
    info->total_memory   = 0;
    info->used_memory    = 0;
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
    return (rc != 0) ? -1 : (int)score;
}

int fragmentation(void)
{
    uint8_t out[12] = {0};
    int rc = MfCall1(DECK_SYSTEM, SYS_FRAG_SCORE,
                     NULL, 0, NULL, 0,
                     out, sizeof(out), NULL,
                     SYS_TIMEOUT_MS, NULL);
    if (rc != 0) return -1;
    uint32_t score = 0;
    memcpy(&score, out, 4);
    return (int)score;
}

int perf_dump(void)
{
    return MfCall1(DECK_SYSTEM, SYS_PERF_DUMP,
                   NULL, 0, NULL, 0, NULL, 0, NULL,
                   SYS_TIMEOUT_MS, NULL);
}
