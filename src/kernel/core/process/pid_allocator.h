#ifndef PID_ALLOCATOR_H
#define PID_ALLOCATOR_H

#include "ktypes.h"
#include "boxos_limits.h"
#include "klib.h"

// PID Structure: [24-bit generation | 8-bit index]
// Example: PID 0x00000142 = generation 1, index 66
#define PID_MAX_COUNT        BOXOS_MAX_PROCESSES  // 256
#define PID_INVALID          0

#define PID_INDEX_BITS       8
#define PID_GEN_BITS         24
#define PID_INDEX_MASK       0xFF
#define PID_GEN_SHIFT        8

#define PID_BITMAP_SIZE      (PID_MAX_COUNT / 8)  // 32 bytes

// Extract index from composite PID
#define PID_TO_INDEX(pid)    ((pid) & PID_INDEX_MASK)
// Extract generation from composite PID
#define PID_TO_GEN(pid)      (((pid) >> PID_GEN_SHIFT) & 0xFFFFFF)
// Build composite PID from generation and index
#define PID_BUILD(gen, idx)  ((((gen) & 0xFFFFFF) << PID_GEN_SHIFT) | ((idx) & 0xFF))

// API Functions
void pid_allocator_init(void);
uint32_t pid_alloc(void);
void pid_free(uint32_t pid);
bool pid_validate(uint32_t pid);
uint32_t pid_allocated_count(void);

#endif // PID_ALLOCATOR_H
