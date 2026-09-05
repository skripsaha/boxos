#include "box/core/manifest.h"
#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/string.h"
#include "box/error.h"
#include "boxos_decks.h"  /* SYSTEM_OP_MANIFEST_* opcodes — single source */

/*
 * Userspace Manifest builder + submitter.
 *
 * Builder writes the op stream after a placeholder header, then Finalize
 * back-patches the header. Submit packs the Manifest+Crate references into
 * the Pocket envelope (manifest_addr/manifest_size/crates_addr/crate_count)
 * and sets POCKET_FLAG_MANIFEST so the kernel routes through ManifestExecuteOnce.
 */

int ManifestBuilderInit(ManifestBuilder *mb, void *buf, uint32_t capacity)
{
    if (!mb || !buf || capacity < sizeof(Manifest)) return -1;
    mb->buf       = (uint8_t *)buf;
    mb->capacity  = capacity;
    mb->size      = sizeof(Manifest);    /* reserve header space */
    mb->op_count  = 0;
    mb->finalized = 0;

    /* Pre-zero the header so a never-finalized Manifest fails magic check. */
    memset(buf, 0, sizeof(Manifest));
    return 0;
}

int ManifestBuilderAddOp(ManifestBuilder *mb,
                         uint16_t         deck,
                         uint16_t         opcode,
                         uint16_t         flags,
                         uint16_t         in_crate,
                         uint16_t         out_crate,
                         const void      *params,
                         uint16_t         param_size)
{
    if (!mb || mb->finalized) return -1;
    if (param_size > 0 && !params) return -1;

    uint32_t need = sizeof(ManifestOp) + (uint32_t)param_size;
    if (mb->size + need > mb->capacity) return -1;

    ManifestOp *op = (ManifestOp *)(mb->buf + mb->size);
    op->op_kind    = OP_KIND(deck, opcode);
    op->flags      = flags;
    op->in_crate   = in_crate;
    op->out_crate  = out_crate;
    op->param_size = param_size;
    if (param_size > 0) {
        memcpy(op->params, params, param_size);
    }
    mb->size += need;
    mb->op_count++;
    return 0;
}

