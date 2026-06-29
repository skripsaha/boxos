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
 * Crate convention follows bay_ops.c / touch_ops.c: the tag in_crate is
 * read and fixed-size outputs are written through the page-walking
 * crate_io API, so a payload that straddles a page boundary is copied
 * correctly across the whole System Deck.
 */

#include "system_deck.h"
#include "brook.h"
#include "op_registry.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "crate_io.h"
#include "manifest_auth.h"
#include "process.h"
#include "klib.h"
#include "error.h"

/* ─────────────────────────────────────────────────────────────────────
 * Tag string read — page-walked snapshot of the in_crate tag into a
 * bounded stack buffer via crate_io. A tag that straddles a page
 * boundary is copied across every backing frame; the read is capped at
 * the buffer size, so an attacker-set in_crate size can neither overflow
 * the buffer nor drive an oversized allocation.
 * ───────────────────────────────────────────────────────────────────── */
static error_t brook_crate_string(const Crate *c, const OpContext *ctx,
                                  char *dst, size_t dst_size)
{
    if (!c || c->size == 0 || dst_size == 0) return ERR_INVALID_ARGUMENT;
    size_t copy = c->size < dst_size - 1 ? c->size : dst_size - 1;
    error_t rc = crate_read(c, ctx, dst, copy);
    if (rc != OK) return rc;
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

    uint8_t blob[24];
    memcpy(blob,      &va_header, sizeof(uint64_t));
    memcpy(blob + 8,  &va_slots,  sizeof(uint64_t));
    memcpy(blob + 16, &out_fs,    sizeof(uint32_t));
    memcpy(blob + 20, &out_fc,    sizeof(uint32_t));

    Crate *out = &crates[op->out_crate];
    if (crate_write(out, ctx, blob, 24) != OK) {
        BrookReleaseInternal(ctx->proc, va_header);
        return ERR_INVALID_ADDRESS;
    }
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
    if (crate_write(out, ctx, snap, sizeof(snap)) != OK)
        return ERR_INVALID_ADDRESS;
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
