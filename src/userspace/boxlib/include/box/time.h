#ifndef BOX_TIME_H
#define BOX_TIME_H

#include "box/types.h"
#include "box/error.h"

typedef struct BOX_PACKED {
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

BOX_STATIC_ASSERT(sizeof(time_t) == 20, "time_t must be 20 bytes");

int time_get(time_t* out);
int time_get_secs(uint64_t* out_seconds);
int time_uptime_ms(uint64_t* out_ms);
int time_uptime_ns(uint64_t* out_ns);
int time_format(const time_t* t, char* buf, size_t buf_size);
int64_t time_diff(const time_t* a, const time_t* b);
void time_add_ms(const time_t* t, int64_t ms, time_t* out);

#endif // BOX_TIME_H
