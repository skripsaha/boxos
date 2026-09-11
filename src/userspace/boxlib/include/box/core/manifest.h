#ifndef BOX_CORE_MANIFEST_H
#define BOX_CORE_MANIFEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/core/crate.h"
#include "boxos_manifest.h"


typedef struct {
    uint8_t  *buf;
    uint32_t  capacity;
    uint32_t  size;
    uint16_t  op_count;
    uint16_t  finalized;
} ManifestBuilder;

int ManifestBuilderInit(ManifestBuilder *mb, void *buf, uint32_t capacity);

int ManifestBuilderAddOp(ManifestBuilder *mb,
                         uint16_t         deck,
                         uint16_t         opcode,
                         uint16_t         flags,
                         uint16_t         in_crate,
                         uint16_t         out_crate,
                         const void      *params,
                         uint16_t         param_size);

int ManifestBuilderFinalize(ManifestBuilder *mb);

int ManifestSubmit(const Manifest *m,
                   Crate          *crates,
                   uint16_t        crate_count,
                   Result         *out_result);

int ManifestSubmitTimeout(const Manifest *m,
                          Crate          *crates,
                          uint16_t        crate_count,
                          Result         *out_result,
                          uint32_t        timeout_ms);

int ManifestSubmitFull(const Manifest *m,
                       Crate          *crates,
                       uint16_t        crate_count,
                       uint32_t        target_pid,
                       Result         *out_result,
                       uint32_t        timeout_ms);

int ManifestSubmitNoWait(const Manifest *m,
                         const Crate    *crates,
                         uint16_t        crate_count,
                         uint32_t        target_pid);

int MfCall1(uint16_t      deck,
            uint16_t      opcode,
            const void   *params,    uint16_t param_size,
            const void   *in_buf,    uint32_t in_size,
            void         *out_buf,   uint32_t out_capacity,
            uint32_t     *out_actual,
            uint32_t      timeout_ms,
            Result       *out_result);


typedef uint64_t ManifestHandle;

int ManifestCompileHandle(const Manifest *m, ManifestHandle *out_handle);

int ManifestSubmitHandle(ManifestHandle  handle,
                         Crate          *crates,
                         uint16_t        crate_count,
                         Result         *out_result);

int ManifestSubmitHandleTimeout(ManifestHandle  handle,
                                Crate          *crates,
                                uint16_t        crate_count,
                                Result         *out_result,
                                uint32_t        timeout_ms);

int ManifestReleaseHandle(ManifestHandle handle);

#ifdef __cplusplus
}
#endif

#endif