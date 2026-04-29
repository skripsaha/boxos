#ifndef MANIFEST_EXEC_H
#define MANIFEST_EXEC_H

#include "ktypes.h"
#include "error.h"
#include "manifest.h"
#include "boxos_crate.h"
#include "op_registry.h"

/*
 * ManifestExecute — sequential dispatch of a compiled Manifest.
 *
 * For each op in cm->op_count:
 *   - Check OP_FLAG_SKIP_ON_ERROR against the last op's outcome.
 *   - Resolve in_crate / out_crate indices into the caller-provided Crate[].
 *   - Invoke the resolved handler.
 *   - Track per-op error and update result counters.
 *
 * Execution stops on the first error unless OP_FLAG_OPTIONAL is set on the
 * failing op. TRANSACTIONAL flag is recognized but rollback semantics are
 * deferred until DiskBook integration in Phase 8.
 *
 * The Crate array is passed by reference: handlers may mutate the size field
 * to indicate produced bytes (e.g., a read returns N bytes < capacity).
 */

typedef struct ManifestExecResult {
    uint32_t error_code;     /* last non-OK error, or OK */
    uint16_t total_ops;
    uint16_t completed_ops;  /* number of ops that ran to completion (success or skipped) */
    uint16_t failed_op_idx;  /* index of first failed op; valid only if error_code != OK */
    uint16_t flags;          /* EXEC_RESULT_* */
} ManifestExecResult;

#define EXEC_RESULT_PARTIAL     (1u << 0)  /* error happened mid-way */
#define EXEC_RESULT_ALL_SKIPPED (1u << 1)  /* every op was skipped due to prior error */

/*
 * Synchronously execute every op of the compiled Manifest.
 *
 *   handle     : compiled handle from ManifestCompile
 *   crates     : caller-owned array referenced by op.in_crate / op.out_crate
 *   crate_count: length of crates[]
 *   ctx        : per-execution context (initiator process, target_pid, ...)
 *   out_result : aggregate execution result (may be NULL if caller doesn't care)
 *
 * Returns OK if every op returned OK. Returns the first non-OK error code
 * otherwise; out_result still carries detailed counters in that case.
 */
error_t ManifestExecute(ManifestHandle           handle,
                        Crate                   *crates,
                        uint16_t                 crate_count,
                        const OpContext         *ctx,
                        ManifestExecResult      *out_result);

/*
 * One-shot execution of a raw Manifest at kernel-translated address.
 *
 * Skips the compile/handle/refcount machinery — ideal for the syscall fast
 * path where every Pocket carries its own Manifest in cabin heap. The caller
 * has already translated the user vaddr through vmm_translate_user_addr and
 * provides a kernel pointer.
 *
 * Validates structure on every call. For repeated execution of the same
 * Manifest, prefer ManifestCompile + ManifestExecute (cached handlers).
 */
error_t ManifestExecuteOnce(const void              *manifest_kp,
                            uint32_t                 manifest_size,
                            Crate                   *crates,
                            uint16_t                 crate_count,
                            const OpContext         *ctx,
                            ManifestExecResult      *out_result);

#endif /* MANIFEST_EXEC_H */
