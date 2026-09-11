#include "box/clock.h"
#include "box/time.h"


static const ClockBoardView *cb(void)
{
    return (const ClockBoardView *)(uintptr_t)CABIN_CLOCKBOARD_ADDR;
}

bool clock_available(void)
{
    const ClockBoardView *v = cb();
    return v->magic == CLOCKBOARD_MAGIC_USER && v->version >= 1u;
}

uint64_t clock_uptime_us(void)
{
    const ClockBoardView *v = cb();
    if (v->magic == CLOCKBOARD_MAGIC_USER) {
        return v->uptime_us;
    }
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
    uint64_t secs = 0;
    if (time_get_secs(&secs) == 0) return secs;
    return 0;
}

uint64_t clock_unix_now_ns(void)
{
    const ClockBoardView *v = cb();
    if (v->magic == CLOCKBOARD_MAGIC_USER && v->boot_unix_secs != 0) {
        uint64_t us  = v->uptime_us;
        uint64_t sec = v->boot_unix_secs + us / 1000000ULL;
        uint64_t sub = us % 1000000ULL;
        return sec * 1000000000ULL + sub * 1000ULL;
    }
    uint64_t secs = 0;
    if (time_get_secs(&secs) == 0) return secs * 1000000000ULL;
    return 0;
}

uint64_t clock_tsc_to_ns(uint64_t tsc_ticks)
{
    const ClockBoardView *v = cb();
    uint64_t khz = (v->magic == CLOCKBOARD_MAGIC_USER) ? v->tsc_freq_khz : 0;
    if (khz == 0) return 0;
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


static void unix_to_civil(uint64_t unix_secs, BoxTime *out)
{
    uint64_t days     = unix_secs / 86400ULL;
    uint32_t sec_day  = (uint32_t)(unix_secs % 86400ULL);

    out->weekday = (uint8_t)((days + 4ULL) % 7ULL);

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

int clock_boxtime(BoxTime *out)
{
    if (!out) return -ERR_NULL_POINTER;

    const ClockBoardView *v = cb();
    if (v->magic != CLOCKBOARD_MAGIC_USER) {
        return time_get(out);
    }

    uint64_t us   = v->uptime_us;
    uint64_t boot = v->boot_unix_secs;
    if (boot == 0) {
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