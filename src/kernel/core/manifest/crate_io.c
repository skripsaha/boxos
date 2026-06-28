/*
 * crate_io — page-walked Crate <-> kernel-buffer copy primitives.
 *
 * crate_read / crate_write copy a fixed-size payload between a user Crate and
 * a caller kernel buffer using the no-heap page-walk in vmm_user_buf_in_into /
 * vmm_user_buf_commit_out. A Crate payload that straddles a page boundary is
 * copied across every backing frame instead of being clipped to the first
 * page, which is what a raw vmm_translate_user_addr would do.
 *
 * crate_in_buf / crate_out_alloc / crate_out_commit / crate_buf_free are the
 * variable-size bounce helpers used by handlers that serialize a record whose
 * length is only known after it is built.
 */

#include "crate_io.h"
#include "klib.h"
#include "vmm.h"
#include "process.h"

error_t crate_read(const Crate *c, const OpContext *ctx, void *dst, uint64_t n)
{
    if (!c || !dst)  return ERR_INVALID_ARGUMENT;
    if (n > c->size) return ERR_INVALID_ARGUMENT;
    if (n == 0)      return OK;

    if (ctx && ctx->proc && ctx->proc->cabin) {
        return vmm_user_buf_in_into(ctx->proc->cabin->vmm,
                                    (uintptr_t)c->addr, (size_t)n, dst);
    }
    /* No cabin (kernel-internal selftest path). */
    memcpy(dst, (const void *)(uintptr_t)c->addr, (size_t)n);
    return OK;
}

error_t crate_write(Crate *c, const OpContext *ctx, const void *src, uint64_t n)
{
    if (!c || !src)      return ERR_INVALID_ARGUMENT;
    if (n > c->capacity) return ERR_INVALID_ARGUMENT;
    if (n == 0) { c->size = 0; return OK; }

    if (ctx && ctx->proc && ctx->proc->cabin) {
        error_t rc = vmm_user_buf_commit_out(ctx->proc->cabin->vmm,
                                             (uintptr_t)c->addr, src, (size_t)n);
        if (rc != OK) return rc;   /* fail closed — leave size unchanged */
    } else {
        memcpy((void *)(uintptr_t)c->addr, src, (size_t)n);
    }
    c->size = n;
    return OK;
}

void *crate_in_buf(const Crate *src, const OpContext *ctx)
{
    if (!src || src->size == 0) return NULL;
    if (src->size > src->capacity) return NULL;
    if (ctx && ctx->proc && ctx->proc->cabin) {
        return vmm_user_buf_in(ctx->proc->cabin->vmm, (uintptr_t)src->addr, (size_t)src->size);
    }
    /* No cabin (kernel-internal caller) — snapshot the bytes so cleanup is
     * uniform. */
    void *kbuf = kmalloc((size_t)src->size);
    if (!kbuf) return NULL;
    memcpy(kbuf, (const void *)(uintptr_t)src->addr, (size_t)src->size);
    return kbuf;
}

void *crate_out_alloc(const Crate *out, uint64_t bytes)
{
    if (!out || bytes == 0) return NULL;
    if (bytes > out->capacity) return NULL;
    return vmm_user_buf_alloc_out((size_t)bytes);
}

int crate_out_commit(const Crate *out, const OpContext *ctx,
                     const void *kbuf, uint64_t bytes)
{
    if (!out || !kbuf || bytes == 0) return 0;
    if (ctx && ctx->proc && ctx->proc->cabin) {
        return vmm_user_buf_commit_out(ctx->proc->cabin->vmm, (uintptr_t)out->addr,
                                        kbuf, (size_t)bytes);
    }
    memcpy((void *)(uintptr_t)out->addr, kbuf, (size_t)bytes);
    return 0;
}

void crate_buf_free(void *kbuf)
{
    if (kbuf) vmm_user_buf_free(kbuf);
}
