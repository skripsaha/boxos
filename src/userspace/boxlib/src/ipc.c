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
                                 NULL, 0, &c, 1, target_pid, 0 /* no deadline — reply guaranteed */);
    }
    return ipc_submit_one_op(0x40, CRATE_INDEX_NONE,
                             NULL, 0, NULL, 0, target_pid, 0 /* no deadline — reply guaranteed */);
}

int broadcast(const char* tag, const void* data, uint16_t size) {
    if (!tag || tag[0] == '\0') return -ERR_INVALID_ARGUMENT;

    /* The tag travels whole as inline params, NUL included: cut short it would
     * be some other tag. The kernel reads a tag of any width a tag can have;
     * what bounds it here is this call's manifest buffer, and a tag it cannot
     * carry is refused rather than cut. No limit on the payload itself. */
    size_t tlen = strlen(tag);
    if (tlen + 1 > MFBUF_BYTES - sizeof(Manifest) - sizeof(ManifestOp))
        return -ERR_INVALID_ARGUMENT;

    Crate c;
    if (data && size > 0) {
        CrateSetInput(&c, (void *)data, size);
        return ipc_submit_one_op(0x41 /* BROADCAST */, 0,
                                 tag, (uint16_t)(tlen + 1),
                                 &c, 1, 0, 0 /* no deadline — reply guaranteed */);
    }
    return ipc_submit_one_op(0x41, CRATE_INDEX_NONE,
                             tag, (uint16_t)(tlen + 1),
                             NULL, 0, 0, 0 /* no deadline — reply guaranteed */);
}

int ask(const char *tag, const void *question, uint32_t len,
        void *answer, uint32_t cap, uint32_t *answer_len)
{
    if (!tag || tag[0] == '\0') return -ERR_INVALID_ARGUMENT;
    if (len > 0 && !question)    return -ERR_INVALID_ARGUMENT;

    /* The tag travels whole, NUL included, as it does for broadcast; one too
     * long for the call's manifest is refused there, never cut. */
    size_t tlen = strlen(tag);
    if (tlen >= UINT16_MAX) return -ERR_INVALID_ARGUMENT;

    uint32_t got = 0;
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_ASK, tag, (uint16_t)(tlen + 1),
                     question, len, answer, answer ? cap : 0, &got,
                     BOX_ANSWER_GUARANTEED, NULL);
    if (rc != OK) return box_fail(rc);
    if (answer_len) *answer_len = got;
    return OK;
}

int answer(const Result *question, const void *bytes, uint32_t len)
{
    if (!question || KCTX_KIND(question->context) != KCTX_ASK) return -ERR_INVALID_ARGUMENT;
    if (len > 0 && !bytes)                                       return -ERR_INVALID_ARGUMENT;

    uint32_t params[2] = { question->sender_pid, KCTX_COOKIE24(question->context) };
    int rc = MfCall1(DECK_SYSTEM, SYSTEM_OP_ANSWER, params, sizeof(params),
                     bytes, len, NULL, 0, NULL, BOX_ANSWER_GUARANTEED, NULL);
    return rc == OK ? OK : box_fail(rc);
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
