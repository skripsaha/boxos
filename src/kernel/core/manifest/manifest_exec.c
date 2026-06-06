#include "manifest_exec.h"
#include "manifest_auth.h"
#include "klib.h"
#include "boxos_manifest.h"
#include "canvas.h"   /* CanvasBatchBegin/End — coalesce multi-op Manifests into one display commit */

/*
 * Sequential executor. Walks the compiled op stream once, dispatching each
 * handler with bounds-checked Crate references.
 *
 * Error handling:
 *   - First non-OK from a handler is recorded as the manifest's error_code.
 *   - If the failing op has OP_FLAG_OPTIONAL, execution continues; otherwise
 *     subsequent ops are skipped unless they have OP_FLAG_SKIP_ON_ERROR
 *     (which means "I expect to be skipped after a prior failure", i.e.,
 *     this op is itself a recovery step that runs only on success path).
 *
 * Note: PARALLEL_OK / TRANSACTIONAL semantics are deferred. Today every
 * Manifest is executed strictly serially. Once the executor handles
 * dependency analysis (data-flow between in_crate/out_crate) and journaling
 * is integrated, those flags will gain teeth.
 */

static inline bool crate_index_in_range(uint16_t idx, uint16_t crate_count)
{
    return idx == CRATE_INDEX_NONE || idx < crate_count;
}

static error_t dispatch_op(const OpRegistration *reg,
                           const ManifestOp     *op,
                           Crate                *crates,
                           uint16_t              crate_count,
                           const OpContext      *ctx)
{
    if (!crate_index_in_range(op->in_crate, crate_count) ||
        !crate_index_in_range(op->out_crate, crate_count)) {
        return ERR_INVALID_ARGUMENT;
    }

    /* Validate referenced crates if any. NONE means the op needs no crate of
     * that direction; the handler is responsible for asserting its own
     * required crates. */
    if (op->in_crate != CRATE_INDEX_NONE && !CrateIsValid(&crates[op->in_crate])) {
        return ERR_INVALID_BUFFER_ID;
    }
    if (op->out_crate != CRATE_INDEX_NONE && !CrateIsValid(&crates[op->out_crate])) {
        return ERR_INVALID_BUFFER_ID;
    }

    /* Tag-based authorization: deny if the calling process lacks the auth
     * level the op was registered with. Kernel-internal manifests (proc==NULL)
     * always pass. */
    if (!ManifestOpAuthorize(op->op_kind, ctx)) {
        return ERR_ACCESS_DENIED;
    }

    return reg->handler(op, crates, crate_count, ctx);
}

error_t ManifestExecute(ManifestHandle           handle,
                        Crate                   *crates,
                        uint16_t                 crate_count,
                        const OpContext         *ctx,
                        ManifestExecResult      *out_result)
{
    if (!ctx) return ERR_NULL_POINTER;

    CompiledManifest *cm = ManifestResolve(handle);
    if (!cm) return ERR_INVALID_ARGUMENT;

    ManifestExecResult result;
    result.error_code    = OK;
    result.total_ops     = (uint16_t)cm->op_count;
    result.completed_ops = 0;
    result.failed_op_idx = 0;
    result.flags         = 0;

    error_t first_error = OK;
    bool    prev_failed = false;
    bool    any_executed = false;

    /* One Canvas batch wraps the whole op stream so multi-op Manifests
     * coalesce into a single backend Present.  Single-op Manifests get
     * no wrap — the op's own per-call batch already commits at the right
     * boundary, and an extra begin/end here would just add lock cycles. */
    const bool batch_wrap = (cm->op_count > 1);
    if (batch_wrap) CanvasBatchBegin();

    for (uint32_t i = 0; i < cm->op_count; i++) {
        const ManifestOp     *op  = (const ManifestOp *)(cm->raw_bytes + cm->op_offsets[i]);
        const OpRegistration *reg = cm->handlers[i];

        /* Skip ops that explicitly want to be skipped if a prior op failed. */
        if (prev_failed && (op->flags & OP_FLAG_SKIP_ON_ERROR)) {
            continue;
        }

        /* If a prior op failed and this op is not OPTIONAL or SKIP_ON_ERROR,
         * we abort. Optional ops continue regardless; skip-on-error ops
         * already handled above. */
        if (prev_failed && !(op->flags & OP_FLAG_OPTIONAL)) {
            break;
        }

        any_executed = true;
        error_t rc = dispatch_op(reg, op, crates, crate_count, ctx);
        if (rc != OK) {
            if (first_error == OK) {
                first_error           = rc;
                result.error_code     = (uint32_t)rc;
                result.failed_op_idx  = (uint16_t)i;
                result.flags         |= EXEC_RESULT_PARTIAL;
            }
            if (op->flags & OP_FLAG_OPTIONAL) {
                /* Optional failure: don't poison subsequent ops. */
                result.completed_ops++;
                continue;
            }
            prev_failed = true;
            continue;
        }
        result.completed_ops++;
    }

    if (!any_executed) {
        result.flags |= EXEC_RESULT_ALL_SKIPPED;
    }

    if (batch_wrap) CanvasBatchEnd();

    if (out_result) *out_result = result;
    ManifestRelease(handle);
    return first_error;
}

