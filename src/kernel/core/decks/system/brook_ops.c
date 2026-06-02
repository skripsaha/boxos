/*
 * brook_ops.c — Manifest opcode handlers for the Brook SPSC streaming
 * primitive.
 *
 * Wrappers translate boxlib API calls into Brook* kernel internals:
 *
 *   SYSTEM_OP_BROOK_OPEN     boxlib brook_open()    → BrookOpenInternal
 *   SYSTEM_OP_BROOK_RELEASE  boxlib brook_release() → BrookReleaseInternal
 *   SYSTEM_OP_BROOK_INFO     diagnostic stats snapshot
 *
 * The wait/wake path runs entirely in userspace (pause / UMWAIT / yield
 * on the shared BrookHeader). Peer death is detected by reading the
 * kernel-managed *_alive flag — set in BrookOpenInternal, cleared in
 * BrookReleaseInternal / BrookCleanupProcess. No additional syscalls
 * are needed for blocking.
 *
 * Crate convention follows bay_ops.c / touch_ops.c so the helpers
 * (read/write/string translation) stay uniform across the System Deck.
 */

#include "system_deck.h"
#include "brook.h"
#include "op_registry.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "manifest_auth.h"
#include "process.h"
#include "vmm.h"
#include "klib.h"
#include "error.h"

/* ─────────────────────────────────────────────────────────────────────
 * Crate helpers — same pattern as bay_ops/touch_ops.
 * ───────────────────────────────────────────────────────────────────── */
static const void *brook_crate_read(const Crate *c, const OpContext *ctx)
{
    if (!c || c->size == 0) return NULL;
    if (ctx && ctx->proc && ctx->proc->cabin)
        return vmm_translate_user_addr(ctx->proc->cabin,
                                       (uintptr_t)c->addr, (size_t)c->size);
    return (const void *)(uintptr_t)c->addr;
}

static void *brook_crate_write(Crate *c, const OpContext *ctx, size_t size)
{
    if (!c || c->capacity < size) return NULL;
    if (ctx && ctx->proc && ctx->proc->cabin)
        return vmm_translate_user_addr(ctx->proc->cabin,
                                       (uintptr_t)c->addr, size);
    return (void *)(uintptr_t)c->addr;
}

static error_t brook_crate_string(const Crate *c, const OpContext *ctx,
                                  char *dst, size_t dst_size)
{
    if (!c || c->size == 0 || dst_size == 0) return ERR_INVALID_ARGUMENT;
    const char *src = brook_crate_read(c, ctx);
    if (!src) return ERR_INVALID_ADDRESS;
    size_t copy = c->size < dst_size - 1 ? c->size : dst_size - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
    if (dst[0] == '\0') return ERR_INVALID_ARGUMENT;
    return OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * SYSTEM_OP_BROOK_OPEN
 *   in_crate:  tag string (null-terminated; payload-bytes == strlen+1)
 *   params:    [u32 frame_size][u32 frame_count][u32 flags]   (12 B)
 *   out_crate: 24 bytes — [u64 va_header][u64 va_slots][u32 fs][u32 fc]
 *
 * Returns OK on success.
 * ───────────────────────────────────────────────────────────────────── */
static int SysBrookOpen(const ManifestOp *op, Crate *crates,
                        uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->in_crate  == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 12)               return ERR_INVALID_ARGUMENT;

    char tag[256];
    error_t rc = brook_crate_string(&crates[op->in_crate], ctx, tag, sizeof(tag));
    if (rc != OK) return rc;

    uint32_t frame_size, frame_count, flags;
    memcpy(&frame_size,  op->params,     sizeof(uint32_t));
    memcpy(&frame_count, op->params + 4, sizeof(uint32_t));
    memcpy(&flags,       op->params + 8, sizeof(uint32_t));

    uint64_t va_header = 0, va_slots = 0;
    uint32_t out_fs = 0, out_fc = 0;
    rc = BrookOpenInternal(ctx->proc, tag, frame_size, frame_count, flags,
                           &va_header, &va_slots, &out_fs, &out_fc);
    if (rc != OK) return rc;

    Crate *out = &crates[op->out_crate];
    uint8_t *dst = (uint8_t *)brook_crate_write(out, ctx, 24);
    if (!dst) {
        BrookReleaseInternal(ctx->proc, va_header);
        return ERR_INVALID_ADDRESS;
    }
    memcpy(dst,      &va_header, sizeof(uint64_t));
    memcpy(dst + 8,  &va_slots,  sizeof(uint64_t));
    memcpy(dst + 16, &out_fs,    sizeof(uint32_t));
    memcpy(dst + 20, &out_fc,    sizeof(uint32_t));
    out->size = 24;
    return OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * SYSTEM_OP_BROOK_RELEASE
 *   params: [u64 va_header]  (8 bytes)
 * ───────────────────────────────────────────────────────────────────── */
static int SysBrookRelease(const ManifestOp *op, Crate *crates,
                           uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    uint64_t va_header;
    memcpy(&va_header, op->params, sizeof(uint64_t));
    return BrookReleaseInternal(ctx->proc, va_header);
}

/* ─────────────────────────────────────────────────────────────────────
 * SYSTEM_OP_BROOK_INFO — diagnostic snapshot.
 *   out_crate: 32 bytes — [u64 objects][u64 claims][u64 pages][u64 releases]
 * ───────────────────────────────────────────────────────────────────── */
static int SysBrookInfo(const ManifestOp *op, Crate *crates,
                        uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;

    uint64_t snap[4];
    BrookStatsSnapshot(snap);

    Crate *out = &crates[op->out_crate];
    uint8_t *dst = (uint8_t *)brook_crate_write(out, ctx, 32);
    if (!dst) return ERR_INVALID_ADDRESS;
    memcpy(dst,      &snap[0], sizeof(uint64_t));
    memcpy(dst + 8,  &snap[1], sizeof(uint64_t));
    memcpy(dst + 16, &snap[2], sizeof(uint64_t));
    memcpy(dst + 24, &snap[3], sizeof(uint64_t));
    out->size = 32;
    return OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * Registration. Called from SystemDeckRegister.
 * ───────────────────────────────────────────────────────────────────── */
error_t BrookOpsRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        { SYSTEM_OP_BROOK_OPEN,    SysBrookOpen,    OP_AUTH_APP, "system.brook.open"    },
        { SYSTEM_OP_BROOK_RELEASE, SysBrookRelease, OP_AUTH_APP, "system.brook.release" },
        { SYSTEM_OP_BROOK_INFO,    SysBrookInfo,    OP_AUTH_APP, "system.brook.info"    },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_SYSTEM, table[i].opcode),
                                        table[i].handler,
                                        table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[BrookOps] register %s failed: %s\n",
                    table[i].name, ErrorString(rc));
            return rc;
        }
    }
    debug_printf("[BrookOps] registered %zu brook ops\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}
