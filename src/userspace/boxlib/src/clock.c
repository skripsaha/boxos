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

static const ClockBoard *cb(void)
{
    return (const ClockBoard *)(uintptr_t)CABIN_CLOCKBOARD_ADDR;
}

bool clock_available(void)
{
    const ClockBoard *v = cb();
    /* The magic says the page is mapped — a Cabin where the mapping
     * hasn't been set up reads none. */
    return v->magic == CLOCKBOARD_MAGIC;
}

static inline uint64_t clock_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* The microsecond now: what the kernel published at its last tick and the
 * TSC it read with it — taken whole, between two even steps of `seq` — and
 * what the TSC ran since, at the board's rate. Never back: a core whose TSC
 * stands a hair behind the BSP's, or a tick whose clock trails what was
 * interpolated before it, gives the last microsecond again, never an earlier
 * one. */
static uint64_t board_now_us(const ClockBoard *v)
{
    uint64_t seq, us, at;
    do {
        seq = v->seq;
        us  = v->uptime_us;
        at  = v->tsc_at;
    } while ((seq & 1u) || seq != v->seq);
    const uint64_t khz = v->tsc_freq_khz;
    const uint64_t tsc = clock_tsc();
    if (at != 0 && khz != 0 && tsc > at) {
        const uint64_t ran = tsc - at;
        us += ran / khz * 1000u + (ran % khz) * 1000u / khz;
    }
    static volatile uint64_t s_last;
    uint64_t last = __atomic_load_n(&s_last, __ATOMIC_RELAXED);
    while (us > last &&
           !__atomic_compare_exchange_n(&s_last, &last, us, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    return us > last ? us : last;
}

uint64_t clock_uptime_us(void)
{
    const ClockBoard *v = cb();
    if (v->magic == CLOCKBOARD_MAGIC) return board_now_us(v);
    /* Fallback: ask the kernel via the HW deck. */
    uint64_t ms = 0;
    if (time_uptime_ms(&ms) == 0) return ms * 1000ULL;
    return 0;
}

uint64_t clock_uptime_ms(void)
{
    const ClockBoard *v = cb();
    if (v->magic == CLOCKBOARD_MAGIC) return board_now_us(v) / 1000u;
    uint64_t ms = 0;
    if (time_uptime_ms(&ms) == 0) return ms;
    return 0;
}

uint64_t clock_unix_now(void)
{
    const ClockBoard *v = cb();
    if (v->magic == CLOCKBOARD_MAGIC && v->boot_unix_secs != 0) {
        return v->boot_unix_secs + (board_now_us(v) / 1000000ULL);
    }
    /* Fallback: ask the kernel directly. */
    uint64_t secs = 0;
    if (time_get_secs(&secs) == 0) return secs;
    return 0;
}

uint64_t clock_unix_now_ns(void)
{
    const ClockBoard *v = cb();
    if (v->magic == CLOCKBOARD_MAGIC && v->boot_unix_secs != 0) {
        /* One microsecond, split once: boot_unix_secs is immutable post-boot,
         * so seconds and sub-seconds come from the same reading. */
        uint64_t us  = board_now_us(v);
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
    const ClockBoard *v = cb();
    uint64_t khz = (v->magic == CLOCKBOARD_MAGIC) ? v->tsc_freq_khz : 0;
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

static void unix_to_civil(uint64_t unix_secs, BoxTime *out)
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

int clock_boxtime(BoxTime *out)
{
    if (!out) return -ERR_NULL_POINTER;

    const ClockBoard *v = cb();
    if (v->magic != CLOCKBOARD_MAGIC) {
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
