
#include "klib.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "crate_io.h"
#include "operations_deck.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"

#define OPS_MAX_BYTES (1u << 20)

static vmm_context_t *op_vmm(const OpContext *ctx)
{
    return (ctx && ctx->proc && ctx->proc->cabin) ? ctx->proc->cabin->vmm : NULL;
}


static int OpBufMove(const ManifestOp *op,
                     Crate            *crates,
                     uint16_t          crate_count,
                     const OpContext  *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE || op->out_crate == CRATE_INDEX_NONE) {
        return ERR_INVALID_ARGUMENT;
    }
    Crate *src = &crates[op->in_crate];
    Crate *dst = &crates[op->out_crate];
    if (src->size > dst->capacity) return ERR_BUFFER_TOO_SMALL;
    if (src->size > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;

    void *kbuf = crate_in_buf(src, ctx);
    if (!kbuf) return ERR_INVALID_ADDRESS;

    int rc = crate_out_commit(dst, ctx, kbuf, src->size);
    crate_buf_free(kbuf);
    if (rc != OK) return rc;
    dst->size = src->size;
    return OK;
}


static int OpBufFill(const ManifestOp *op,
                     Crate            *crates,
                     uint16_t          crate_count,
                     const OpContext  *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 1)                 return ERR_INVALID_ARGUMENT;

    Crate   *dst       = &crates[op->out_crate];
    uint8_t  fill_byte = op->params[0];
    uint64_t n         = dst->capacity;
    if (n == 0) return ERR_INVALID_ADDRESS;
    if (n > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;

    vmm_context_t *vmm = op_vmm(ctx);
    if (vmm) {
        uint8_t *page = kmalloc(PMM_PAGE_SIZE);
        if (!page) return ERR_NO_MEMORY;
        memset(page, fill_byte, PMM_PAGE_SIZE);
        for (uint64_t off = 0; off < n; off += PMM_PAGE_SIZE) {
            uint64_t chunk = n - off;
            if (chunk > PMM_PAGE_SIZE) chunk = PMM_PAGE_SIZE;
            error_t rc = vmm_user_buf_commit_out(vmm, (uintptr_t)dst->addr + off,
                                                 page, (size_t)chunk);
            if (rc != OK) { kfree(page); return rc; }
        }
        kfree(page);
    } else {
        memset((void *)(uintptr_t)dst->addr, fill_byte, (size_t)n);
    }
    dst->size = n;
    return OK;
}


static int OpBufXor(const ManifestOp *op,
                    Crate            *crates,
                    uint16_t          crate_count,
                    const OpContext  *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE || op->out_crate == CRATE_INDEX_NONE) {
        return ERR_INVALID_ARGUMENT;
    }
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;

    uint32_t key_len = *(const uint32_t *)op->params;
    if (key_len == 0)                         return ERR_INVALID_ARGUMENT;
    if ((uint64_t)4 + key_len > op->param_size) return ERR_INVALID_ARGUMENT;
    const uint8_t *key = op->params + 4;

    Crate *src = &crates[op->in_crate];
    Crate *dst = &crates[op->out_crate];
    if (src->size > dst->capacity) return ERR_BUFFER_TOO_SMALL;
    if (src->size > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;

    uint8_t *in = crate_in_buf(src, ctx);
    if (!in) return ERR_INVALID_ADDRESS;
    uint8_t *out = crate_out_alloc(dst, src->size);
    if (!out) { crate_buf_free(in); return ERR_INVALID_ADDRESS; }

    for (uint64_t i = 0; i < src->size; i++) {
        out[i] = in[i] ^ key[i % key_len];
    }

    int rc = crate_out_commit(dst, ctx, out, src->size);
    crate_buf_free(in);
    crate_buf_free(out);
    if (rc != OK) return rc;
    dst->size = src->size;
    return OK;
}


