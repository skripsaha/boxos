/*
 * clipboard_ops.c — SYSTEM_OP_CLIPBOARD_*: the machine's list of what was
 * copied, as a program reaches it. The list is core/clipboard; the crates
 * each op takes and answers are told in boxos_decks.h.
 *
 * A clip's bytes travel a piece at a time between the program's pages and
 * the clip's own leaves (crate_read_at, crate_out_commit_at): however long a
 * clip is, the kernel never stages a copy of it on the way.
 */

#include "system_deck.h"
#include "clipboard.h"
#include "crate_io.h"
#include "op_registry.h"
#include "manifest_auth.h"
#include "boxos_manifest.h"
#include "process.h"
#include "klib.h"

/* Records gathered under one hold of the list's lock: a list answer is made
 * of pieces this long, so no hold walks further than one piece. */
#define CLIPBOARD_LIST_PIECE 128u

typedef struct {
    Crate            *crate;
    const OpContext  *ctx;
} CrateSide;

static error_t FillFromCrate(void *source, uint64_t offset, void *into, uint64_t n)
{
    const CrateSide *side = source;
    return crate_read_at(side->crate, side->ctx, offset, into, n);
}

static error_t PourIntoCrate(void *sink, uint64_t offset, const void *from, uint64_t n)
{
    const CrateSide *side = sink;
    return (error_t)crate_out_commit_at(side->crate, side->ctx, offset, from, n);
}

static uint64_t ParamU64(const ManifestOp *op, uint32_t at)
{
    uint64_t value;
    memcpy(&value, op->params + at, sizeof(value));
    return value;
}

static uint32_t ParamU32(const ManifestOp *op, uint32_t at)
{
    uint32_t value;
    memcpy(&value, op->params + at, sizeof(value));
    return value;
}

/* The out crate when the op was given one at least `least` bytes long. */
static Crate *OutCrate(const ManifestOp *op, Crate *crates, uint64_t least)
{
    if (op->out_crate == CRATE_INDEX_NONE) return NULL;
    Crate *out = &crates[op->out_crate];
    return out->capacity >= least ? out : NULL;
}

/* A number the program does not get back — its crate refused it — leaves
 * the clip in the list all the same: it is the person's copy, and list
 * names it. */
static int SysClipboardPut(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc || !ctx->proc->cabin) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 8)                       return ERR_INVALID_ARGUMENT;
    if (op->in_crate == CRATE_INDEX_NONE)          return ERR_INVALID_ARGUMENT;

    Crate *in = &crates[op->in_crate];
    if (in->size == 0 || in->size > in->capacity) return ERR_INVALID_ARGUMENT;

    CrateSide source = { in, ctx };
    uint64_t  number = 0;
    error_t   rc     = ClipboardPut(in->size, FillFromCrate, &source, ParamU32(op, 0), ParamU32(op, 4),
                                    ctx->proc->cabin, &number);
    if (rc != OK) return rc;

    Crate *out = OutCrate(op, crates, sizeof(number));
    return out ? crate_write(out, ctx, &number, sizeof(number)) : OK;
}

/* The answer is written piece by piece behind the room left for its head,
 * and the head last, once it is known how many records it carries. */
static int SysClipboardList(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)  return ERR_INVALID_ARGUMENT;
    if (op->param_size < 8)  return ERR_INVALID_ARGUMENT;

    Crate *out = OutCrate(op, crates, sizeof(ClipboardPage));
    if (!out) return ERR_BUFFER_TOO_SMALL;

    ClipboardRecord *piece = kmalloc(CLIPBOARD_LIST_PIECE * sizeof(ClipboardRecord));
    if (!piece) return ERR_NO_MEMORY;

    const uint64_t room = (out->capacity - sizeof(ClipboardPage)) / sizeof(ClipboardRecord);
    ClipboardPage  page = {0};
    uint64_t       from = ParamU64(op, 0);
    int            rc   = OK;
    do {
        const uint64_t left = room - page.delivered;
        const uint32_t ask  = left < CLIPBOARD_LIST_PIECE ? (uint32_t)left : CLIPBOARD_LIST_PIECE;
        const uint32_t got  = ClipboardList(from, piece, ask, &page.next, &page.clips, &page.bytes);
        rc = crate_out_commit_at(out, ctx, sizeof(ClipboardPage) + page.delivered * sizeof(ClipboardRecord),
                                 piece, got * sizeof(ClipboardRecord));
        page.delivered += got;
        from = page.next;
    } while (rc == OK && page.next != 0 && page.delivered < room);
    kfree(piece);

    if (rc == OK) rc = crate_out_commit_at(out, ctx, 0, &page, sizeof(page));
    if (rc != OK) return rc;
    out->size = sizeof(ClipboardPage) + page.delivered * sizeof(ClipboardRecord);
    return OK;
}

static int SysClipboardTake(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)  return ERR_INVALID_ARGUMENT;
    if (op->param_size < 16) return ERR_INVALID_ARGUMENT;

    Crate *out = OutCrate(op, crates, 1);
    if (!out) return ERR_BUFFER_TOO_SMALL;

    CrateSide sink  = { out, ctx };
    uint64_t  given = 0;
    error_t   rc    = ClipboardTake(ParamU64(op, 0), ParamU64(op, 8), out->capacity, PourIntoCrate, &sink, &given);
    if (rc != OK) return rc;
    out->size = given;
    return OK;
}

static int SysClipboardDrop(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count; (void)ctx;
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;
    return ClipboardDrop(ParamU64(op, 0));
}

static int SysClipboardClear(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    const uint64_t went = ClipboardClear();
    Crate         *out  = OutCrate(op, crates, sizeof(went));
    return out ? crate_write(out, ctx, &went, sizeof(went)) : OK;
}

error_t ClipboardOpsRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        { SYSTEM_OP_CLIPBOARD_PUT,   SysClipboardPut,   OP_AUTH_APP, "system.clipboard.put"   },
        { SYSTEM_OP_CLIPBOARD_LIST,  SysClipboardList,  OP_AUTH_APP, "system.clipboard.list"  },
        { SYSTEM_OP_CLIPBOARD_TAKE,  SysClipboardTake,  OP_AUTH_APP, "system.clipboard.take"  },
        { SYSTEM_OP_CLIPBOARD_DROP,  SysClipboardDrop,  OP_AUTH_APP, "system.clipboard.drop"  },
        { SYSTEM_OP_CLIPBOARD_CLEAR, SysClipboardClear, OP_AUTH_APP, "system.clipboard.clear" },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_SYSTEM, table[i].opcode), table[i].handler, table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[Clipboard] register %s failed: %s\n", table[i].name, ErrorString(rc));
            return rc;
        }
    }
    return OK;
}
