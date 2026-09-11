#ifndef CRATE_IO_H
#define CRATE_IO_H

#include "ktypes.h"
#include "error.h"
#include "boxos_crate.h"
#include "op_registry.h"


error_t crate_read(const Crate *c, const OpContext *ctx, void *dst, uint64_t n);

error_t crate_write(Crate *c, const OpContext *ctx, const void *src, uint64_t n);

void *crate_in_buf   (const Crate *src, const OpContext *ctx);
void *crate_out_alloc(const Crate *out, uint64_t bytes);
int   crate_out_commit(const Crate *out, const OpContext *ctx,
                       const void *kbuf, uint64_t bytes);
void  crate_buf_free (void *kbuf);

#endif