static int OpBufHash(const ManifestOp *op,
                     Crate            *crates,
                     uint16_t          crate_count,
                     const OpContext  *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE || op->out_crate == CRATE_INDEX_NONE) {
        return ERR_INVALID_ARGUMENT;
    }
    Crate *src = &crates[op->in_crate];
    Crate *dst = &crates[op->out_crate];
    if (dst->capacity < 4)        return ERR_BUFFER_TOO_SMALL;
    if (src->size > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;

    uint8_t *in = crate_in_buf(src, ctx);
    if (!in) return ERR_INVALID_ADDRESS;

    uint32_t hash = 0;
    for (uint64_t i = 0; i < src->size; i++) {
        hash += in[i];
        hash = (hash << 5) | (hash >> 27);
    }
    crate_buf_free(in);

    return crate_write(dst, ctx, &hash, 4);
}


static int OpBufCmp(const ManifestOp *op,
                    Crate            *crates,
                    uint16_t          crate_count,
                    const OpContext  *ctx)
{
    if (op->in_crate == CRATE_INDEX_NONE || op->out_crate == CRATE_INDEX_NONE) {
        return ERR_INVALID_ARGUMENT;
    }
    if (op->param_size < 2) return ERR_INVALID_ARGUMENT;

    uint16_t b_idx = *(const uint16_t *)op->params;
    if (b_idx >= crate_count) return ERR_INVALID_ARGUMENT;

    Crate *a = &crates[op->in_crate];
    Crate *b = &crates[b_idx];
    Crate *r = &crates[op->out_crate];
    if (r->capacity < 4) return ERR_BUFFER_TOO_SMALL;
    if (a->size > OPS_MAX_BYTES || b->size > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;

    uint8_t *a_kp = crate_in_buf(a, ctx);
    if (!a_kp) return ERR_INVALID_ADDRESS;
    uint8_t *b_kp = crate_in_buf(b, ctx);
    if (!b_kp) { crate_buf_free(a_kp); return ERR_INVALID_ADDRESS; }

    uint64_t cmp_len = (a->size < b->size) ? a->size : b->size;
    int      cmp     = memcmp(a_kp, b_kp, (size_t)cmp_len);
    int32_t  result;
    if (cmp != 0)                result = (cmp < 0) ? -1 : 1;
    else if (a->size != b->size) result = (a->size < b->size) ? -1 : 1;
    else                         result = 0;

    int rc = crate_write(r, ctx, &result, 4);
    crate_buf_free(a_kp);
    crate_buf_free(b_kp);
    return rc;
}


static int OpBufFind(const ManifestOp *op,
                     Crate            *crates,
                     uint16_t          crate_count,
                     const OpContext  *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE || op->out_crate == CRATE_INDEX_NONE) {
        return ERR_INVALID_ARGUMENT;
    }
    if (op->param_size < 4) return ERR_INVALID_ARGUMENT;

    uint32_t pattern_len = *(const uint32_t *)op->params;
    if (pattern_len == 0)                     return ERR_INVALID_ARGUMENT;
    if ((uint64_t)4 + pattern_len > op->param_size) return ERR_INVALID_ARGUMENT;
    const uint8_t *pattern = op->params + 4;

    Crate *hay = &crates[op->in_crate];
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 4)         return ERR_BUFFER_TOO_SMALL;
    if (hay->size > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;

    uint8_t *hay_kp = crate_in_buf(hay, ctx);
    if (!hay_kp) return ERR_INVALID_ADDRESS;

    uint32_t found = 0xFFFFFFFFu;
    if (hay->size >= pattern_len) {
        uint64_t end = hay->size - pattern_len;
        for (uint64_t i = 0; i <= end; i++) {
            if (memcmp(hay_kp + i, pattern, pattern_len) == 0) {
                found = (uint32_t)i;
                break;
            }
        }
    }
    crate_buf_free(hay_kp);

    return crate_write(out, ctx, &found, 4);
}


