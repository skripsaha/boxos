#ifndef BOX_CORE_MANIFEST_H
#define BOX_CORE_MANIFEST_H

#include "box/types.h"
#include "box/core/notify.h"
#include "box/core/result.h"
#include "box/core/crate.h"
#include "boxos_manifest.h"

/*
 * box/manifest.h — userspace builder + submitter for Manifests.
 *
 * Three steps to issue a Manifest-mode syscall:
 *
 *   1. Allocate a buffer in cabin heap large enough to hold the Manifest.
 *      ManifestBuilderInit() points the builder at it.
 *
 *   2. ManifestBuilderAddOp() once per operation. params can be NULL when an
 *      op has no inline parameters.
 *
 *   3. ManifestBuilderFinalize() writes the header. After this the buffer is
 *      a valid Manifest; pass it to ManifestSubmit() with your Crate array.
 *
 * Example — fill a 64-byte buffer with 0xAA and copy into a second:
 *
 *   uint8_t mbuf[64];   // Manifest scratch
 *   uint8_t out_a[64];  // first crate's payload
 *   uint8_t out_b[64];  // second crate's payload
 *
 *   ManifestBuilder mb;
 *   ManifestBuilderInit(&mb, mbuf, sizeof(mbuf));
 *   uint8_t fill_byte = 0xAA;
 *   ManifestBuilderAddOp(&mb, DECK_OPERATIONS, 0x02, 0,
 *                        CRATE_INDEX_NONE, 0, &fill_byte, 1);
 *   ManifestBuilderAddOp(&mb, DECK_OPERATIONS, 0x01, 0, 0, 1, NULL, 0);
 *   ManifestBuilderFinalize(&mb);
 *
 *   Crate crates[2];
 *   CrateSetOutput(&crates[0], out_a, sizeof(out_a));
 *   CrateSetOutput(&crates[1], out_b, sizeof(out_b));
 *
 *   Result r;
 *   ManifestSubmit((Manifest *)mbuf, crates, 2, &r);
 */

typedef struct {
    uint8_t  *buf;
    uint32_t  capacity;
    uint32_t  size;       /* current bytes written into buf */
    uint16_t  op_count;
    uint16_t  finalized;  /* boolean — set by Finalize */
} ManifestBuilder;

/* Initialize the builder over a caller-owned buffer. The buffer must remain
 * valid for the lifetime of the Manifest, including the kernel-side processing
 * window. */
int ManifestBuilderInit(ManifestBuilder *mb, void *buf, uint32_t capacity);

/* Append one operation. Returns 0 on success, negative on overflow.
 *   deck       : OP_KIND deck half (DECK_OPERATIONS, DECK_STORAGE, ...)
 *   opcode     : OP_KIND opcode half
 *   flags      : OP_FLAG_* (0 for default)
 *   in_crate   : index into Crate[] of input data, or CRATE_INDEX_NONE
 *   out_crate  : index into Crate[] of output buffer, or CRATE_INDEX_NONE
 *   params     : inline parameter blob (may be NULL if param_size == 0)
 *   param_size : bytes of params
 */
int ManifestBuilderAddOp(ManifestBuilder *mb,
                         uint16_t         deck,
                         uint16_t         opcode,
                         uint16_t         flags,
                         uint16_t         in_crate,
                         uint16_t         out_crate,
                         const void      *params,
                         uint16_t         param_size);

/* Write the Manifest header into the front of the buffer. After this call,
 * (Manifest *)mb->buf is a valid Manifest. Returns 0 on success. */
int ManifestBuilderFinalize(ManifestBuilder *mb);

/*
 * Submit a Manifest to the kernel by encoding a manifest-mode Pocket and
 * pushing it onto the PocketRing. Blocks for the result.
 *
 * out_result may be NULL if the caller doesn't care about the kernel Result
 * record (success/failure error_code is also returned by this function).
 *
 * Returns:
 *    OK on successful execution
 *    ERR_TIMEOUT  if no result arrives within timeout_ms (default 1000)
 *    other error_t propagated from kernel
 */
int ManifestSubmit(const Manifest *m,
                   Crate          *crates,
                   uint16_t        crate_count,
                   Result         *out_result);

int ManifestSubmitTimeout(const Manifest *m,
                          Crate          *crates,
                          uint16_t        crate_count,
                          Result         *out_result,
                          uint32_t        timeout_ms);

