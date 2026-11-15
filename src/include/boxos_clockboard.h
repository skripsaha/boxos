#ifndef BOXOS_CLOCKBOARD_H
#define BOXOS_CLOCKBOARD_H

/*
 * boxos_clockboard.h — the ClockBoard page, shared kernel/userspace: the one
 * page the kernel publishes at CABIN_CLOCKBOARD_ADDR (core/clockboard) and
 * boxlib reads with a plain load (box/clock.h). One spelling of its layout:
 * the kernel writes these fields and boxlib reads them, and neither keeps a
 * copy of its own. A reader checks `magic` first; without it (no page mapped)
 * it falls back to the manifest path.
 *
 * Kernel:    #include "ktypes.h" first.
 * Userspace: #include "box/types.h" first.
 */

#include "cabin_layout.h"   /* CABIN_CLOCKBOARD_SIZE — the page it fills */

#define CLOCKBOARD_MAGIC    0x4b4c4342u    /* 'BCLK' = Box Clock */

typedef struct __attribute__((packed)) ClockBoard {
    /* Dynamic — written per PIT tick by BSP. Each a naturally-aligned word. */
    volatile uint64_t uptime_us;     /* monotonic; advanced by 1_000_000/freq µs per tick */
    volatile uint64_t uptime_ms;     /* uptime_us / 1000, refreshed per tick */
    volatile uint64_t tick_count;    /* raw PIT tick counter */

    /* Constant after boot. */
    uint64_t tsc_freq_khz;     /* TSC frequency in kHz, calibrated once */
    uint64_t boot_unix_secs;   /* RTC unix-epoch seconds at boot */
    uint32_t magic;            /* CLOCKBOARD_MAGIC, set once during boot */

    /* Pad to one full page so the struct sizeof matches the mapping. */
    uint8_t  _reserved[CABIN_CLOCKBOARD_SIZE - 8 - 8 - 8 - 8 - 8 - 4];
} ClockBoard;

#ifdef __cplusplus
static_assert(sizeof(ClockBoard) == CABIN_CLOCKBOARD_SIZE, "ClockBoard must be exactly its page");
#else
_Static_assert(sizeof(ClockBoard) == CABIN_CLOCKBOARD_SIZE, "ClockBoard must be exactly its page");
#endif

#endif /* BOXOS_CLOCKBOARD_H */