static int OpBufPack(const ManifestOp *op,
                     Crate            *crates,
                     uint16_t          crate_count,
                     const OpContext  *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE || op->out_crate == CRATE_INDEX_NONE) {
        return ERR_INVALID_ARGUMENT;
    }
    Crate *src = &crates[op->in_crate];
    Crate *dst = &crates[op->out_crate];
    if (src->size > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;

    uint8_t *in = crate_in_buf(src, ctx);
    if (!in) return ERR_INVALID_ADDRESS;

    uint64_t worst   = src->size * 2;
    uint64_t out_cap = (worst < dst->capacity) ? worst : dst->capacity;
    uint8_t *out = crate_out_alloc(dst, out_cap);
    if (!out) { crate_buf_free(in); return ERR_INVALID_ADDRESS; }

    uint64_t o = 0;
    uint64_t i = 0;
    while (i < src->size) {
        uint8_t  byte  = in[i];
        uint64_t count = 1;
        while (i + count < src->size && count < 255 && in[i + count] == byte) count++;

        if (o + 2 > out_cap) {
            crate_buf_free(in);
            crate_buf_free(out);
            dst->size = 0;
            return ERR_BUFFER_TOO_SMALL;
        }
        out[o++] = (uint8_t)count;
        out[o++] = byte;
        i += count;
    }

    int rc = crate_out_commit(dst, ctx, out, o);
    crate_buf_free(in);
    crate_buf_free(out);
    if (rc != OK) return rc;
    dst->size = o;
    return OK;
}


static int OpBufUnpack(const ManifestOp *op,
                       Crate            *crates,
                       uint16_t          crate_count,
                       const OpContext  *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE || op->out_crate == CRATE_INDEX_NONE) {
        return ERR_INVALID_ARGUMENT;
    }
    Crate *src = &crates[op->in_crate];
    Crate *dst = &crates[op->out_crate];
    if (src->size % 2 != 0)        return ERR_INVALID_ARGUMENT;
    if (src->size > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;

    uint8_t *in = crate_in_buf(src, ctx);
    if (!in) return ERR_INVALID_ADDRESS;

    if (dst->capacity == 0) { crate_buf_free(in); return ERR_INVALID_ADDRESS; }

    uint64_t total = 0;
    for (uint64_t i = 0; i + 1 < src->size; i += 2) total += in[i];
    if (total > dst->capacity || total > OPS_MAX_BYTES) {
        crate_buf_free(in);
        dst->size = 0;
        return ERR_BUFFER_TOO_SMALL;
    }
    if (total == 0) { crate_buf_free(in); dst->size = 0; return OK; }

    uint8_t *out = crate_out_alloc(dst, total);
    if (!out) { crate_buf_free(in); return ERR_INVALID_ADDRESS; }

    uint64_t o = 0;
    for (uint64_t i = 0; i + 1 < src->size; i += 2) {
        uint8_t count = in[i];
        uint8_t byte  = in[i + 1];
        memset(out + o, byte, count);
        o += count;
    }

    int rc = crate_out_commit(dst, ctx, out, o);
    crate_buf_free(in);
    crate_buf_free(out);
    if (rc != OK) return rc;
    dst->size = o;
    return OK;
}


static int OpBitSwap(const ManifestOp *op,
                     Crate            *crates,
                     uint16_t          crate_count,
                     const OpContext  *ctx)
{
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE || op->out_crate == CRATE_INDEX_NONE) {
        return ERR_INVALID_ARGUMENT;
    }
    if (op->param_size < 1) return ERR_INVALID_ARGUMENT;

    Crate  *src  = &crates[op->in_crate];
    Crate  *dst  = &crates[op->out_crate];
    uint8_t mode = op->params[0];
    if (src->size > dst->capacity) return ERR_BUFFER_TOO_SMALL;
    if (src->size > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;

    uint8_t *in = crate_in_buf(src, ctx);
    if (!in) return ERR_INVALID_ADDRESS;
    uint8_t *out = crate_out_alloc(dst, src->size);
    if (!out) { crate_buf_free(in); return ERR_INVALID_ADDRESS; }

    if (mode > 2) {
        crate_buf_free(in);
        crate_buf_free(out);
        return ERR_INVALID_ARGUMENT;
    }

    uint64_t n = src->size;
    memcpy(out, in, (size_t)n);
    if (mode == 0) {
        for (uint64_t i = 0; i + 1 < n; i += 2) {
            out[i]     = in[i + 1];
            out[i + 1] = in[i];
        }
    } else if (mode == 1) {
        for (uint64_t i = 0; i + 3 < n; i += 4) {
            out[i]     = in[i + 3];
            out[i + 1] = in[i + 2];
            out[i + 2] = in[i + 1];
            out[i + 3] = in[i];
        }
    } else {
        for (uint64_t i = 0; i + 7 < n; i += 8) {
            for (int j = 0; j < 4; j++) {
                out[i + j]     = in[i + 7 - j];
                out[i + 7 - j] = in[i + j];
            }
        }
    }

    int rc = crate_out_commit(dst, ctx, out, n);
    crate_buf_free(in);
    crate_buf_free(out);
    if (rc != OK) return rc;
    dst->size = n;
    return OK;
}


