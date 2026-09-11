#ifndef BOX_CLOCK_H
#define BOX_CLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/types.h"
#include "box/error.h"
#include "box/time.h"


#define CLOCKBOARD_MAGIC_USER    0x4b4c4342u

typedef struct PACKED {
    uint32_t magic;
    uint32_t version;
    volatile uint64_t uptime_us;
    volatile uint64_t uptime_ms;
    volatile uint64_t tick_count;
    uint64_t tsc_freq_khz;
    uint64_t boot_unix_secs;
} ClockBoardView;

bool clock_available(void);

uint64_t clock_uptime_us(void);
uint64_t clock_uptime_ms(void);

uint64_t clock_unix_now(void);

uint64_t clock_unix_now_ns(void);

int clock_boxtime(BoxTime *out);

uint64_t clock_tsc_to_ns(uint64_t tsc_ticks);

bool clock_throttle(uint64_t *last_us, uint64_t interval_us);

#ifdef __cplusplus
}
#endif

#endif