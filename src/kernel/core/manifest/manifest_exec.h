#ifndef MANIFEST_EXEC_H
#define MANIFEST_EXEC_H

#include "ktypes.h"
#include "error.h"
#include "manifest.h"
#include "boxos_crate.h"
#include "op_registry.h"


typedef struct ManifestExecResult {
    uint32_t error_code;
    uint16_t total_ops;
    uint16_t completed_ops;
    uint16_t failed_op_idx;
    uint16_t flags;
} ManifestExecResult;

#define EXEC_RESULT_PARTIAL     (1u << 0)
#define EXEC_RESULT_ALL_SKIPPED (1u << 1)

error_t ManifestExecute(ManifestHandle           handle,
                        Crate                   *crates,
                        uint16_t                 crate_count,
                        const OpContext         *ctx,
                        ManifestExecResult      *out_result);

error_t ManifestExecuteOnce(const void              *manifest_kp,
                            uint32_t                 manifest_size,
                            Crate                   *crates,
                            uint16_t                 crate_count,
                            const OpContext         *ctx,
                            ManifestExecResult      *out_result);

#endif