static int OpValAdd(const ManifestOp *op,
                    Crate            *crates,
                    uint16_t          crate_count,
                    const OpContext  *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 9)                 return ERR_INVALID_ARGUMENT;

    uint32_t offset    = *(const uint32_t *)(op->params + 0);
    uint8_t  type_size = op->params[4];
    int32_t  delta     = *(const int32_t *)(op->params + 5);

    Crate *target = &crates[op->out_crate];
    if (type_size != 1 && type_size != 2 && type_size != 4)  return ERR_INVALID_ARGUMENT;
    if ((uint64_t)offset + type_size > target->size)         return ERR_OUT_OF_RANGE;

    uintptr_t      at  = (uintptr_t)target->addr + offset;
    vmm_context_t *vmm = op_vmm(ctx);

    uint8_t scalar[4];
    if (vmm) {
        if (vmm_user_buf_in_into(vmm, at, type_size, scalar) != OK) return ERR_INVALID_ADDRESS;
    } else {
        memcpy(scalar, (const void *)at, type_size);
    }

    if (type_size == 1) {
        int8_t v;  memcpy(&v, scalar, 1);
        v = (int8_t)(v + (int8_t)delta);            memcpy(scalar, &v, 1);
    } else if (type_size == 2) {
        uint16_t v; memcpy(&v, scalar, 2);
        v = (uint16_t)((int16_t)v + (int16_t)delta); memcpy(scalar, &v, 2);
    } else {
        uint32_t v; memcpy(&v, scalar, 4);
        v = (uint32_t)((int32_t)v + delta);          memcpy(scalar, &v, 4);
    }

    if (vmm) {
        error_t rc = vmm_user_buf_commit_out(vmm, at, scalar, type_size);
        if (rc != OK) return rc;
    } else {
        memcpy((void *)at, scalar, type_size);
    }
    return OK;
}


error_t OperationsDeckRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        { OP_BUF_MOVE,   OpBufMove,   OP_AUTH_APP, "ops.move"   },
        { OP_BUF_FILL,   OpBufFill,   OP_AUTH_APP, "ops.fill"   },
        { OP_BUF_XOR,    OpBufXor,    OP_AUTH_APP, "ops.xor"    },
        { OP_BUF_HASH,   OpBufHash,   OP_AUTH_APP, "ops.hash"   },
        { OP_BUF_CMP,    OpBufCmp,    OP_AUTH_APP, "ops.cmp"    },
        { OP_BUF_FIND,   OpBufFind,   OP_AUTH_APP, "ops.find"   },
        { OP_BUF_PACK,   OpBufPack,   OP_AUTH_APP, "ops.pack"   },
        { OP_BUF_UNPACK, OpBufUnpack, OP_AUTH_APP, "ops.unpack" },
        { OP_BIT_SWAP,   OpBitSwap,   OP_AUTH_APP, "ops.bswap"  },
        { OP_VAL_ADD,    OpValAdd,    OP_AUTH_APP, "ops.vadd"   },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_OPERATIONS, table[i].opcode),
                                        table[i].handler,
                                        table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[OperationsDeck] register %s failed: %s\n",
                    table[i].name, ErrorString(rc));
            return rc;
        }
    }
    debug_printf("[OperationsDeck] registered %zu ops\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}