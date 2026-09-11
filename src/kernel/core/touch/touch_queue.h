#ifndef TOUCH_QUEUE_H
#define TOUCH_QUEUE_H

#include "ktypes.h"

void TouchQueueInit(void);
void TouchQueueEnqueue(uint16_t tag_id, const void *payload, uint32_t plen,
                       uint64_t after_ticks, uint32_t source_pid, uint16_t flags);
void TouchQueueWakeAfter(uint32_t target_pid, uint64_t after_ticks,
                         uint32_t owes_result, uint32_t park_seq);
void TouchQueueTick(uint64_t now);

#endif