int ManifestBuilderFinalize(ManifestBuilder *mb)
{
    if (!mb || mb->finalized) return -1;
    if (mb->op_count == 0)    return -1;

    Manifest *hdr = (Manifest *)mb->buf;
    hdr->magic      = MANIFEST_MAGIC;
    hdr->version    = MANIFEST_VERSION;
    hdr->op_count   = mb->op_count;
    hdr->flags      = 0;
    hdr->total_size = mb->size;
    mb->finalized   = 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Submit path: encode manifest + crate refs into the legacy Pocket fields,
 * trip POCKET_FLAG_MANIFEST, push, wait for the Result.
 * ------------------------------------------------------------------------- */

static void encode_manifest_pocket(Pocket          *p,
                                   const Manifest  *m,
                                   const Crate     *crates,
                                   uint16_t         crate_count,
                                   uint32_t         target_pid)
{
    pocket_prepare(p);
    p->flags         = POCKET_FLAG_MANIFEST;
    p->target_pid    = target_pid;
    p->manifest_size = m->total_size;
    p->crates_addr   = (uint64_t)(uintptr_t)crates;
    p->crate_count   = crate_count;
    p->pier_id       = 0;

    if (m->total_size <= POCKET_ENCLOSURE_MAX) {
        /* The letter fits the envelope: carry the bytes in the ring slot
         * itself. The producer cannot reuse the slot until the kernel moves
         * head past it, so the enclosure is alive for exactly as long as the
         * kernel may still read it — the caller's buffer (typically a stack
         * frame) is free to die the moment the push returns. This is what
         * makes fire-and-forget submission safe: an envelope that only NAMED
         * a stack address handed the kernel a dead frame once the sender
         * stopped waiting. */
        p->flags |= POCKET_FLAG_ENCLOSED;
        p->manifest_addr = 0;
        memcpy(p->enclosure, m, m->total_size);
    } else {
        /* Too large to enclose: the envelope names the caller's memory, and
         * that memory must outlive the kernel's read — a synchronous
         * submitter guarantees it by waiting for the Result; a no-wait
         * submitter must own the bytes past the call (see
         * ManifestSubmitNoWait below). */
        p->manifest_addr = (uint64_t)(uintptr_t)m;
    }
}

/*
 * ManifestSubmitNoWait — push a Manifest-mode Pocket to the kernel without
 * blocking for the reply. Used by touch_await and other async-pattern
 * callers that consume their own replies via a context-filtered
 * result_wait_any loop instead of the synchronous error_code return.
 *
 * Lifetime contract: a Manifest small enough for the enclosure
 * (total_size <= POCKET_ENCLOSURE_MAX — every current no-wait caller)
 * travels inside the ring slot and the caller's buffer may die at return.
 * A larger Manifest travels by address, and since nobody waits here, the
 * caller must keep the bytes in memory it owns until the reply arrives —
 * the way box::ferry keeps them in its station-owned submission object.
 * Handing this function a large Manifest on a stack frame about to return
 * would give the kernel a dead frame to execute.
 *
 * The Crate[] array and crate payloads always travel by address; the same
 * ownership rule applies to them regardless of manifest size.
 *
 * Sharing the encoder + push step here keeps the manifest-mode Pocket
 * layout in exactly one place (encode_manifest_pocket above). Drift
 * between async and sync paths was the original reason touch.c carried
 * a hand-rolled copy of the envelope packing.
 */
/* The next cloakroom token: process-wide, atomic, 24-bit, never zero
 * (zero on the wire means "answers nobody" — fire-and-forget pockets).
 * Uniqueness matters only across the handful of in-flight submits sharing
 * a ring, so the 16M wrap is astronomically clear of collision. */
static uint32_t g_submit_cookie;

static uint32_t submit_cookie_next(void)
{
    uint32_t ck;
    do {
        ck = __atomic_add_fetch(&g_submit_cookie, 1u, __ATOMIC_RELAXED) & 0xFFFFFFu;
    } while (ck == 0u);
    return ck;
}

int ManifestSubmitNoWait(const Manifest *m,
                         const Crate    *crates,
                         uint16_t        crate_count,
                         uint32_t        target_pid)
{
    if (!m)                                   return -ERR_INVALID_ARGS;
    if (m->magic != MANIFEST_MAGIC)           return -ERR_INVALID_ARGS;
    if (crate_count > 0 && !crates)           return -ERR_INVALID_ARGS;

    Pocket p;
    encode_manifest_pocket(&p, m, crates, crate_count, target_pid);
    if (pocket_submit(&p) != 0) return -ERR_POCKET_RING_FULL;
    return OK;
}

int ManifestSubmitFull(const Manifest *m,
                       Crate          *crates,
                       uint16_t        crate_count,
                       uint32_t        target_pid,
                       Result         *out_result,
                       uint32_t        timeout_ms)
{
    if (!m)                                   return -ERR_INVALID_ARGS;
    if (m->magic != MANIFEST_MAGIC)           return -ERR_INVALID_ARGS;
    if (crate_count > 0 && !crates)           return -ERR_INVALID_ARGS;

    /* Drop any orphan replies left over from prior timed-out callers BEFORE
     * we submit — belt to the cloakroom token's braces: the token below
     * already guarantees a stale reply can never be adopted, but a drained
     * ring keeps the wait from wading through corpses. */
    result_drain_orphan_replies();

    Pocket p;
    encode_manifest_pocket(&p, m, crates, crate_count, target_pid);
    uint32_t ck = submit_cookie_next();
    PocketSetCookie24(&p, ck);

    if (pocket_submit(&p) != 0) return -ERR_POCKET_RING_FULL;

    Result tmp;
    if (!result_wait(&tmp, ck, timeout_ms)) {
        return -ERR_TIMEOUT;
    }
    if (out_result) *out_result = tmp;
    return (int)tmp.error_code;
}

int ManifestSubmitTimeout(const Manifest *m,
                          Crate          *crates,
                          uint16_t        crate_count,
                          Result         *out_result,
                          uint32_t        timeout_ms)
{
    return ManifestSubmitFull(m, crates, crate_count, 0, out_result, timeout_ms);
}

int ManifestSubmit(const Manifest *m,
                   Crate          *crates,
                   uint16_t        crate_count,
                   Result         *out_result)
{
    /* No deadline: the reply to a synchronous submit is guaranteed, and a
     * caller that abandons a pocket the K-Core has not processed yet leaves
     * its stack-resident manifest and crates to be read — and written back
     * to — after the frame is gone. */
    return ManifestSubmitTimeout(m, crates, crate_count, out_result, 0);
}

/* -------------------------------------------------------------------------
 * MfCall1 — single-op convenience wrapper used by every boxlib syscall stub.
 * Builds a 1-op Manifest with optional input/output crates, submits, and
 * returns the kernel error_code (OK = 0).
 * ------------------------------------------------------------------------- */

/* =========================================================================
 *  Compile-and-reuse — wraps SYSTEM_OP_MANIFEST_COMPILE / _RELEASE +
 *  the POCKET_FLAG_MANIFEST_HANDLE submit path. See manifest.h for the
 *  workflow.
 * ========================================================================= */

int ManifestCompileHandle(const Manifest *m, ManifestHandle *out_handle)
{
    if (!m || !out_handle)             return -ERR_INVALID_ARGS;
    if (m->magic != MANIFEST_MAGIC)    return -ERR_INVALID_ARGS;

    /* Build an outer Manifest with ONE op = system.manifest.compile.
     * Input crate = inner Manifest bytes; output crate = uint64 handle. */
    uint8_t outer[sizeof(Manifest) + sizeof(ManifestOp)];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, outer, sizeof(outer)) != 0) return -ERR_INVALID_ARGS;
    if (ManifestBuilderAddOp(&mb, DECK_SYSTEM, SYSTEM_OP_MANIFEST_COMPILE,
                             0, 0, 1, NULL, 0) != 0)         return -ERR_INVALID_ARGS;
    if (ManifestBuilderFinalize(&mb) != 0)                   return -ERR_INVALID_ARGS;

    Crate crates[2];
    CrateSetInput(&crates[0], (void *)m, m->total_size);
    ManifestHandle handle_buf = 0;
    CrateSetOutput(&crates[1], &handle_buf, sizeof(handle_buf));

    Result r;
    int rc = ManifestSubmit((Manifest *)outer, crates, 2, &r);
    if (rc != OK) return rc;

    *out_handle = handle_buf;
    return OK;
}

