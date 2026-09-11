#ifndef MANIFEST_H
#define MANIFEST_H

#include "ktypes.h"
#include "klib.h"
#include "error.h"
#include "boxos_manifest.h"
#include "boxos_crate.h"
#include "op_registry.h"


struct process_t;

typedef uint64_t ManifestHandle;

#define MANIFEST_HANDLE_INVALID  0ull
#define MANIFEST_HANDLE_SLOT(h)  ((uint32_t)((h) & 0xFFFFFFFFu))
#define MANIFEST_HANDLE_GEN(h)   ((uint32_t)((h) >> 32))
#define MANIFEST_MAKE_HANDLE(slot, gen) (((uint64_t)(gen) << 32) | (uint64_t)(slot))

#define COMPILED_MANIFEST_MAGIC 0x434D4E46u

#define MANIFEST_RAW_MAX_SIZE (1u << 20)

typedef struct CompiledManifest {
    uint32_t                magic;
    uint32_t                handle_slot;
    uint32_t                generation;
    uint32_t                op_count;
    uint32_t                flags;
    uint32_t                ref_count;
    uint32_t                owner_pid;
    uint32_t                _pad;

    uint8_t                *raw_bytes;
    uint32_t                raw_size;
    uint32_t                _pad2;

    const OpRegistration  **handlers;
    uint32_t               *op_offsets;

    spinlock_t              lock;
} CompiledManifest;


error_t ManifestSubsystemInit(void);
void    ManifestSubsystemShutdown(void);

error_t ManifestCompile(struct process_t *owner,
                        const void       *user_or_kernel,
                        uint32_t          size,
                        bool              is_kernel_ptr,
                        ManifestHandle   *out_handle);

CompiledManifest *ManifestResolve(ManifestHandle handle);

error_t ManifestRetain(ManifestHandle handle);

error_t ManifestRelease(ManifestHandle handle);

uint32_t ManifestActiveCount(void);
void     ManifestDump(ManifestHandle handle);

void ManifestReleaseAllForOwner(uint32_t owner_pid);

#endif