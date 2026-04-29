/*
 * Operations Deck — Manifest-native handlers.
 *
 * Each op cleanly separates INPUT and OUTPUT through Crates. There is no
 * "single buffer with packed args" cargo cult: parameters live in op->params
 * and never overlap data buffers. Sizes are bounded only by Crate.capacity
 * (which is itself runtime, never compile-time).
 *
 * Two-input ops (CMP, FIND, XOR-with-key) carry the second buffer via a
 * crate index encoded in op->params. ManifestOp's two crate slots (in/out)
 * cover the common case; params encode anything beyond that.
 */

#include "klib.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "operations_deck.h"
#include "vmm.h"
#include "process.h"

/* -------------------------------------------------------------------------
 * Crate access helpers — uniform translation for kernel and user pointers.
 *
 * When ctx->proc is set, the Crate.addr is a user vaddr in proc->cabin and is
 * translated through the VMM. When ctx->proc is NULL (kernel self-tests, or
 * future kernel-issued manifests), Crate.addr is taken as a direct kernel
 * pointer. Returning NULL means the caller must abort with ERR_INVALID_ADDRESS.
 * ------------------------------------------------------------------------- */

static void *OpCrateMap(const Crate *c, const OpContext *ctx, uint64_t bytes)
{
    if (!c || bytes == 0)            return NULL;
    if (bytes > c->capacity)         return NULL;
    if (ctx && ctx->proc && ctx->proc->cabin) {
        return vmm_translate_user_addr(ctx->proc->cabin, (uintptr_t)c->addr, (size_t)bytes);
    }
    return (void *)(uintptr_t)c->addr;
}

static const void *OpCrateMapRead(const Crate *c, const OpContext *ctx)
{
    return OpCrateMap(c, ctx, c->size);
}

/* -------------------------------------------------------------------------
 * BUF_MOVE — copy in_crate.size bytes into out_crate (overlap-safe).
 * params: none.
 * ------------------------------------------------------------------------- */

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

    const void *src_kp = OpCrateMapRead(src, ctx);
    void       *dst_kp = OpCrateMap(dst, ctx, src->size);
    if (!src_kp || !dst_kp) return ERR_INVALID_ADDRESS;

    memmove(dst_kp, src_kp, (size_t)src->size);
    dst->size = src->size;
    return OK;
}

/* -------------------------------------------------------------------------
 * BUF_FILL — fill out_crate.capacity bytes with a constant.
 * params: [u8 fill_byte].
 * ------------------------------------------------------------------------- */

static int OpBufFill(const ManifestOp *op,
                     Crate            *crates,
                     uint16_t          crate_count,
                     const OpContext  *ctx)
{
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 1)                 return ERR_INVALID_ARGUMENT;

    Crate  *dst       = &crates[op->out_crate];
    uint8_t fill_byte = op->params[0];

    void *dst_kp = OpCrateMap(dst, ctx, dst->capacity);
    if (!dst_kp) return ERR_INVALID_ADDRESS;

    memset(dst_kp, fill_byte, (size_t)dst->capacity);
    dst->size = dst->capacity;
    return OK;
}

/* -------------------------------------------------------------------------
 * BUF_XOR — XOR src bytes with a repeating key, writing into dst.
 * params: [u32 key_len][u8 key[key_len]].
 * ------------------------------------------------------------------------- */

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
    if (op->param_size < 4 + key_len)         return ERR_INVALID_ARGUMENT;
    const uint8_t *key = op->params + 4;

    Crate *src = &crates[op->in_crate];
    Crate *dst = &crates[op->out_crate];
    if (src->size > dst->capacity) return ERR_BUFFER_TOO_SMALL;

    const uint8_t *src_kp = OpCrateMapRead(src, ctx);
    uint8_t       *dst_kp = OpCrateMap(dst, ctx, src->size);
    if (!src_kp || !dst_kp) return ERR_INVALID_ADDRESS;

    for (uint64_t i = 0; i < src->size; i++) {
        dst_kp[i] = src_kp[i] ^ key[i % key_len];
    }
    dst->size = src->size;
    return OK;
}

