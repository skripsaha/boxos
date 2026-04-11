#ifndef PID_ALLOCATOR_H
#define PID_ALLOCATOR_H

#include "ktypes.h"
#include "boxos_limits.h"
#include "klib.h"

// Simple sequential PID allocation: PID = index + 1.
// First process gets PID 1, second gets PID 2, etc.
// PID 0 is reserved for PID_INVALID.
#define PID_MAX_COUNT MAX_PROCESSES
#define PID_INVALID 0

void pid_allocator_init(void);
uint32_t pid_alloc(void);
void pid_free(uint32_t pid);
bool pid_validate(uint32_t pid);
uint32_t pid_allocated_count(void);

#endif // PID_ALLOCATOR_H
