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
uint32_t pid_generation(uint32_t pid);  /* generation[pid-1] under allocator-lock; 0 if invalid */
uint32_t pid_allocated_count(void);

/* What became of one named incarnation.
 *
 * A slot's generation only ever counts UP — pid_alloc bumps it, pid_free
 * leaves it — so the counter alone decides the question exactly, with no
 * history buffer and no forgetting: an incarnation older than the slot's
 * current count has provably finished, one newer than it was never issued.
 * That is what lets "wait until this process is gone" be a question about a
 * STATE rather than a subscription to an EDGE — an edge can be missed, and a
 * missed death used to mean waiting forever. */
typedef enum {
    PID_LIFE_NEVER    = 0,  /* generation ahead of the slot — never issued */
    PID_LIFE_LIVE     = 1,  /* this exact incarnation holds the slot right now */
    PID_LIFE_DEPARTED = 2,  /* this incarnation ran and is finished */
} PidLife;

/* Bitmap AND generation read under ONE acquisition of the allocator lock:
 * sampled separately they can straddle a free+realloc and answer about two
 * different lives. */
PidLife pid_life(uint32_t pid, uint32_t generation);

#endif // PID_ALLOCATOR_H
