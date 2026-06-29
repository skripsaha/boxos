/*
 * Bay — Manifest opcode handlers (cross-cabin shared memory).
 *
 * The handlers translate the boxlib wrappers (bay_open / bay_release /
 * bay_size) into kernel-internal Bay* calls. Tag strings are passed via
 * the in_crate, sized scalars (va, size, flags) live in op->params.
 */

#include "system_deck.h"
#include "bay.h"
#include "op_registry.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "crate_io.h"
#include "manifest_auth.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "klib.h"
#include "error.h"

/* ─────────────────────────────────────────────────────────────────────
 * Tag string read — page-walked snapshot of the in_crate tag into a
 * bounded stack buffer via crate_io. A tag whose bytes straddle a page
 * boundary is copied across every backing frame (the old single-page
 * vmm_translate_user_addr clipped it). The read is capped at the buffer
 * size, so an attacker-set in_crate size can neither overflow the buffer
 * nor drive an oversized allocation.
 * ───────────────────────────────────────────────────────────────────── */
static error_t bay_crate_string(const Crate *c, const OpContext *ctx,
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
 * SYSTEM_OP_BAY_OPEN
 *   in_crate:  tag string (null-terminated, payload-bytes is strlen+1)
 *   params:    [u64 size][u32 flags]   (12 bytes)
 *   out_crate: 16 bytes — [u64 user_va][u64 actual_size]
 *
 * Returns OK and fills out_crate on success.
 * ───────────────────────────────────────────────────────────────────── */
static int SysBayOpen(const ManifestOp *op, Crate *crates,
                      uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->in_crate  == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 12)               return ERR_INVALID_ARGUMENT;

    char tag[256];
    error_t rc = bay_crate_string(&crates[op->in_crate], ctx, tag, sizeof(tag));
    if (rc != OK) return rc;

    uint64_t size;
    uint32_t flags;
    memcpy(&size,  op->params,     sizeof(uint64_t));
    memcpy(&flags, op->params + 8, sizeof(uint32_t));

    /* Hard size cap — prevent a runaway userspace from asking for the
     * whole physical RAM in one go. The cap is per-call; cabins can
     * still open many separate Bays. */
    if (size > BAY_MAX_OPEN_SIZE) return ERR_INVALID_ARGUMENT;

    uint64_t user_va     = 0;
    uint64_t actual_size = 0;
    rc = BayOpenInternal(ctx->proc, tag, size, flags, &user_va, &actual_size);
    if (rc != OK) return rc;

    uint8_t blob[16];
    memcpy(blob,     &user_va,     sizeof(uint64_t));
    memcpy(blob + 8, &actual_size, sizeof(uint64_t));

    Crate *out = &crates[op->out_crate];
    if (crate_write(out, ctx, blob, 16) != OK) {
        /* Caller's out_crate is unwritable — roll back the open. */
        BayReleaseInternal(ctx->proc, user_va);
        return ERR_INVALID_ADDRESS;
    }
    return OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * SYSTEM_OP_BAY_RELEASE
 *   params: [u64 user_va]  (8 bytes)
 * ───────────────────────────────────────────────────────────────────── */
static int SysBayRelease(const ManifestOp *op, Crate *crates,
                         uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 8) return ERR_INVALID_ARGUMENT;

    uint64_t user_va;
    memcpy(&user_va, op->params, sizeof(uint64_t));
    return BayReleaseInternal(ctx->proc, user_va);
}

/* ─────────────────────────────────────────────────────────────────────
 * SYSTEM_OP_BAY_SIZE
 *   params:    [u64 user_va]  (8 bytes)
 *   out_crate: 8 bytes — [u64 size]
 * ───────────────────────────────────────────────────────────────────── */
static int SysBaySize(const ManifestOp *op, Crate *crates,
                      uint16_t crate_count, const OpContext *ctx)
{
    (void)crate_count;
    if (!ctx || !ctx->proc)                return ERR_INVALID_ARGUMENT;
    if (op->out_crate == CRATE_INDEX_NONE) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 8)                return ERR_INVALID_ARGUMENT;

    uint64_t user_va;
    memcpy(&user_va, op->params, sizeof(uint64_t));
    uint64_t size = BaySizeInternal(ctx->proc, user_va);

    Crate *out = &crates[op->out_crate];
    if (crate_write(out, ctx, &size, sizeof(uint64_t)) != OK)
        return ERR_INVALID_ADDRESS;
    return OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * SYSTEM_OP_HEAP_PREFAULT — implicit huge-page hint from user-heap.
 *
 *   params: [u64 va_base][u64 size]   (16 bytes)
 *
 * Pre-back a 2 MiB-aligned VA range with 2 MiB physical pages, charged
 * to the calling cabin. The boxlib heap allocator calls this when its
 * sbrk-style growth crosses a 2 MiB threshold; the kernel maps 2 MiB
 * chunks in one shot rather than waiting for 4 KiB demand faults.
 *
 * Falls back to 4 KiB pages transparently if PMM can't deliver a 2 MiB
 * chunk for any reason (fragmentation, exhaustion). In either case the
 * VA range is fully resident on return.
 *
 * Cleanup: backing pages are reclaimed by vmm_destroy_context when the
 * process dies (the LARGE_PAGE walker in vmm.c sees VMM_FLAG_USER and
 * calls pmm_free(phys, VMM_LARGE_PAGE_2M_PAGES)). No per-process
 * tracker needed.
 *
 * VA bounds: we reject anything below CABIN_CODE_START_ADDR — the NULL
 * trap and Cabin metadata pages (PocketRing/ResultRing/TouchRing/
 * ClockBoard/CpuCaps) live there and a confused caller mapping a 2 MiB
 * leaf over those would silently demote the NULL trap. We also bound
 * the high side at CABIN_USER_VA_CANONICAL_END (bit 47 must stay 0). */
