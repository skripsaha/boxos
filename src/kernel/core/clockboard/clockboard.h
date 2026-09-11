#ifndef CLOCKBOARD_H
#define CLOCKBOARD_H

#include "ktypes.h"
#include "cabin_layout.h"


#define CLOCKBOARD_MAGIC    0x4b4c4342u
#define CLOCKBOARD_VERSION  1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;

    volatile uint64_t uptime_us;
    volatile uint64_t uptime_ms;
    volatile uint64_t tick_count;

    uint64_t tsc_freq_khz;
    uint64_t boot_unix_secs;

    uint8_t  _reserved[4096 - 4 - 4 - 8 - 8 - 8 - 8 - 8];
} ClockBoard;

_Static_assert(sizeof(ClockBoard) == 4096, "ClockBoard must be exactly one page");

void clockboard_init(void);

uint64_t clockboard_phys(void);

void clockboard_tick_update(uint64_t uptime_us, uint64_t tick_count);

uint64_t clockboard_uptime_ms(void);

void clockboard_set_tsc_freq_khz(uint64_t khz);
void clockboard_set_boot_unix_secs(uint64_t secs);

#endif