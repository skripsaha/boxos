#ifndef STORAGE_SETTLE_OPS_H
#define STORAGE_SETTLE_OPS_H

#include "ktypes.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "op_registry.h"
#include "use_context.h"

/* The storage deck's waits for the volume's Settle (settle_ops.c): anchor and
 * obj.write.settled. The caller waits behind its Chit; a cycle's S6 answers. */
int ObjAnchor(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjWriteSettled(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                    const OpContext *ctx);

/* A program's `use`, told to the volume: `said` is the `saying`-th word of the
 * Use Context. `out` (optional, one byte) is 1 once the volume remembers
 * exactly it. OK with the byte written now, or ERR_WOULD_BLOCK with the caller
 * parked until the cycle that carries the word settles — or is refused. */
int SettleUseRemembered(UseText *said, uint64_t saying, const OpContext *ctx, Crate *crates,
                        Crate *out);

#endif /* STORAGE_SETTLE_OPS_H */
