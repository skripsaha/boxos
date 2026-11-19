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
    /* Dynamic — written each tick by the BSP, every word naturally aligned,
     * between two steps of `seq`: odd while a write is under way. A reader
     * takes what lies between and reads again if it saw `seq` odd or changed,
     * so uptime_us and tsc_at are always of one write. */
    volatile uint64_t seq;
    volatile uint64_t uptime_us;     /* the kernel's clock as the tick read it */
    volatile uint64_t uptime_ms;     /* uptime_us / 1000 */
    volatile uint64_t tick_count;    /* the tick counter */
    volatile uint64_t tsc_at;        /* the TSC read with uptime_us: a reader adds what the
                                      * TSC ran since, and has its own microsecond, not the
                                      * tick's (box/clock.h) */

    uint64_t tsc_freq_khz;     /* TSC frequency in kHz: calibrated at boot, refined by the
                                * kernel's recalibration */
    uint64_t boot_unix_secs;   /* RTC unix-epoch seconds at boot */
    uint32_t magic;            /* CLOCKBOARD_MAGIC, set once during boot */

    /* Pad to one full page so the struct sizeof matches the mapping. */
    uint8_t  _reserved[CABIN_CLOCKBOARD_SIZE - 7 * 8 - 4];
} ClockBoard;

#ifdef __cplusplus
static_assert(sizeof(ClockBoard) == CABIN_CLOCKBOARD_SIZE, "ClockBoard must be exactly its page");
#else
_Static_assert(sizeof(ClockBoard) == CABIN_CLOCKBOARD_SIZE, "ClockBoard must be exactly its page");
#endif

#endif /* BOXOS_CLOCKBOARD_H */
