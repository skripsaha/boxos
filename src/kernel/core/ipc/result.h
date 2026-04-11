#ifndef RESULT_H
#define RESULT_H

#include "ktypes.h"

// Result: a syscall response from kernel to userspace.
// Lives in ResultRing (per-process shared memory).
// Data is NOT inline — data_addr points to cabin heap (same addr userspace passed).

typedef struct __packed {
    uint32_t error_code;     // error_t from deck processing
    uint32_t data_length;    // bytes kernel wrote at data_addr
    uint64_t data_addr;      // for IPC: address of data in receiver's heap
    uint32_t sender_pid;     // for IPC: who sent this result (0 = kernel)
    uint32_t _reserved;
} Result;

_Static_assert(sizeof(Result) == 24, "Result must be 24 bytes for ResultRing packing");

#endif // RESULT_H
