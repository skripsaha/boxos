
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
        BayReleaseInternal(ctx->proc, user_va);
        return ERR_INVALID_ADDRESS;
    }
    return OK;
}

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