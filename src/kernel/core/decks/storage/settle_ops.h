#ifndef STORAGE_SETTLE_OPS_H
#define STORAGE_SETTLE_OPS_H

#include "ktypes.h"
#include "error.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "op_registry.h"
#include "use_context.h"

/* The storage deck's waits for the volume's Settle (settle_ops.c): anchor and
 * obj.write.settled. The caller waits behind its Chit; a cycle's S6 answers. */
int ObjAnchor(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);
int ObjWriteSettled(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                    const OpContext *ctx);

/* OBJ_ESCHEAT — Escheat (tagfs/settle/escheat.h), only for a cabin wearing
 * system or god: out_crate = [u64 ranges][u64 files lost][u64 blocks let go]
 * [u64 tails lost][u64 DIGESTS ranges known to read in no copy, laid anew of the data]. */
int ObjEscheat(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);

/* OBJ_CONJUGATE — Conjugate (tagfs/dedup/conjugate.h), only for a cabin wearing
 * system or god: params = [u64 fid][u64 at][u64 onto][u64 onto_at][u64 count],
 * out_crate = [u64 blocks laid][u64 block a refusal met][u64 why]. Room on its
 * way on the ribbon is waited for, and the act asked again (SettleOpForRoom). */
int ObjConjugate(const ManifestOp *op, Crate *crates, uint16_t crate_count, const OpContext *ctx);

/* A program's `use`, told to the volume: `said` is the `saying`-th word of the
 * Use Context. `out` (optional, one byte) is 1 once the volume remembers
 * exactly it. OK with the byte written now, or ERR_WOULD_BLOCK with the caller
 * parked until the cycle that carries the word settles — or is refused. */
int SettleUseRemembered(UseText *said, uint64_t saying, const OpContext *ctx, Crate *crates,
                        Crate *out);

/*
 * A storage deck handler whose operation the volume answered ERR_WOULD_BLOCK —
 * room a Settle cycle gives back, nothing of the operation done — hands it
 * here, with what it was called with, and returns what this returns. The word
 * is no answer: the program is parked behind its Chit (`file_id` the chit's
 * word), the op waits for the next cycle's end, and a service point then asks
 * it again through the deck's own handler — the same params, the program's
 * crates — until it answers something else, which is the program's answer. A
 * caller in the kernel with no program to park waits for the cycles in its
 * own call, and is answered the op's own answer. ERR_WOULD_BLOCK with the
 * program parked, ERR_NO_MEMORY with nothing parked when no wait could be
 * made, or the kernel caller's answer.
 */
int SettleOpForRoom(const ManifestOp *op, Crate *crates, uint16_t crate_count,
                    const OpContext *ctx, uint64_t file_id);

#ifdef CONFIG_VOLUME_TESTS
/* The stand's caller of an op that waits for room: a caller in the kernel has
 * no program to park, so a test that wants the op parked as a program's is
 * brings the place its answer is told to — taken by the next op parked, once.
 * The test turns the machine itself while it waits (the cycles, the service
 * point). */
typedef struct SettleOpStandAnswer {
    volatile uint32_t answered;      /* 1 once the op answered; read rc after it */
    error_t           rc;
} SettleOpStandAnswer;

void SettleOpStandAnswerTo(SettleOpStandAnswer *a);
#endif

#endif /* STORAGE_SETTLE_OPS_H */
