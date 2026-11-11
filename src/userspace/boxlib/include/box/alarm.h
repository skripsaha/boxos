#ifndef BOX_ALARM_H
#define BOX_ALARM_H

#include "box/types.h"
#include "boxos_alarm.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Alarm — time, as an event.
 *
 * box_turn_in has no deadline, and box/turnin.h says why: a wait for
 * "something to happen" has no honest one. What a strand cannot sleep on it
 * can be told: set an alarm for a moment, and when the moment comes the kernel
 * puts an ordinary Touch in this strand's own TouchRing — the very arrival
 * Turn In already watches. The sleep stays as it is, and the clock is one more
 * thing that arrives.
 *
 *     uint64_t due = clock_uptime_ms() + 40;
 *     alarm_set(due);                       // before the look, as any bell
 *     TurnInMark mark = box_mark();
 *     if (look_at_everything()) continue;   // alarm_heard is part of the look
 *     box_turn_in(mark);
 *
 * A moment is uptime in milliseconds, the ClockBoard's own (clock_uptime_ms);
 * the kernel's tick is its resolution, and a moment already past comes at the
 * next tick. One alarm per strand: setting again replaces the one pending, so
 * what a strand wants "next" is simply set again, and an animation, a clock
 * and a timeout each ask for the nearest of what they wait for. A replaced or
 * cancelled alarm does not come once the call has run; an event already in the
 * ring stays there, and says which moment it was for. An alarm ends with the
 * strand that set it, and costs nothing while none is set.
 *
 * It is the strand's own: another strand of the same program has its own ring
 * and its own alarm.
 */

/* Set this strand's alarm for `moment_ms`, in place of any pending; 0 is no
 * moment and takes it back (alarm_cancel). OK, or -ERR_* when the call could
 * not be made. The kernel answers nothing: the arrival is the answer. */
int alarm_set(uint64_t moment_ms);

/* The alarm taken back: nothing comes for it any more. */
int alarm_cancel(void);

/* Take the alarm's event if one has come, without waiting; *moment_ms is the
 * moment it was set for — compare it with the one set now, as an event
 * delivered before a replacement is for the moment before. Anything else in
 * the ring is left for its own consumer. */
bool alarm_heard(uint64_t *moment_ms);

#ifdef __cplusplus
}
#endif

#endif /* BOX_ALARM_H */