error_t ManifestExecuteOnce(const void              *manifest_kp,
                            uint32_t                 manifest_size,
                            Crate                   *crates,
                            uint16_t                 crate_count,
                            const OpContext         *ctx,
                            ManifestExecResult      *out_result)
{
    if (!ctx)                                return ERR_NULL_POINTER;
    if (!manifest_kp)                        return ERR_NULL_POINTER;
    if (manifest_size < sizeof(Manifest))    return ERR_BUFFER_TOO_SMALL;

    const Manifest *hdr = (const Manifest *)manifest_kp;
    if (hdr->magic != MANIFEST_MAGIC)        return ERR_INVALID_POCKET;
    if (hdr->version != MANIFEST_VERSION)    return ERR_VERSION_MISMATCH;
    if (hdr->total_size != manifest_size)    return ERR_INVALID_ARGUMENT;
    if (hdr->op_count == 0)                  return ERR_INVALID_ARGUMENT;

    ManifestExecResult result;
    result.error_code    = OK;
    result.total_ops     = (uint16_t)hdr->op_count;
    result.completed_ops = 0;
    result.failed_op_idx = 0;
    result.flags         = 0;

    error_t first_error  = OK;
    bool    prev_failed  = false;
    bool    any_executed = false;

    const bool batch_wrap = (hdr->op_count > 1);
    if (batch_wrap) CanvasBatchBegin();

    uint32_t cursor = sizeof(Manifest);
    for (uint32_t i = 0; i < hdr->op_count; i++) {
        if (cursor + sizeof(ManifestOp) > manifest_size) {
            if (first_error == OK) {
                first_error           = ERR_INVALID_ARGUMENT;
                result.error_code     = (uint32_t)first_error;
                result.failed_op_idx  = (uint16_t)i;
                result.flags         |= EXEC_RESULT_PARTIAL;
            }
            break;
        }
        const ManifestOp *op = (const ManifestOp *)((const uint8_t *)manifest_kp + cursor);
        uint32_t op_total = (uint32_t)sizeof(ManifestOp) + (uint32_t)op->param_size;
        if (cursor + op_total > manifest_size) {
            if (first_error == OK) {
                first_error           = ERR_INVALID_ARGUMENT;
                result.error_code     = (uint32_t)first_error;
                result.failed_op_idx  = (uint16_t)i;
                result.flags         |= EXEC_RESULT_PARTIAL;
            }
            break;
        }
        cursor += op_total;

        if (prev_failed && (op->flags & OP_FLAG_SKIP_ON_ERROR)) continue;
        if (prev_failed && !(op->flags & OP_FLAG_OPTIONAL))     break;

        const OpRegistration *reg = OpRegistryLookup(op->op_kind);
        if (!reg) {
            if (first_error == OK) {
                first_error           = ERR_INVALID_OPCODE;
                result.error_code     = (uint32_t)first_error;
                result.failed_op_idx  = (uint16_t)i;
                result.flags         |= EXEC_RESULT_PARTIAL;
            }
            prev_failed = true;
            continue;
        }
        if (op->in_crate  != CRATE_INDEX_NONE && op->in_crate  >= crate_count) {
            if (first_error == OK) {
                first_error           = ERR_INVALID_ARGUMENT;
                result.error_code     = (uint32_t)first_error;
                result.failed_op_idx  = (uint16_t)i;
                result.flags         |= EXEC_RESULT_PARTIAL;
            }
            prev_failed = true;
            continue;
        }
        if (op->out_crate != CRATE_INDEX_NONE && op->out_crate >= crate_count) {
            if (first_error == OK) {
                first_error           = ERR_INVALID_ARGUMENT;
                result.error_code     = (uint32_t)first_error;
                result.failed_op_idx  = (uint16_t)i;
                result.flags         |= EXEC_RESULT_PARTIAL;
            }
            prev_failed = true;
            continue;
        }

        if (!ManifestOpAuthorize(op->op_kind, ctx)) {
            if (first_error == OK) {
                first_error           = ERR_ACCESS_DENIED;
                result.error_code     = (uint32_t)first_error;
                result.failed_op_idx  = (uint16_t)i;
                result.flags         |= EXEC_RESULT_PARTIAL;
            }
            if (op->flags & OP_FLAG_OPTIONAL) {
                result.completed_ops++;
                continue;
            }
            prev_failed = true;
            continue;
        }

        any_executed = true;
        int rc = reg->handler(op, crates, crate_count, ctx);
        if (rc != OK) {
            if (first_error == OK) {
                first_error           = (error_t)rc;
                result.error_code     = (uint32_t)rc;
                result.failed_op_idx  = (uint16_t)i;
                result.flags         |= EXEC_RESULT_PARTIAL;
            }
            if (op->flags & OP_FLAG_OPTIONAL) {
                result.completed_ops++;
                continue;
            }
            prev_failed = true;
            continue;
        }
        result.completed_ops++;
    }

    if (!any_executed) result.flags |= EXEC_RESULT_ALL_SKIPPED;

    if (batch_wrap) CanvasBatchEnd();

    if (out_result) *out_result = result;
    return first_error;
}
