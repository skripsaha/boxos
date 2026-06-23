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
/* Schedule a kernel-side wake of `target_pid` after `after_ticks` PIT ticks.
 * On expiry the PIT tick reschedules the target (PROC_WAITING -> PROC_WORKING
 * + IPI) and posts an irq_defer that delivers an ERR_TIMEOUT Result from a
 * K-Core IFF the target is an addr_park waiter whose entry->seq still equals
 * `wait_seq` (snapshotted at arm time). SysTouchAwait — which does not register
 * an addr_wait entry — passes wait_seq=0; only the reschedule applies to it. */
void TouchQueueWakeAfter(uint32_t target_pid, uint64_t after_ticks,
                         uint32_t wait_seq);
void TouchQueueTick(uint64_t now);

#endif /* TOUCH_QUEUE_H */
