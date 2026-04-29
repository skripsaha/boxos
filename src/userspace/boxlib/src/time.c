/*
 * time.c — userspace RTC/timer wrappers (Phase 12: Manifest-only).
 */

#include "box/time.h"
#include "box/core/manifest.h"
#include "box/core/notify.h"
#include "box/string.h"
#include "box/core/result.h"

#define HW_TIMER_GET_MS    0x11
#define HW_RTC_GET_TIME    0x15
#define HW_RTC_GET_UNIX64  0x16
#define HW_RTC_GET_UPTIME  0x17

#define TIME_TIMEOUT_MS    50000u

int time_get(time_t *out)
{
    if (!out) return ERR_NULL_POINTER;

    /* Out-crate layout matches kernel's time_t struct (20 bytes). */
    uint8_t buf[20] = {0};
    int rc = MfCall1(DECK_HARDWARE, HW_RTC_GET_TIME,
                     NULL, 0, NULL, 0,
                     buf, sizeof(buf), NULL,
                     TIME_TIMEOUT_MS, NULL);
    if (rc != 0) return rc;

    memcpy(&out->seconds, buf + 0,  8);
    memcpy(&out->nanosec, buf + 8,  4);
    memcpy(&out->year,    buf + 12, 2);
    out->month   = buf[14];
    out->day     = buf[15];
    out->hour    = buf[16];
    out->minute  = buf[17];
    out->second  = buf[18];
    out->weekday = buf[19];
    return OK;
}

int time_get_secs(uint64_t *out_seconds)
{
    if (!out_seconds) return ERR_NULL_POINTER;
    uint8_t buf[8] = {0};
    int rc = MfCall1(DECK_HARDWARE, HW_RTC_GET_UNIX64,
                     NULL, 0, NULL, 0,
                     buf, sizeof(buf), NULL,
                     TIME_TIMEOUT_MS, NULL);
    if (rc != 0) return rc;
    memcpy(out_seconds, buf, 8);
    return OK;
}

int time_uptime_ms(uint64_t *out_ms)
{
    if (!out_ms) return ERR_NULL_POINTER;
    uint8_t buf[8] = {0};
    int rc = MfCall1(DECK_HARDWARE, HW_TIMER_GET_MS,
                     NULL, 0, NULL, 0,
                     buf, sizeof(buf), NULL,
                     TIME_TIMEOUT_MS, NULL);
    if (rc != 0) return rc;
    memcpy(out_ms, buf, 8);
    return OK;
}

int time_uptime_ns(uint64_t *out_ns)
{
    if (!out_ns) return ERR_NULL_POINTER;
    uint8_t buf[8] = {0};
    int rc = MfCall1(DECK_HARDWARE, HW_RTC_GET_UPTIME,
                     NULL, 0, NULL, 0,
                     buf, sizeof(buf), NULL,
                     TIME_TIMEOUT_MS, NULL);
    if (rc != 0) return rc;
    memcpy(out_ns, buf, 8);
    return OK;
}

/* ------------------------------------------------------------------------- */

static void write2(char *buf, uint8_t v)
{
    buf[0] = (char)('0' + v / 10);
    buf[1] = (char)('0' + v % 10);
}

static void write4(char *buf, uint16_t v)
{
    buf[0] = (char)('0' + v / 1000);
    buf[1] = (char)('0' + (v / 100) % 10);
    buf[2] = (char)('0' + (v / 10) % 10);
    buf[3] = (char)('0' + v % 10);
}

int64_t time_diff(const time_t *a, const time_t *b)
{
    if (!a || !b) return 0;
    int64_t sec_diff = (int64_t)a->seconds - (int64_t)b->seconds;
    int64_t ns_diff  = (int64_t)a->nanosec - (int64_t)b->nanosec;
    return sec_diff * 1000 + ns_diff / 1000000;
}

void time_add_ms(const time_t *t, int64_t ms, time_t *out)
{
    if (!t || !out) return;

    int64_t total_ns  = (int64_t)t->nanosec + (ms % 1000) * 1000000;
    int64_t total_sec = (int64_t)t->seconds + ms / 1000;

    if (total_ns >= 1000000000) { total_sec += 1; total_ns -= 1000000000; }
    else if (total_ns < 0)      { total_sec -= 1; total_ns += 1000000000; }

    out->seconds = (uint64_t)total_sec;
    out->nanosec = (uint32_t)total_ns;

    uint64_t rem = (uint64_t)total_sec;
    uint32_t days = (uint32_t)(rem / 86400);
    rem %= 86400;
    out->hour   = (uint8_t)(rem / 3600);
    rem %= 3600;
    out->minute = (uint8_t)(rem / 60);
    out->second = (uint8_t)(rem % 60);
    out->weekday = (uint8_t)((days + 4) % 7);

    int32_t y = 1970;
    while (1) {
        uint32_t ydays = 365;
        if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ydays = 366;
        if (days < ydays) break;
        days -= ydays;
        y++;
    }
    out->year = (uint16_t)y;

    static const uint8_t mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint8_t leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 1 : 0;
    uint8_t m;
    for (m = 0; m < 12; m++) {
        uint8_t md = mdays[m] + ((m == 1) ? leap : 0);
        if (days < md) break;
        days -= md;
    }
    out->month = m + 1;
    out->day   = (uint8_t)(days + 1);
}

int time_format(const time_t *t, char *buf, size_t buf_size)
{
    if (!t || !buf) return ERR_NULL_POINTER;
    if (buf_size < 20) return ERR_BUFFER_TOO_SMALL;

    write4(buf, t->year);
    buf[4] = '-';
    write2(buf + 5, t->month);
    buf[7] = '-';
    write2(buf + 8, t->day);
    buf[10] = ' ';
    write2(buf + 11, t->hour);
    buf[13] = ':';
    write2(buf + 14, t->minute);
    buf[16] = ':';
    write2(buf + 17, t->second);
    buf[19] = '\0';
    return OK;
}
