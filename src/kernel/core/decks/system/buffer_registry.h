#ifndef BUFFER_REGISTRY_H
#define BUFFER_REGISTRY_H

#include "ktypes.h"
#include "error.h"

typedef struct process_t process_t;

typedef struct {
    error_t  err;
    uint64_t handle;
    uint64_t phys_addr;
    uint64_t virt_addr;
    uint64_t actual_size;
} BufferAllocResult;

/* Allocate a buffer for `proc`. Returns handle/phys/virt/actual on success.
 * On failure, .err is set and the rest is undefined. */
BufferAllocResult BufferRegistryAlloc(process_t *proc, uint64_t requested_size);

/* Free a buffer by handle. owner_pid must match the recorded owner. */
error_t BufferRegistryFree(uint32_t owner_pid, uint64_t handle);

/* Resize a buffer by handle. owner_pid must match. On success, *out_actual is
 * the new actual byte count and *out_virt is the (possibly relocated) virt.
 * out_actual / out_virt may be NULL if not needed. */
error_t BufferRegistryResize(process_t *proc, uint64_t handle,
                             uint64_t new_size,
                             uint64_t *out_actual,
                             uint64_t *out_virt);

/* Release every buffer owned by the given pid (called from proc kill / exit). */
void BufferRegistryCleanupProcess(uint32_t pid);

#endif /* BUFFER_REGISTRY_H */
