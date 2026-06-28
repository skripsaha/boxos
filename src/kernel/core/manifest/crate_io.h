#ifndef CRATE_IO_H
#define CRATE_IO_H

#include "ktypes.h"
#include "error.h"
#include "boxos_crate.h"
#include "op_registry.h"   /* OpContext */

/*
 * crate_io — page-walked Crate <-> kernel-buffer copy primitives.
 *
 * Deck handlers move bytes between a user Crate and a kernel buffer through
 * here instead of mapping the crate's first page with vmm_translate_user_addr
 * (which fails closed on a page-straddling range). crate_read / crate_write
 * copy fixed-size payloads via the page-by-page vmm_user_buf_* walk, so a
 * Crate whose payload crosses a page boundary is copied correctly. The
 * crate_in_buf / crate_out_* helpers carry the variable-size bounce-buffer
 * pattern for outputs whose length is only known once the handler has built
 * them.
 */

/* Fixed-size SNAPSHOT read: user crate -> caller's kernel buffer.
 *   n > c->size            -> ERR_INVALID_ARGUMENT
 *   bad user address       -> ERR_INVALID_ADDRESS
 *   ctx == NULL / no cabin -> direct memcpy from c->addr (kernel selftest) */
error_t crate_read(const Crate *c, const OpContext *ctx, void *dst, uint64_t n);

/* Fixed-size ALL-OR-NOTHING write: caller's kernel buffer -> user crate.
 *   n > c->capacity        -> ERR_INVALID_ARGUMENT
 *   on success sets c->size = n
 *   ctx == NULL / no cabin -> direct memcpy to c->addr (kernel selftest) */
error_t crate_write(Crate *c, const OpContext *ctx, const void *src, uint64_t n);

/* Variable-size bounce helpers. Each walks the user page table page-by-page,
 * so arbitrary cross-page ranges are handled. Used for outputs whose size is
 * only known after the handler serializes its record. */
void *crate_in_buf   (const Crate *src, const OpContext *ctx);
void *crate_out_alloc(const Crate *out, uint64_t bytes);
int   crate_out_commit(const Crate *out, const OpContext *ctx,
                       const void *kbuf, uint64_t bytes);
void  crate_buf_free (void *kbuf);

#endif /* CRATE_IO_H */
