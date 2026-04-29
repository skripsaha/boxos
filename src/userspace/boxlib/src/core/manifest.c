#include "box/core/manifest.h"
#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/string.h"
#include "box/error.h"

/*
 * Userspace Manifest builder + submitter.
 *
 * Builder writes the op stream after a placeholder header, then Finalize
 * back-patches the header. Submit packs the Manifest+Crate references into
 * the legacy Pocket fields (data_addr/data_length/route_tag) and sets
 * POCKET_FLAG_MANIFEST so the kernel routes through ManifestExecuteOnce.
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
    p->flags       = POCKET_FLAG_MANIFEST;
    p->target_pid  = target_pid;
    p->data_addr   = (uint64_t)(uintptr_t)m;
    p->data_length = m->total_size;

    /* route_tag layout in manifest mode:
     *   [0..7]   crates_addr
     *   [8..9]   crate_count
     *   [10..11] pier_id (0 for now)
     */
    uint64_t crates_addr = (uint64_t)(uintptr_t)crates;
    memcpy(p->route_tag + 0, &crates_addr, sizeof(uint64_t));
    memcpy(p->route_tag + 8, &crate_count, sizeof(uint16_t));
    uint16_t pier_id = 0;
    memcpy(p->route_tag + 10, &pier_id, sizeof(uint16_t));
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

    Pocket p;
    encode_manifest_pocket(&p, m, crates, crate_count, target_pid);

    if (pocket_submit(&p) != 0) return -ERR_POCKET_RING_FULL;

    Result tmp;
    if (!result_wait(&tmp, timeout_ms)) {
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
    return ManifestSubmitTimeout(m, crates, crate_count, out_result, 1000);
}

/* -------------------------------------------------------------------------
 * MfCall1 — single-op convenience wrapper used by every boxlib syscall stub.
 * Builds a 1-op Manifest with optional input/output crates, submits, and
 * returns the kernel error_code (OK = 0).
 * ------------------------------------------------------------------------- */

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

    Result r;
    int rc = (timeout_ms != 0)
               ? ManifestSubmitTimeout((Manifest *)mbuf, crates, cc, &r, timeout_ms)
               : ManifestSubmit((Manifest *)mbuf, crates, cc, &r);

    if (out_actual && out_idx != CRATE_INDEX_NONE) {
        *out_actual = (uint32_t)crates[out_idx].size;
    }
    if (out_result) *out_result = r;
    return rc;
}
