#include "box/core/manifest.h"
#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/string.h"
#include "box/error.h"
#include "boxos_decks.h"


int ManifestBuilderInit(ManifestBuilder *mb, void *buf, uint32_t capacity)
{
    if (!mb || !buf || capacity < sizeof(Manifest)) return -1;
    mb->buf       = (uint8_t *)buf;
    mb->capacity  = capacity;
    mb->size      = sizeof(Manifest);
    mb->op_count  = 0;
    mb->finalized = 0;

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
        p->flags |= POCKET_FLAG_ENCLOSED;
        p->manifest_addr = 0;
        memcpy(p->enclosure, m, m->total_size);
    } else {
        p->manifest_addr = (uint64_t)(uintptr_t)m;
    }
}

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
    return ManifestSubmitTimeout(m, crates, crate_count, out_result, 0);
}



int ManifestCompileHandle(const Manifest *m, ManifestHandle *out_handle)
{
    if (!m || !out_handle)             return -ERR_INVALID_ARGS;
    if (m->magic != MANIFEST_MAGIC)    return -ERR_INVALID_ARGS;

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

    result_drain_orphan_replies();

    Pocket p;
    pocket_prepare(&p);
    p.flags         = POCKET_FLAG_MANIFEST | POCKET_FLAG_MANIFEST_HANDLE;
    p.target_pid    = target_pid;
    p.manifest_addr = (uint64_t)handle;
    p.manifest_size = 0;
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
    return ManifestSubmitHandleTimeout(handle, crates, crate_count,
                                        out_result, 0);
}

int ManifestReleaseHandle(ManifestHandle handle)
{
    if (handle == 0) return -ERR_INVALID_ARGS;

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
    int rc = ManifestSubmitTimeout((Manifest *)mbuf, crates, cc, &r, timeout_ms);

    if (out_actual && out_idx != CRATE_INDEX_NONE) {
        *out_actual = (uint32_t)crates[out_idx].size;
    }
    if (out_result) *out_result = r;
    return rc;
}