/*
 * Operations Deck — Manifest-native handlers.
 *
 * Each op cleanly separates INPUT and OUTPUT through Crates. There is no
 * "single buffer with packed args" cargo cult: parameters live in op->params
 * and never overlap data buffers.
 *
 * Two-input ops (CMP, FIND, XOR-with-key) carry the second buffer via a
 * crate index encoded in op->params. ManifestOp's two crate slots (in/out)
 * cover the common case; params encode anything beyond that.
 *
 * Buffers move through the page-walked crate_io primitives (crate_in_buf /
 * crate_out_alloc / crate_out_commit, plus crate_read / crate_write for fixed
 * scalars). A Crate payload that straddles a page boundary is copied across
 * every backing frame instead of being clipped to its first page — the
 * vmm_translate_user_addr straddle bug that silently corrupted every >4 KiB
 * transform. Inputs are snapshotted into kernel buffers and each transform
 * runs kernel->kernel, which also makes the in-place-looking ops (MOVE / XOR /
 * BIT_SWAP) overlap-safe for free.
 *
 * Every op here is OP_AUTH_APP, and Crate.size / Crate.capacity are
 * attacker-controlled (CrateIsValid bounds neither), so each snapshot and each
 * output allocation is capped to OPS_MAX_BYTES — an unbounded crate_in_buf /
 * crate_out_alloc of a claimed size would be a kmalloc DoS.
 */

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

/*
 * Operations ceiling. The legacy single-page map effectively capped every op
 * at one page (4 KiB), so no real workload ever moved more; 1 MiB is far past
 * any genuine buffer transform yet small enough that an attacker-sized crate
 * cannot exhaust the kernel heap. Applied to every crate_in_buf snapshot and
 * crate_out_alloc output below.
 */
#define OPS_MAX_BYTES (1u << 20)   /* 1 MiB */

static vmm_context_t *op_vmm(const OpContext *ctx)
{
    return (ctx && ctx->proc && ctx->proc->cabin) ? ctx->proc->cabin->vmm : NULL;
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
    if (src->size > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;

    /* Snapshot the source into a kernel buffer, then commit that buffer to the
     * destination: src/dst may overlap in user space but the bounce is its own
     * allocation, so the copy is overlap-safe. */
    void *kbuf = crate_in_buf(src, ctx);
    if (!kbuf) return ERR_INVALID_ADDRESS;

    int rc = crate_out_commit(dst, ctx, kbuf, src->size);
    crate_buf_free(kbuf);
    if (rc != OK) return rc;          /* fail closed: dst->size left unchanged */
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

    Crate   *dst       = &crates[op->out_crate];
    uint8_t  fill_byte = op->params[0];
    uint64_t n         = dst->capacity;
    if (n == 0) return ERR_INVALID_ADDRESS;   /* legacy mapped capacity==0 -> NULL */
    if (n > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;   /* bound fill CPU like other ops */

    vmm_context_t *vmm = op_vmm(ctx);
    if (vmm) {
        /* One page of the fill byte committed across the user output in
         * page-sized chunks: kernel memory stays a single page no matter how
         * large dst->capacity is (it is attacker-controlled and unbounded), so
         * a giant capacity cannot force a giant kmalloc+memset. */
        uint8_t *page = kmalloc(PMM_PAGE_SIZE);
        if (!page) return ERR_NO_MEMORY;
        memset(page, fill_byte, PMM_PAGE_SIZE);
        for (uint64_t off = 0; off < n; off += PMM_PAGE_SIZE) {
            uint64_t chunk = n - off;
            if (chunk > PMM_PAGE_SIZE) chunk = PMM_PAGE_SIZE;
            error_t rc = vmm_user_buf_commit_out(vmm, (uintptr_t)dst->addr + off,
                                                 page, (size_t)chunk);
            if (rc != OK) { kfree(page); return rc; }   /* fail closed */
        }
        kfree(page);
    } else {
        memset((void *)(uintptr_t)dst->addr, fill_byte, (size_t)n);
    }
    dst->size = n;
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
    if ((uint64_t)4 + key_len > op->param_size) return ERR_INVALID_ARGUMENT; /* no u32 wrap */
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

    /* 4-byte result -> stack scalar; crate_write sets dst->size on success. */
    return crate_write(dst, ctx, &hash, 4);
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
    if (a->size > OPS_MAX_BYTES || b->size > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;

    /* Two inputs -> two snapshots. */
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
    if ((uint64_t)4 + pattern_len > op->param_size) return ERR_INVALID_ARGUMENT; /* no u32 wrap */
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
    if (src->size > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;

    uint8_t *in = crate_in_buf(src, ctx);
    if (!in) return ERR_INVALID_ADDRESS;

    /* Worst-case RLE output is two bytes per input byte; src->size is already
     * bounded, so the bounce is at most 2*OPS_MAX. Allocate the smaller of the
     * real worst case and the claimed capacity (==0 -> ERR_INVALID_ADDRESS,
     * matching the legacy capacity map). */
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
    if (src->size % 2 != 0)        return ERR_INVALID_ARGUMENT;
    if (src->size > OPS_MAX_BYTES) return ERR_BUFFER_TOO_SMALL;

    uint8_t *in = crate_in_buf(src, ctx);
    if (!in) return ERR_INVALID_ADDRESS;

    /* Legacy mapped the whole output capacity up front as an address gate. */
    if (dst->capacity == 0) { crate_buf_free(in); return ERR_INVALID_ADDRESS; }

    /* Decompressed length is the sum of the run counts. A single [count] byte
     * amplifies up to 255x, so resolve the exact size BEFORE allocating and
     * reject if it would exceed the user capacity (legacy ERR_BUFFER_TOO_SMALL)
     * or the operations ceiling (an unbounded kmalloc DoS otherwise). */
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

    /* Seed with a straight copy so any trailing bytes that do not fill a whole
     * element pass through from the source — never leak uninitialised heap. */
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
    } else {   /* mode == 2 */
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
    if ((uint64_t)offset + type_size > target->size)         return ERR_OUT_OF_RANGE; /* no u32 wrap */

    /* In-place read-modify-write of just the type_size bytes at offset, page-
     * walked so an offset past the first page lands on the right frame. */
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
