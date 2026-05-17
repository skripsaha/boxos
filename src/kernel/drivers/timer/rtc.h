#ifndef RTC_H
#define RTC_H

#include "klib.h"

typedef struct __attribute__((packed)) {
    uint64_t seconds;
    uint32_t nanosec;
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  weekday;
} time_t;

_Static_assert(sizeof(time_t) == 20, "time_t must be 20 bytes");

void     rtc_init(void);
void     rtc_get_boxtime(time_t* out);
uint64_t rtc_get_unix64(void);
uint64_t rtc_get_uptime_ns(void);

/* Toggle the NMI mask state baked into every subsequent CMOS register
 * selection. Default after rtc_init is "NMI enabled" (mask byte = 0).
 * Pairs must be balanced — disable() before a critical sequence,
 * enable() immediately after — to avoid permanently silencing the
 * system NMI source. */
void cmos_nmi_disable(void);
void cmos_nmi_enable(void);

#endif // RTC_H
