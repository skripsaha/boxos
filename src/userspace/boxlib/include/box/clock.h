#ifndef BOX_CLOCK_H
#define BOX_CLOCK_H

#include "box/types.h"
#include "box/error.h"

/*
 * box/clock.h — userspace API over the kernel-mapped ClockBoard page.
 *
 * Reads cost a single memory load; no syscall, no manifest, no ring
 * round-trip. The page is mapped read-only at a fixed VA in every
 * Cabin; the kernel writes per-tick on the BSP. If the page is missing
 * or has the wrong magic / version (e.g. a future kernel/boxlib drift)
 * each helper falls back to the manifest path so the caller still gets
 * a correct answer — slower, but never wrong.
 */

#define CLOCKBOARD_VA            0x4000ULL
#define CLOCKBOARD_MAGIC_USER    0x4b4c4342u    /* 'BCLK' */

/* Mirrors the kernel struct exactly — keep in lock-step with
 * src/kernel/core/clockboard/clockboard.h. */
typedef struct PACKED {
    uint32_t magic;
    uint32_t version;
    volatile uint64_t uptime_us;
    volatile uint64_t uptime_ms;
    volatile uint64_t tick_count;
    uint64_t tsc_freq_khz;
    uint64_t boot_unix_secs;
} ClockBoardView;

/* True if the ClockBoard page is mapped and carries a valid header.
 * Helpers below silently fall back to the kernel path otherwise. */
bool clock_available(void);

/* Hot-path uptime reads (zero syscalls). */
uint64_t clock_uptime_us(void);
uint64_t clock_uptime_ms(void);

/* Wallclock as unix-epoch seconds. Derived from boot_unix_secs +
 * uptime_us. Boxtime is the preferred form via clock_boxtime() below;
 * unix is kept for interop where seconds are already enough. */
uint64_t clock_unix_now(void);

/* Calendar form (BoxOS-native time_t). Computed in userspace from
 * boot_unix_secs + uptime_us — the kernel does NOT do calendar
 * arithmetic in the IRQ. */
struct time_t_;
int clock_boxtime(struct time_t_ *out);

/* Convert a TSC delta to nanoseconds using the calibrated frequency
 * cached on the ClockBoard. ns = (tsc * 1_000_000) / freq_khz. */
uint64_t clock_tsc_to_ns(uint64_t tsc_ticks);

/* Throttle helper: returns true at most once every interval_us, using
 * the ClockBoard as the time source. Caller maintains last_us. */
bool clock_throttle(uint64_t *last_us, uint64_t interval_us);

#endif /* BOX_CLOCK_H */
