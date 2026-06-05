#ifndef MANIFEST_H
#define MANIFEST_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "op_registry.h"

/*
 * CompiledManifest — kernel-owned, validated, dispatch-ready chain.
 *
 * Lifecycle:
 *   1. User builds raw Manifest in cabin heap.
 *   2. User submits a "compile" request → kernel copies bytes, walks ops,
 *      validates each op_kind is registered, security_gate passes, fills in
 *      cached handler pointers.
 *   3. Kernel returns ManifestHandle (uint64). Handle is opaque to userspace.
 *   4. User submits Pockets that reference the handle. Each execution skips
 *      validation; pure dispatch.
 *   5. ManifestRelease decrements refcount; on 0 the compiled form is freed.
 *
 * ManifestHandle layout:
 *   bits  0..31 : slot index in the global ManifestTable
 *   bits 32..63 : generation counter (incremented per slot reuse)
 *
 * The generation prevents use-after-free: stale handles see gen mismatch.
 */

struct process_t;

typedef uint64_t ManifestHandle;

#define MANIFEST_HANDLE_INVALID  0ull
#define MANIFEST_HANDLE_SLOT(h)  ((uint32_t)((h) & 0xFFFFFFFFu))
#define MANIFEST_HANDLE_GEN(h)   ((uint32_t)((h) >> 32))
#define MANIFEST_MAKE_HANDLE(slot, gen) (((uint64_t)(gen) << 32) | (uint64_t)(slot))

#define COMPILED_MANIFEST_MAGIC 0x434D4E46u  /* 'CMNF' */

/*
 * Shared upper bound on raw Manifest payloads accepted by ANY entry point —
 * ManifestCompile and the guide.c dispatch staging path (ManifestStage) both
 * reject inputs larger than this. 1 MiB is far beyond any realistic op
 * stream: the 16-bit op_count field caps the wire format at 65535 ops, and
 * even at the maximum 12-byte op header (param_size=0) that's 768 KiB
 * without parameters. Larger payloads should use Brook (streaming) or be
 * split into multiple smaller submissions instead of growing this cap.
 */
#define MANIFEST_RAW_MAX_SIZE (1u << 20)

typedef struct CompiledManifest {
    uint32_t                magic;
    uint32_t                handle_slot;     /* slot index this object lives in */
    uint32_t                generation;      /* must equal slot generation to be valid */
    uint32_t                op_count;
    uint32_t                flags;           /* from Manifest.flags */
    uint32_t                ref_count;       /* atomic */
    uint32_t                owner_pid;       /* the cabin that compiled it */
    uint32_t                _pad;

    /* Kernel-owned copy of the raw Manifest bytes. ops[] inside this buffer
     * are referenced by parallel arrays below. */
    uint8_t                *raw_bytes;
    uint32_t                raw_size;
    uint32_t                _pad2;

    /* Parallel arrays, length = op_count */
    const OpRegistration  **handlers;        /* resolved at compile time */
    uint32_t               *op_offsets;      /* byte offset of each ManifestOp inside raw_bytes */

    spinlock_t              lock;
} CompiledManifest;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

error_t ManifestSubsystemInit(void);
void    ManifestSubsystemShutdown(void);

/*
 * Compile a raw Manifest into a dispatch-ready CompiledManifest.
 *
 *   owner          : process that "owns" the compiled form (refcount root)
 *   user_or_kernel : pointer to raw Manifest bytes
 *   size           : bytes available at user_or_kernel
 *   is_kernel_ptr  : if true, treat user_or_kernel as kernel address (used by
 *                    kernel self-tests and bootstrap manifests). If false,
 *                    user_or_kernel is a user vaddr that gets translated via
 *                    vmm_translate_user_addr against owner->cabin.
 *   out_handle     : on success, opaque handle for later submit/release.
 *
 * Validation performed:
 *   - magic, version, total_size sanity
 *   - op_count > 0
 *   - each ManifestOp fits within total_size
 *   - each op_kind is registered in OpRegistry
 *   - each op's security_mask is satisfied by owner->tag_bits (if owner != NULL)
 *
 * On failure, no allocation is leaked; out_handle is set to
 * MANIFEST_HANDLE_INVALID.
 */
error_t ManifestCompile(struct process_t *owner,
                        const void       *user_or_kernel,
                        uint32_t          size,
                        bool              is_kernel_ptr,
                        ManifestHandle   *out_handle);

/*
 * Resolve a handle into a CompiledManifest pointer, with refcount pin.
 * Caller MUST balance with ManifestRelease(handle). NULL on stale/invalid.
 */
CompiledManifest *ManifestResolve(ManifestHandle handle);

/* Increment refcount on an already-pinned handle. */
error_t ManifestRetain(ManifestHandle handle);

/* Decrement refcount; on 0 the compiled form is freed and slot generation
 * advances so the handle becomes permanently invalid. */
error_t ManifestRelease(ManifestHandle handle);

/* Diagnostic */
uint32_t ManifestActiveCount(void);
void     ManifestDump(ManifestHandle handle);

/*
 * Release every handle owned by `owner_pid`. Called from process_destroy
 * so a dying cabin doesn't leak compiled Manifests. Race-safe against
 * concurrent ManifestExecute on the same handle from another core — Resolve
 * pins the compiled form for the executor's duration, and our Release just
 * drops the cabin's initial ref. The compiled form is freed when refcount
 * hits 0 (i.e. after the in-flight Execute also Releases).
 *
 * Iterates in 64-handle batches so we hold g_manifest_table.lock briefly;
 * the actual ManifestRelease calls happen unlocked. Generation counter in
 * the handle makes any stale-slot race a harmless ERR_INVALID_ARGUMENT.
 */
void ManifestReleaseAllForOwner(uint32_t owner_pid);

#endif /* MANIFEST_H */
