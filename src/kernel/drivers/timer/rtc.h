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

#endif // RTC_H
