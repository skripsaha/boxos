#include "manifest_selftest.h"
#include "manifest.h"
#include "manifest_exec.h"
#include "op_registry.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "klib.h"

#define TEST_OP_COPY  0xFE00u
#define TEST_OP_FILL  0xFE01u


static int test_op_copy(const ManifestOp *op,
                        Crate            *crates,
                        uint16_t          crate_count,
                        const OpContext  *ctx)
{
    (void)ctx;
    (void)crate_count;
    if (op->in_crate == CRATE_INDEX_NONE || op->out_crate == CRATE_INDEX_NONE) {
        return ERR_INVALID_ARGUMENT;
    }
    Crate *in  = &crates[op->in_crate];
    Crate *out = &crates[op->out_crate];

    uint64_t n = in->size;
    if (n > out->capacity) return ERR_BUFFER_TOO_SMALL;

    const uint8_t *src = (const uint8_t *)(uintptr_t)in->addr;
    uint8_t       *dst = (uint8_t *)(uintptr_t)out->addr;
    for (uint64_t i = 0; i < n; i++) dst[i] = src[i];
    out->size = n;
    return OK;
}

static int test_op_fill(const ManifestOp *op,
                        Crate            *crates,
                        uint16_t          crate_count,
                        const OpContext  *ctx)
{
    (void)ctx;
    (void)crate_count;
    if (op->out_crate == CRATE_INDEX_NONE)         return ERR_INVALID_ARGUMENT;
    if (op->param_size < 1)                        return ERR_INVALID_ARGUMENT;

    uint8_t fill_byte = op->params[0];
    Crate  *out       = &crates[op->out_crate];
    uint8_t *dst      = (uint8_t *)(uintptr_t)out->addr;

    for (uint64_t i = 0; i < out->capacity; i++) dst[i] = fill_byte;
    out->size = out->capacity;
    return OK;
}


static error_t register_test_ops_once(void)
{
    error_t rc;
    rc = OpRegistryRegister(OP_KIND(DECK_OPERATIONS, TEST_OP_COPY),
                            test_op_copy, 0, "test.copy");
    if (rc != OK && rc != ERR_ALREADY_EXISTS) return rc;

    rc = OpRegistryRegister(OP_KIND(DECK_OPERATIONS, TEST_OP_FILL),
                            test_op_fill, 0, "test.fill");
    if (rc != OK && rc != ERR_ALREADY_EXISTS) return rc;

    return OK;
}

static error_t build_test_manifest(uint8_t **out_buf, uint32_t *out_size)
{
    const uint32_t total = 16u + 13u + 12u;
    uint8_t *buf = kmalloc(total);
    if (!buf) return ERR_NO_MEMORY;

    Manifest *m = (Manifest *)buf;
    m->magic       = MANIFEST_MAGIC;
    m->version     = MANIFEST_VERSION;
    m->op_count    = 2;
    m->flags       = 0;
    m->total_size  = total;

    ManifestOp *op0 = (ManifestOp *)(buf + 16);
    op0->op_kind    = OP_KIND(DECK_OPERATIONS, TEST_OP_FILL);
    op0->flags      = 0;
    op0->in_crate   = CRATE_INDEX_NONE;
    op0->out_crate  = 0;
    op0->param_size = 1;
    op0->params[0]  = 0xABu;

    ManifestOp *op1 = (ManifestOp *)(buf + 16 + 13);
    op1->op_kind    = OP_KIND(DECK_OPERATIONS, TEST_OP_COPY);
    op1->flags      = 0;
    op1->in_crate   = 0;
    op1->out_crate  = 1;
    op1->param_size = 0;

    *out_buf  = buf;
    *out_size = total;
    return OK;
}

error_t ManifestSelfTest(void)
{
    debug_printf("[Manifest][selftest] starting\n");

    error_t rc = register_test_ops_once();
    if (rc != OK) {
        kprintf("[Manifest][selftest] register_test_ops failed: %s\n", ErrorString(rc));
        return rc;
    }

    uint8_t *raw      = NULL;
    uint32_t raw_size = 0;
    rc = build_test_manifest(&raw, &raw_size);
    if (rc != OK) {
        kprintf("[Manifest][selftest] build_test_manifest failed: %s\n", ErrorString(rc));
        return rc;
    }

    ManifestHandle handle = MANIFEST_HANDLE_INVALID;
    rc = ManifestCompile(NULL, raw, raw_size, true, &handle);
    kfree(raw);
    if (rc != OK) {
        kprintf("[Manifest][selftest] ManifestCompile failed: %s\n", ErrorString(rc));
        return rc;
    }

    const uint64_t payload_bytes = 32;
    uint8_t *buf_a = kmalloc(payload_bytes);
    uint8_t *buf_b = kmalloc(payload_bytes);
    if (!buf_a || !buf_b) {
        if (buf_a) kfree(buf_a);
        if (buf_b) kfree(buf_b);
        ManifestRelease(handle);
        return ERR_NO_MEMORY;
    }
    for (uint64_t i = 0; i < payload_bytes; i++) buf_a[i] = 0x11u;
    for (uint64_t i = 0; i < payload_bytes; i++) buf_b[i] = 0x22u;

    Crate crates[2];
    crates[0].magic    = CRATE_MAGIC;
    crates[0].flags    = 0;
    crates[0].addr     = (uint64_t)(uintptr_t)buf_a;
    crates[0].size     = payload_bytes;
    crates[0].capacity = payload_bytes;
    crates[0].kind     = CRATE_KIND_INOUT;
    crates[0]._pad0    = 0;
    crates[0]._pad1    = 0;

    crates[1].magic    = CRATE_MAGIC;
    crates[1].flags    = 0;
    crates[1].addr     = (uint64_t)(uintptr_t)buf_b;
    crates[1].size     = 0;
    crates[1].capacity = payload_bytes;
    crates[1].kind     = CRATE_KIND_OUTPUT;
    crates[1]._pad0    = 0;
    crates[1]._pad1    = 0;

    OpContext ctx = {0};
    ctx.proc       = NULL;
    ctx.target_pid = 0;
    ctx.flags      = 0;
    ctx.pier_id    = 0;

    ManifestExecResult result;
    rc = ManifestExecute(handle, crates, 2, &ctx, &result);

    bool ok = (rc == OK)
           && (result.error_code == OK)
           && (result.completed_ops == 2)
           && (result.total_ops == 2);

    if (ok) {
        for (uint64_t i = 0; i < payload_bytes; i++) {
            if (buf_a[i] != 0xABu) { ok = false; break; }
            if (buf_b[i] != 0xABu) { ok = false; break; }
        }
    }

    if (ok) {
        kprintf("[Manifest][selftest] PASS — 2 ops, 32 bytes filled+copied\n");
    } else {
        kprintf("[Manifest][selftest] FAIL rc=%d err=%u completed=%u/%u\n",
                rc, result.error_code, result.completed_ops, result.total_ops);
    }

    kfree(buf_a);
    kfree(buf_b);
    ManifestRelease(handle);

    return ok ? OK : ERR_INTERNAL;
}