static int SysHeapPrefault(const ManifestOp *op, Crate *crates,
                           uint16_t crate_count, const OpContext *ctx)
{
    (void)crates; (void)crate_count;
    if (!ctx || !ctx->proc || !ctx->proc->cabin) return ERR_INVALID_ARGUMENT;
    if (op->param_size < 16)                     return ERR_INVALID_ARGUMENT;

    uint64_t va_base, size;
    memcpy(&va_base, op->params,     sizeof(uint64_t));
    memcpy(&size,    op->params + 8, sizeof(uint64_t));

    if (size == 0)                                    return ERR_INVALID_ARGUMENT;
    if (va_base & VMM_LARGE_PAGE_2M_MASK)             return ERR_INVALID_ARGUMENT;
    if (size    & VMM_LARGE_PAGE_2M_MASK)             return ERR_INVALID_ARGUMENT;
    if (va_base < CABIN_CODE_START_ADDR)              return ERR_INVALID_ARGUMENT;
    /* Wrap-safe upper-bound check. */
    if (size > CABIN_USER_VA_CANONICAL_END - va_base) return ERR_INVALID_ARGUMENT;

    uint64_t chunks = size / VMM_LARGE_PAGE_2M_SIZE;
    const uint64_t pte_flags = VMM_FLAGS_USER_RW | VMM_FLAG_NO_EXECUTE;

    for (uint64_t i = 0; i < chunks; i++) {
        uint64_t va = va_base + i * VMM_LARGE_PAGE_2M_SIZE;
        void *phys = pmm_alloc_zero(VMM_LARGE_PAGE_2M_PAGES);
        if (phys && vmm_map_huge_2m(ctx->proc->cabin->vmm, va, (uintptr_t)phys,
                                    pte_flags)) {
            continue;
        }
        /* Fallback: 4 KiB pages for this chunk. */
        if (phys) pmm_free(phys, VMM_LARGE_PAGE_2M_PAGES);
        for (uint64_t j = 0; j < VMM_LARGE_PAGE_2M_PAGES; j++) {
            void *p = pmm_alloc_zero(1);
            if (!p) return ERR_NO_MEMORY;
            vmm_map_result_t r = vmm_map_page(ctx->proc->cabin->vmm,
                                              va + j * PMM_PAGE_SIZE,
                                              (uintptr_t)p, pte_flags);
            if (!r.success) {
                pmm_free(p, 1);
                return ERR_NO_MEMORY;
            }
        }
    }
    return OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * Registration. Called from SystemDeckRegister.
 * ───────────────────────────────────────────────────────────────────── */
error_t BayOpsRegister(void)
{
    struct {
        uint16_t    opcode;
        OpHandler   handler;
        uint32_t    auth;
        const char *name;
    } table[] = {
        { SYSTEM_OP_BAY_OPEN,      SysBayOpen,      OP_AUTH_APP, "system.bay.open"      },
        { SYSTEM_OP_BAY_RELEASE,   SysBayRelease,   OP_AUTH_APP, "system.bay.release"   },
        { SYSTEM_OP_BAY_SIZE,      SysBaySize,      OP_AUTH_APP, "system.bay.size"      },
        { SYSTEM_OP_HEAP_PREFAULT, SysHeapPrefault, OP_AUTH_APP, "system.heap.prefault" },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        error_t rc = OpRegistryRegister(OP_KIND(DECK_SYSTEM, table[i].opcode),
                                        table[i].handler,
                                        table[i].auth,
                                        table[i].name);
        if (rc != OK && rc != ERR_ALREADY_EXISTS) {
            kprintf("[BayOps] register %s failed: %s\n",
                    table[i].name, ErrorString(rc));
            return rc;
        }
    }
    debug_printf("[BayOps] registered %zu bay/heap ops\n",
                 sizeof(table) / sizeof(table[0]));
    return OK;
}
