#ifndef BOX_TIME_H
#define BOX_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"

typedef struct PACKED {
    uint64_t seconds;
    uint32_t nanosec;
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
    uint8_t  weekday;
} BoxTime;

STATIC_ASSERT(sizeof(BoxTime) == 20, "BoxTime must be 20 bytes");

int time_get(BoxTime* out);
int time_get_secs(uint64_t* out_seconds);
int time_uptime_ms(uint64_t* out_ms);
int time_uptime_ns(uint64_t* out_ns);
int time_format(const BoxTime* t, char* buf, size_t buf_size);
int64_t time_diff(const BoxTime* a, const BoxTime* b);
void time_add_ms(const BoxTime* t, int64_t ms, BoxTime* out);

#ifdef __cplusplus
}
#endif

#endif