/* -------------------------------------------------------------------------
 * BUF_HASH — ROL5 additive hash over in_crate, write 4 bytes to out_crate.
 * params: none.
 * ------------------------------------------------------------------------- */

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
    if (dst->capacity < 4) return ERR_BUFFER_TOO_SMALL;

    const uint8_t *src_kp = OpCrateMapRead(src, ctx);
    void          *dst_kp = OpCrateMap(dst, ctx, 4);
    if (!src_kp || !dst_kp) return ERR_INVALID_ADDRESS;

    uint32_t hash = 0;
    for (uint64_t i = 0; i < src->size; i++) {
        hash += src_kp[i];
        hash = (hash << 5) | (hash >> 27);
    }
    *(uint32_t *)dst_kp = hash;
    dst->size = 4;
    return OK;
}

/* -------------------------------------------------------------------------
 * BUF_CMP — compare in_crate with crates[params.crate_b_idx]; write 4-byte
 * result to out_crate (0 == equal, otherwise sign of memcmp).
 * params: [u16 crate_b_idx].
 * ------------------------------------------------------------------------- */

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

    const uint8_t *a_kp = OpCrateMapRead(a, ctx);
    const uint8_t *b_kp = OpCrateMapRead(b, ctx);
    void          *r_kp = OpCrateMap(r, ctx, 4);
    if (!a_kp || !b_kp || !r_kp) return ERR_INVALID_ADDRESS;

    uint64_t cmp_len = (a->size < b->size) ? a->size : b->size;
    int      cmp     = memcmp(a_kp, b_kp, (size_t)cmp_len);
    int32_t  result;
    if (cmp != 0)            result = (cmp < 0) ? -1 : 1;
    else if (a->size != b->size) result = (a->size < b->size) ? -1 : 1;
    else                          result = 0;

    *(int32_t *)r_kp = result;
    r->size = 4;
    return OK;
}

/* -------------------------------------------------------------------------
 * BUF_FIND — search for inline pattern inside in_crate; write 4-byte offset
 * to out_crate (0xFFFFFFFF if not found).
 * params: [u32 pattern_len][u8 pattern[pattern_len]].
 * ------------------------------------------------------------------------- */

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
    if (op->param_size < 4 + pattern_len)     return ERR_INVALID_ARGUMENT;
    const uint8_t *pattern = op->params + 4;

    Crate *hay = &crates[op->in_crate];
    Crate *out = &crates[op->out_crate];
    if (out->capacity < 4) return ERR_BUFFER_TOO_SMALL;

    const uint8_t *hay_kp = OpCrateMapRead(hay, ctx);
    void          *out_kp = OpCrateMap(out, ctx, 4);
    if (!hay_kp || !out_kp) return ERR_INVALID_ADDRESS;

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
    *(uint32_t *)out_kp = found;
    out->size = 4;
    return OK;
}

/* -------------------------------------------------------------------------
 * BUF_PACK — RLE compress in_crate into out_crate. Pairs of [count][byte];
 * out_crate.size set to compressed length.
 * params: none.
 * ------------------------------------------------------------------------- */

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

    const uint8_t *src_kp = OpCrateMapRead(src, ctx);
    uint8_t       *dst_kp = OpCrateMap(dst, ctx, dst->capacity);
    if (!src_kp || !dst_kp) return ERR_INVALID_ADDRESS;

    uint64_t out = 0;
    uint64_t i   = 0;
    while (i < src->size) {
        uint8_t  byte  = src_kp[i];
        uint64_t count = 1;
        while (i + count < src->size && count < 255 && src_kp[i + count] == byte) count++;

        if (out + 2 > dst->capacity) {
            dst->size = 0;
            return ERR_BUFFER_TOO_SMALL;
        }
        dst_kp[out++] = (uint8_t)count;
        dst_kp[out++] = byte;
        i += count;
    }
    dst->size = out;
    return OK;
}

