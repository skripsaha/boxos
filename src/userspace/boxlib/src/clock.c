#include "box/clock.h"
#include "box/time.h"

/*
 * Userspace ClockBoard reader.
 *
 * The page lives at CABIN_CLOCKBOARD_ADDR in every Cabin (mapped R/O
 * by the kernel during process creation). Plain pointer read — no
 * syscall.
 * If the page is missing or carries the wrong header, every helper
 * falls back to the manifest path (time_uptime_ms etc.) so the answer
 * is still correct, just slow.
 */

static const ClockBoardView *cb(void)
{
    return (const ClockBoardView *)(uintptr_t)CABIN_CLOCKBOARD_ADDR;
}

bool clock_available(void)
{
    const ClockBoardView *v = cb();
    /* Magic + version check — guards against an older boxlib running
     * against a future kernel layout, or a Cabin where the mapping
     * hasn't been set up. */
    return v->magic == CLOCKBOARD_MAGIC_USER && v->version >= 1u;
}

uint64_t clock_uptime_us(void)
{
    const ClockBoardView *v = cb();
    if (v->magic == CLOCKBOARD_MAGIC_USER) {
        return v->uptime_us;
    }
    /* Fallback: ask the kernel via the HW deck. */
    uint64_t ms = 0;
    if (time_uptime_ms(&ms) == 0) return ms * 1000ULL;
    return 0;
}

uint64_t clock_uptime_ms(void)
{
    const ClockBoardView *v = cb();
    if (v->magic == CLOCKBOARD_MAGIC_USER) {
        return v->uptime_ms;
    }
    uint64_t ms = 0;
    if (time_uptime_ms(&ms) == 0) return ms;
    return 0;
}

uint64_t clock_unix_now(void)
{
    const ClockBoardView *v = cb();
    if (v->magic == CLOCKBOARD_MAGIC_USER && v->boot_unix_secs != 0) {
        return v->boot_unix_secs + (v->uptime_us / 1000000ULL);
    }
    /* Fallback: ask the kernel directly. */
    uint64_t secs = 0;
    if (time_get_secs(&secs) == 0) return secs;
    return 0;
}

uint64_t clock_unix_now_ns(void)
{
    const ClockBoardView *v = cb();
    if (v->magic == CLOCKBOARD_MAGIC_USER && v->boot_unix_secs != 0) {
        /* Single board view: boot_unix_secs is immutable post-boot and the
         * kernel writes uptime_us as one 8-byte store, so reading both from
         * the same snapshot keeps seconds and sub-seconds consistent. */
        uint64_t us  = v->uptime_us;
        uint64_t sec = v->boot_unix_secs + us / 1000000ULL;
        uint64_t sub = us % 1000000ULL;            /* leftover microseconds */
        return sec * 1000000000ULL + sub * 1000ULL;
    }
    /* Fallback: kernel seconds (no sub-second precision available). */
    uint64_t secs = 0;
    if (time_get_secs(&secs) == 0) return secs * 1000000000ULL;
    return 0;
}

uint64_t clock_tsc_to_ns(uint64_t tsc_ticks)
{
    const ClockBoardView *v = cb();
    uint64_t khz = (v->magic == CLOCKBOARD_MAGIC_USER) ? v->tsc_freq_khz : 0;
    if (khz == 0) return 0;
    /* (tsc * 1_000_000) / khz — split to avoid 64-bit overflow on the
     * multiply when tsc is large. khz is ~1e6 so (tsc / khz) is the
     * integer-seconds piece, and the remainder gives sub-second ns. */
    uint64_t secs_int = tsc_ticks / khz;
    uint64_t rem      = tsc_ticks % khz;
    return secs_int * 1000000ULL + (rem * 1000000ULL) / khz;
}

bool clock_throttle(uint64_t *last_us, uint64_t interval_us)
{
    if (!last_us) return false;
    uint64_t now = clock_uptime_us();
    if (now - *last_us < interval_us) return false;
    *last_us = now;
    return true;
}

/* -------------------------------------------------------------------------
 * Boxtime calendar conversion. The kernel does NOT do calendar arithmetic
 * in the IRQ (per-tick branch on leap years would dwarf the actual update);
 * we derive year/month/day/hour/min/sec from unix-secs in userspace.
 *
 * Algorithm: Howard Hinnant's "civil_from_days" (public domain), proven
 * leap-year-correct for 1970..9999. seconds-of-day is straightforward.
 * ------------------------------------------------------------------------- */

static void unix_to_civil(uint64_t unix_secs, time_t *out)
{
    uint64_t days     = unix_secs / 86400ULL;
    uint32_t sec_day  = (uint32_t)(unix_secs % 86400ULL);

    /* Day-of-week: 1970-01-01 was a Thursday (=4 in 0=Sunday). */
    out->weekday = (uint8_t)((days + 4ULL) % 7ULL);

    /* Hinnant: shift epoch to 0000-03-01, work in 400-year eras. */
    int64_t  z   = (int64_t)days + 719468LL;
    int64_t  era = (z >= 0 ? z : z - 146096LL) / 146097LL;
    uint32_t doe = (uint32_t)(z - era * 146097LL);
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int64_t  y   = (int64_t)yoe + era * 400LL;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp  = (5u * doy + 2u) / 153u;
    uint32_t d   = doy - (153u * mp + 2u) / 5u + 1u;
    uint32_t m   = mp < 10u ? mp + 3u : mp - 9u;
    if (m <= 2u) y += 1;

    out->year   = (uint16_t)y;
    out->month  = (uint8_t)m;
    out->day    = (uint8_t)d;
    out->hour   = (uint8_t)(sec_day / 3600u);
    out->minute = (uint8_t)((sec_day / 60u) % 60u);
    out->second = (uint8_t)(sec_day % 60u);
}

int clock_boxtime(struct time_t_ *out_)
{
    time_t *out = (time_t *)out_;
    if (!out) return -ERR_NULL_POINTER;

    const ClockBoardView *v = cb();
    if (v->magic != CLOCKBOARD_MAGIC_USER) {
        /* Fallback: full RTC fetch via manifest. */
        return time_get(out);
    }

    uint64_t us   = v->uptime_us;
    uint64_t boot = v->boot_unix_secs;
    if (boot == 0) {
        /* Static field not populated yet — use kernel path for the calendar
         * fields, but keep nanosec from our high-res uptime. */
        int rc = time_get(out);
        if (rc == 0) out->nanosec = (uint32_t)((us % 1000000ULL) * 1000ULL);
        return rc;
    }

    uint64_t unix_now = boot + (us / 1000000ULL);
    out->seconds      = unix_now;
    out->nanosec      = (uint32_t)((us % 1000000ULL) * 1000ULL);
    unix_to_civil(unix_now, out);
    return OK;
}
