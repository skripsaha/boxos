#ifndef TOUCH_QUEUE_H
#define TOUCH_QUEUE_H

#include "ktypes.h"

#define TOUCH_QUEUE_CAPACITY 256

typedef struct {
    uint16_t tag_id;
    uint16_t flags;
    uint32_t source_pid;
    uint8_t  payload[64];
    uint32_t plen;
    uint64_t fire_tick;
} TouchQueueEntry;

void TouchQueueInit(void);
void TouchQueueEnqueue(uint16_t tag_id, const void *payload, uint32_t plen,
                       uint64_t after_ticks, uint32_t source_pid, uint16_t flags);
/* Schedule a kernel-side wake (PROC_WAITING -> PROC_WORKING + IPI) of
 * `target_pid` after `after_ticks` PIT ticks. Used by SysTouchAwait so a
 * parked caller can timeout when no real touch arrives. */
void TouchQueueWakeAfter(uint32_t target_pid, uint64_t after_ticks);
void TouchQueueTick(uint64_t now);

#endif /* TOUCH_QUEUE_H */
