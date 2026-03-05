#ifndef PID_ALLOCATOR_H
#define PID_ALLOCATOR_H

#include "ktypes.h"
#include "boxos_limits.h"
#include "klib.h"

// PID structure: [16-bit generation | 16-bit index]
// Supports up to 65536 slots; active limit is MAX_PROCESSES (4096).
#define PID_MAX_COUNT        MAX_PROCESSES
#define PID_INVALID          0

#define PID_INDEX_BITS       16
#define PID_GEN_BITS         16
#define PID_INDEX_MASK       0xFFFF
#define PID_GEN_SHIFT        16

#define PID_BITMAP_SIZE      (PID_MAX_COUNT / 8)

#define PID_TO_INDEX(pid)    ((pid) & PID_INDEX_MASK)
#define PID_TO_GEN(pid)      (((pid) >> PID_GEN_SHIFT) & 0xFFFF)
#define PID_BUILD(gen, idx)  ((((gen) & 0xFFFF) << PID_GEN_SHIFT) | ((idx) & 0xFFFF))

void pid_allocator_init(void);
uint32_t pid_alloc(void);
void pid_free(uint32_t pid);
bool pid_validate(uint32_t pid);
uint32_t pid_allocated_count(void);

#endif // PID_ALLOCATOR_H