/* Full-control submit. Sets pocket->target_pid for IPC routing (system.route /
 * system.broadcast read it via OpContext). target_pid = 0 means "self". */
int ManifestSubmitFull(const Manifest *m,
                       Crate          *crates,
                       uint16_t        crate_count,
                       uint32_t        target_pid,
                       Result         *out_result,
                       uint32_t        timeout_ms);

/*
 * Push a Manifest-mode Pocket to the kernel WITHOUT blocking for a reply.
 *
 * Use this when the caller has its own context-filtered reply loop
 * (e.g. touch_await waits for KCTX_TOUCH-stamped Results, not the
 * manifest's own ack). Returns OK on successful push, ERR_POCKET_RING_FULL
 * if the producer ring has no capacity, or ERR_INVALID_ARGS on a bad
 * Manifest header.
 */
int ManifestSubmitNoWait(const Manifest *m,
                         const Crate    *crates,
                         uint16_t        crate_count,
                         uint32_t        target_pid);

/*
 * MfCall1 — single-op Manifest convenience wrapper.
 *
 * Builds a 1-op Manifest with optional input crate (in_buf/in_size) and
 * optional output crate (out_buf/out_capacity), submits it, waits for the
 * Result, and returns the kernel error code (OK == 0). On output, when
 * out_actual is provided and an out_crate exists, *out_actual is set to the
 * number of bytes the kernel wrote into out_buf.
 *
 * timeout_ms == 0 uses the default (ManifestSubmit). out_result is optional.
 *
 * Returns:  OK (0) on success; ERR_TIMEOUT or any kernel error_t otherwise.
 *           Negative values for builder/submit failures.
 */
int MfCall1(uint16_t      deck,
            uint16_t      opcode,
            const void   *params,    uint16_t param_size,
            const void   *in_buf,    uint32_t in_size,
            void         *out_buf,   uint32_t out_capacity,
            uint32_t     *out_actual,
            uint32_t      timeout_ms,
            Result       *out_result);

/* =========================================================================
 *  Compile-and-reuse API ("prepared statement" pattern)
 *
 *  Workflow for hot syscall loops:
 *
 *      uint8_t mbuf[64];
 *      ManifestBuilder mb;
 *      ManifestBuilderInit(&mb, mbuf, sizeof(mbuf));
 *      ManifestBuilderAddOp(&mb, DECK_..., OP_..., 0, 0, 1, NULL, 0);
 *      ManifestBuilderFinalize(&mb);
 *
 *      uint64_t handle;
 *      if (ManifestCompileHandle((Manifest *)mbuf, &handle) != 0) error;
 *
 *      for (...) {
 *          Crate crates[2] = { ... };
 *          Result r;
 *          ManifestSubmitHandle(handle, crates, 2, &r);
 *      }
 *
 *      ManifestReleaseHandle(handle);
 *
 *  Each ManifestSubmitHandle skips the kernel's per-syscall validate +
 *  per-op OpRegistryLookup pass (~500-700 ns per call on the benchmarked
 *  workloads). Handles outstanding at process exit are auto-released by
 *  the kernel — explicit release is courteous but not required.
 *
 *  Ownership: only the cabin that compiled a handle may submit-with or
 *  release it. Other cabins receive ERR_ACCESS_DENIED.
 * ========================================================================= */

typedef uint64_t ManifestHandle;

/* Compile a Manifest, return an opaque handle on success. */
int ManifestCompileHandle(const Manifest *m, ManifestHandle *out_handle);

/* Submit a handle-mode Pocket. Same call shape as ManifestSubmit but no
 * Manifest bytes are copied into the kernel — the cached CompiledManifest
 * is executed directly with the supplied Crate[]. */
int ManifestSubmitHandle(ManifestHandle  handle,
                         Crate          *crates,
                         uint16_t        crate_count,
                         Result         *out_result);

int ManifestSubmitHandleTimeout(ManifestHandle  handle,
                                Crate          *crates,
                                uint16_t        crate_count,
                                Result         *out_result,
                                uint32_t        timeout_ms);

/* Release a compiled handle. Idempotent against stale (generation-bumped)
 * handles — returns OK. Caller MUST stop using the handle after release. */
int ManifestReleaseHandle(ManifestHandle handle);

#endif /* BOX_MANIFEST_H */