static int submit_handle_pocket(ManifestHandle handle, const Crate *crates,
                                uint16_t crate_count, uint32_t target_pid,
                                Result *out_result, uint32_t timeout_ms)
{
    if (handle == 0)                          return -ERR_INVALID_ARGS;
    if (crate_count > 0 && !crates)           return -ERR_INVALID_ARGS;

    /* Drop orphan replies from prior timed-out submits — same hygiene as
     * ManifestSubmitFull (manifest.c:148). */
    result_drain_orphan_replies();

    Pocket p;
    pocket_prepare(&p);
    p.flags         = POCKET_FLAG_MANIFEST | POCKET_FLAG_MANIFEST_HANDLE;
    p.target_pid    = target_pid;
    p.manifest_addr = (uint64_t)handle;   /* repurposed: carries the handle */
    p.manifest_size = 0;                  /* unused in handle mode */
    p.crates_addr   = (uint64_t)(uintptr_t)crates;
    p.crate_count   = crate_count;
    p.pier_id       = 0;
    uint32_t ck = submit_cookie_next();
    PocketSetCookie24(&p, ck);

    if (pocket_submit(&p) != 0) return -ERR_POCKET_RING_FULL;

    Result tmp;
    if (!result_wait(&tmp, ck, timeout_ms)) return -ERR_TIMEOUT;
    if (out_result) *out_result = tmp;
    return (int)tmp.error_code;
}

