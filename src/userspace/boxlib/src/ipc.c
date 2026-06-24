#include "box/ipc.h"
#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/system.h"
#include "box/string.h"
#include "box/file.h"
#include "box/cpu.h"
#include "box/timeouts.h"
#include "box/core/manifest.h"
#include "box/core/crate.h"

/*
 * IPC primitives now travel through the Manifest path. The legacy 256-byte
 * static scratch buffer (`g_ipc_buf`) is gone — payload is whatever the caller
 * gave us, sent through a single Crate of arbitrary length.
 */

#define MFBUF_BYTES 64u  /* enough for one ManifestOp + small inline params */

static int ipc_submit_one_op(uint16_t opcode,
                             uint16_t in_crate_idx,
                             const void *params,
                             uint16_t param_size,
                             Crate *crates,
                             uint16_t crate_count,
                             uint32_t target_pid,
                             uint32_t timeout_ms)
{
    uint8_t mbuf[MFBUF_BYTES];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return -ERR_INTERNAL;
    if (ManifestBuilderAddOp(&mb, DECK_SYSTEM, opcode, 0,
                             in_crate_idx, CRATE_INDEX_NONE,
                             params, param_size) != 0) return -ERR_INTERNAL;
    if (ManifestBuilderFinalize(&mb) != 0) return -ERR_INTERNAL;

    Result r;
    int rc = ManifestSubmitFull((const Manifest *)mbuf, crates, crate_count,
                                target_pid, &r, timeout_ms);
    return box_fail(rc);
}

int send(uint32_t target_pid, const void* data, uint16_t size) {
    if (target_pid == 0) return -ERR_INVALID_ARGUMENT;

    Crate c;
    if (data && size > 0) {
        CrateSetInput(&c, (void *)data, size);
        return ipc_submit_one_op(0x40 /* ROUTE */, 0,
                                 NULL, 0, &c, 1, target_pid, BOX_TIMEOUT_IPC_MS);
    }
    return ipc_submit_one_op(0x40, CRATE_INDEX_NONE,
                             NULL, 0, NULL, 0, target_pid, BOX_TIMEOUT_IPC_MS);
}

int broadcast(const char* tag, const void* data, uint16_t size) {
    if (!tag || tag[0] == '\0') return -ERR_INVALID_ARGUMENT;

    /* Tag travels as inline params (NUL-terminated). System.broadcast caps it
     * at 64 bytes — same effective ceiling as before, no compile-time limit
     * on payload itself. */
    size_t tlen = strlen(tag);
    if (tlen >= 63) tlen = 63;
    char tag_param[64];
    memcpy(tag_param, tag, tlen);
    tag_param[tlen] = '\0';

    Crate c;
    if (data && size > 0) {
        CrateSetInput(&c, (void *)data, size);
        return ipc_submit_one_op(0x41 /* BROADCAST */, 0,
                                 tag_param, (uint16_t)(tlen + 1),
                                 &c, 1, 0, BOX_TIMEOUT_IPC_MS);
    }
    return ipc_submit_one_op(0x41, CRATE_INDEX_NONE,
                             tag_param, (uint16_t)(tlen + 1),
                             NULL, 0, 0, BOX_TIMEOUT_IPC_MS);
}

int listen(uint64_t required_tags, uint8_t flags) {
    uint8_t params[9];
    memcpy(params, &required_tags, sizeof(uint64_t));
    params[8] = flags;
    return ipc_submit_one_op(0x42 /* LISTEN */, CRATE_INDEX_NONE,
                             params, sizeof(params),
                             NULL, 0, 0, BOX_TIMEOUT_IPC_MS);
}

bool receive(Result* out) {
    if (!out) return false;
    return result_pop_ipc(out);
}

// Wall-clock timeout via rdtsc
bool receive_wait(Result* out, uint32_t timeout_ms) {
    // Event-driven IPC wait: UMWAIT on the ResultRing tail where WAITPKG exists
    // (woken the instant a sender's KResultPush lands), cooperative-yield loop
    // otherwise. Was a bare yield-poll. See result_wait_ipc (core/result.c).
    return result_wait_ipc(out, timeout_ms);
}

int send_args(uint32_t target_pid, int argc, char** argv) {
    if (!argv || argc <= 0) return -ERR_INVALID_ARGUMENT;

    char buf[240];
    int pos = 0;
    buf[pos++] = (char)(argc > 127 ? 127 : argc);
    for (int i = 0; i < argc && i < 127 && pos < 235; i++) {
        if (!argv[i]) continue;
        size_t len = strlen(argv[i]);
        if (pos + (int)len + 1 >= 240) break;
        memcpy(buf + pos, argv[i], len);
        pos += (int)len;
        buf[pos++] = '\0';
    }
    return send(target_pid, buf, (uint16_t)pos);
}

int receive_args(int* argc, char argv[][64], int max_args) {
    /* Always initialise *argc up-front. Otherwise the four early-return
     * paths below leave the caller with an uninitialised int — typical
     * usage in utilities is `int argc; receive_args(&argc, argv, 16);`
     * with the return value discarded. A garbage argc then drives a
     * loop that reads beyond argv[max_args-1] into stack noise; PID 44
     * crashed exactly that way (see output.log). */
    if (argc) *argc = 0;

    Result entry;
    if (!receive_wait(&entry, 1000)) return -ERR_TIMEOUT;

    if (entry.data_addr == 0 || entry.data_length == 0) return -ERR_INTERNAL;
    const char* buf = (const char*)(uintptr_t)entry.data_addr;
    uint32_t total = entry.data_length;

    if (total < 2) return -ERR_INTERNAL;

    *argc = (uint8_t)buf[0];
    uint32_t pos = 1;
    for (int i = 0; i < *argc && i < max_args; i++) {
        if (pos >= total) break;

        /* Safe strlen: scan at most to end of buffer */
        size_t len = 0;
        while (pos + len < total && buf[pos + len] != '\0') len++;
        if (len >= 64) len = 63;
        memcpy(argv[i], buf + pos, len);
        argv[i][len] = '\0';
        pos += len + 1;
    }

    /* Apply context tags if present after args.
     *
     * The wire format is `[count][tag\0][tag\0]…`. We must verify each tag
     * is actually NUL-terminated *inside* the buffer before handing the
     * pointer to context_set() — that helper calls strlen() and would
     * scan past the end of the IPC payload if the producer happened to
     * cut us off at the boundary. */
    if (pos < total) {
        uint8_t ctx_count = (uint8_t)buf[pos++];
        for (uint8_t i = 0; i < ctx_count && pos < total; i++) {
            size_t tag_len = 0;
            while (pos + tag_len < total && buf[pos + tag_len] != '\0') tag_len++;
            bool terminated = (pos + tag_len < total) && (buf[pos + tag_len] == '\0');
            if (terminated && tag_len > 0 && tag_len < 32) {
                context_set(buf + pos);
            }
            pos += tag_len + 1;
        }
    }

    return 0;
}
