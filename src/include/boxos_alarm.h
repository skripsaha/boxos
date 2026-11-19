#ifndef BOXOS_ALARM_H
#define BOXOS_ALARM_H

/*
 * The alarm's one shared word — what the kernel says when a moment comes
 * (SYSTEM_OP_ALARM, box/alarm.h). Time arrives as an ordinary Touch on the
 * strand that set it, wearing this tag; its payload is an AlarmEvent.
 *
 * Includer must provide uint64_t BEFORE including this header.
 */

#define ALARM_TOUCH_TAG  "alarm"

typedef struct AlarmEvent {
    uint64_t moment_us;     /* the moment it was set for, uptime in µs as the ClockBoard counts */
} AlarmEvent;

#endif