int ManifestSubmitHandleTimeout(ManifestHandle  handle,
                                Crate          *crates,
                                uint16_t        crate_count,
                                Result         *out_result,
                                uint32_t        timeout_ms)
{
    return submit_handle_pocket(handle, crates, crate_count, 0,
                                 out_result, timeout_ms);
}

int ManifestSubmitHandle(ManifestHandle  handle,
                         Crate          *crates,
                         uint16_t        crate_count,
                         Result         *out_result)
{
    /* No deadline, for the same reason as ManifestSubmit: the reply is
     * guaranteed, and abandoning the submit leaves the caller's Crate[] to
     * be written back into after its frame is gone. */
    return ManifestSubmitHandleTimeout(handle, crates, crate_count,
                                        out_result, 0);
}

int ManifestReleaseHandle(ManifestHandle handle)
{
    if (handle == 0) return -ERR_INVALID_ARGS;

    /* SYSTEM_OP_MANIFEST_RELEASE takes the handle as in_crate payload (8 B). */
    uint8_t outer[sizeof(Manifest) + sizeof(ManifestOp)];
    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, outer, sizeof(outer)) != 0) return -ERR_INVALID_ARGS;
    if (ManifestBuilderAddOp(&mb, DECK_SYSTEM, SYSTEM_OP_MANIFEST_RELEASE,
                             0, 0, CRATE_INDEX_NONE, NULL, 0) != 0) return -ERR_INVALID_ARGS;
    if (ManifestBuilderFinalize(&mb) != 0)                   return -ERR_INVALID_ARGS;

    Crate crates[1];
    CrateSetInput(&crates[0], &handle, sizeof(handle));

    return ManifestSubmit((Manifest *)outer, crates, 1, NULL);
}

int MfCall1(uint16_t      deck,
            uint16_t      opcode,
            const void   *params,    uint16_t param_size,
            const void   *in_buf,    uint32_t in_size,
            void         *out_buf,   uint32_t out_capacity,
            uint32_t     *out_actual,
            uint32_t      timeout_ms,
            Result       *out_result)
{
    /* Per-process stack scratch — reused across calls. Size covers
     * Manifest header (16) + ManifestOp header (12) + up to 256B params. */
    uint8_t mbuf[284];

    ManifestBuilder mb;
    if (ManifestBuilderInit(&mb, mbuf, sizeof(mbuf)) != 0) return -ERR_INVALID_ARGS;

    Crate crates[2];
    uint16_t cc = 0;
    uint16_t in_idx  = CRATE_INDEX_NONE;
    uint16_t out_idx = CRATE_INDEX_NONE;

    if (in_buf && in_size > 0) {
        CrateSetInput(&crates[cc], (void *)in_buf, in_size);
        in_idx = cc++;
    }
    if (out_buf && out_capacity > 0) {
        CrateSetOutput(&crates[cc], out_buf, out_capacity);
        out_idx = cc++;
    }

    if (ManifestBuilderAddOp(&mb, deck, opcode, 0, in_idx, out_idx,
                             params, param_size) != 0) return -ERR_INVALID_ARGS;
    if (ManifestBuilderFinalize(&mb) != 0)             return -ERR_INVALID_ARGS;

    /* timeout_ms == 0 means WITHOUT a deadline (result_wait's own
     * convention), not "the default one" — a bounded wait on an answer the
     * kernel is guaranteed to deliver turns a late reply into a false
     * refusal, and callers who chose 0 chose to trust the delivery. */
    Result r;
    int rc = ManifestSubmitTimeout((Manifest *)mbuf, crates, cc, &r, timeout_ms);

    if (out_actual && out_idx != CRATE_INDEX_NONE) {
        *out_actual = (uint32_t)crates[out_idx].size;
    }
    if (out_result) *out_result = r;
    return rc;
}
