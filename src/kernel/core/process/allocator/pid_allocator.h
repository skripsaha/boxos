#ifndef PID_ALLOCATOR_H
#define PID_ALLOCATOR_H

#include "ktypes.h"
#include "boxos_limits.h"
#include "klib.h"

#define PID_MAX_COUNT MAX_PROCESSES
#define PID_INVALID 0

void pid_allocator_init(void);
uint32_t pid_alloc(void);
void pid_free(uint32_t pid);
bool pid_validate(uint32_t pid);
uint32_t pid_generation(uint32_t pid);
uint32_t pid_allocated_count(void);

typedef enum {
    PID_LIFE_NEVER    = 0,
    PID_LIFE_LIVE     = 1,
    PID_LIFE_DEPARTED = 2,
} PidLife;

PidLife pid_life(uint32_t pid, uint32_t generation);

#endif