/* -------------------------------------------------------------------------
 * BUF_UNPACK — RLE decompress in_crate into out_crate.
 * params: none.
 * ------------------------------------------------------------------------- */

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
    if (src->size % 2 != 0) return ERR_INVALID_ARGUMENT;

    const uint8_t *src_kp = OpCrateMapRead(src, ctx);
    uint8_t       *dst_kp = OpCrateMap(dst, ctx, dst->capacity);
    if (!src_kp || !dst_kp) return ERR_INVALID_ADDRESS;

    uint64_t out = 0;
    for (uint64_t i = 0; i + 1 < src->size; i += 2) {
        uint8_t count = src_kp[i];
        uint8_t byte  = src_kp[i + 1];
        if (out + count > dst->capacity) {
            dst->size = 0;
            return ERR_BUFFER_TOO_SMALL;
        }
        memset(dst_kp + out, byte, count);
        out += count;
    }
    dst->size = out;
    return OK;
}

/* -------------------------------------------------------------------------
 * BIT_SWAP — endian swap of in_crate bytes into out_crate.
 * params: [u8 mode] where 0=16-bit, 1=32-bit, 2=64-bit element width.
 * ------------------------------------------------------------------------- */

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

    const uint8_t *src_kp = OpCrateMapRead(src, ctx);
    uint8_t       *dst_kp = OpCrateMap(dst, ctx, src->size);
    if (!src_kp || !dst_kp) return ERR_INVALID_ADDRESS;

    uint64_t n = src->size;
    if (mode == 0) {
        for (uint64_t i = 0; i + 1 < n; i += 2) {
            dst_kp[i]     = src_kp[i + 1];
            dst_kp[i + 1] = src_kp[i];
        }
    } else if (mode == 1) {
        for (uint64_t i = 0; i + 3 < n; i += 4) {
            dst_kp[i]     = src_kp[i + 3];
            dst_kp[i + 1] = src_kp[i + 2];
            dst_kp[i + 2] = src_kp[i + 1];
            dst_kp[i + 3] = src_kp[i];
        }
    } else if (mode == 2) {
        for (uint64_t i = 0; i + 7 < n; i += 8) {
            for (int j = 0; j < 4; j++) {
                dst_kp[i + j]     = src_kp[i + 7 - j];
                dst_kp[i + 7 - j] = src_kp[i + j];
            }
        }
    } else {
        return ERR_INVALID_ARGUMENT;
    }
    dst->size = n;
    return OK;
}

/* -------------------------------------------------------------------------
 * VAL_ADD — atomic-style add to a value at offset in out_crate (in-place).
 * params: [u32 offset][u8 type_size][i32 delta].
 * type_size in {1, 2, 4}.
 * ------------------------------------------------------------------------- */

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
    if (offset + type_size > target->size)                    return ERR_OUT_OF_RANGE;

    uint8_t *kp = OpCrateMap(target, ctx, offset + type_size);
    if (!kp) return ERR_INVALID_ADDRESS;

    if (type_size == 1) {
        kp[offset] = (uint8_t)((int8_t)kp[offset] + (int8_t)delta);
    } else if (type_size == 2) {
        uint16_t v = *(uint16_t *)(kp + offset);
        v = (uint16_t)((int16_t)v + (int16_t)delta);
        *(uint16_t *)(kp + offset) = v;
    } else {
        uint32_t v = *(uint32_t *)(kp + offset);
        v = (uint32_t)((int32_t)v + delta);
        *(uint32_t *)(kp + offset) = v;
    }
    return OK;
}

/* -------------------------------------------------------------------------
 * Registration entry point. Called once at boot after OpRegistryInit.
 * ------------------------------------------------------------------------- */

error_t OperationsDeckRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        /* Pure-buffer ops on caller-supplied Crates: app+. */
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
