#ifndef TOUCH_QUEUE_H
#define TOUCH_QUEUE_H

#include "ktypes.h"

void TouchQueueInit(void);
void TouchQueueEnqueue(uint16_t tag_id, const void *payload, uint32_t plen,
                       uint64_t after_ticks, uint32_t source_pid, uint16_t flags);
/* Schedule a kernel-side wake of `target_pid` after `after_ticks` PIT ticks.
 *
 *   park_seq     the caller's process_t.park_seq at arm time. On expiry the
 *                PIT tick acts (reschedules the target, passes its deadline
 *                baton) ONLY while the strand is still in that same sleep. A
 *                wait that ended early leaves its wake armed, and without
 *                this the wake would land on whatever park came next — see
 *                process_t.park_seq.
 *   owes_result  1 for an addr_park: on expiry the kernel owes ERR_TIMEOUT as
 *                a Result, delivered by a K-Core on the process's own baton.
 *                SysTouchAwait registers no addr_wait entry and passes 0: it
 *                is owed no Result, only the reschedule. */
void TouchQueueWakeAfter(uint32_t target_pid, uint64_t after_ticks,
                         uint32_t owes_result, uint32_t park_seq);
void TouchQueueTick(uint64_t now);

#endif /* TOUCH_QUEUE_H */
