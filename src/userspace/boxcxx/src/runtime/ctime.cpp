
#include <chrono>
#include <cstring>
#include <ctime>
#include <string>
#include <__bits/chrono_calendar>
#include <__bits/chrono_format>
#include <__bits/time_render>

#include "box/clock.h"
#include "box/system.h"

namespace {

using ::std::chrono::__detail::civil_from_days;
using ::std::chrono::__detail::days_from_civil;
using ::std::chrono::__detail::weekday_from_days;

constexpr long long kSecondsPerDay = 86400;

constexpr long long FloorDiv(long long a, long long b) noexcept
{
    const long long q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

constexpr long long FloorMod(long long a, long long b) noexcept
{
    return a - FloorDiv(a, b) * b;
}

void FillTm(::std::tm &out, long long days, long long sod) noexcept
{
    long long y = 0;
    unsigned  m = 0, d = 0;
    civil_from_days(days, y, m, d);

    out.tm_sec   = static_cast<int>(sod % 60);
    out.tm_min   = static_cast<int>((sod / 60) % 60);
    out.tm_hour  = static_cast<int>(sod / 3600);
    out.tm_mday  = static_cast<int>(d);
    out.tm_mon   = static_cast<int>(m) - 1;
    out.tm_year  = static_cast<int>(y - 1900);
    out.tm_wday  = static_cast<int>(weekday_from_days(days));
    out.tm_yday  = static_cast<int>(days - days_from_civil(y, 1, 1));
    out.tm_isdst = 0;
}

thread_local ::std::tm g_gmtime;
thread_local ::std::tm g_localtime;
thread_local char      g_asctime[32];
thread_local char      g_ctime[32];

constexpr const char kWday[7][4] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char kMon[12][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void Put2(char *p, int v, char pad) noexcept
{
    p[0] = v / 10 ? static_cast<char>('0' + v / 10) : pad;
    p[1] = static_cast<char>('0' + v % 10);
}

}

namespace std {

clock_t clock() noexcept
{
    uint64_t us = 0;
    if (::proc_cpu_time(&us) != 0) return static_cast<clock_t>(-1);
    return static_cast<clock_t>(us);
}

time_t time(time_t *timer) noexcept
{
    const time_t now = static_cast<time_t>(::clock_unix_now());
    if (timer) *timer = now;
    return now;
}

int timespec_get(timespec *ts, int base) noexcept
{
    if (!ts || base != TIME_UTC) return 0;
    const uint64_t ns = ::clock_unix_now_ns();
    ts->tv_sec  = static_cast<time_t>(ns / 1000000000ULL);
    ts->tv_nsec = static_cast<long>(ns % 1000000000ULL);
    return base;
}

tm *gmtime(const time_t *timer) noexcept
{
    if (!timer) return nullptr;
    const long long t    = *timer;
    const long long days = FloorDiv(t, kSecondsPerDay);
    FillTm(g_gmtime, days, FloorMod(t, kSecondsPerDay));
    return &g_gmtime;
}

tm *localtime(const time_t *timer) noexcept
{
    if (!timer) return nullptr;
    const long long t    = *timer;
    const long long days = FloorDiv(t, kSecondsPerDay);
    FillTm(g_localtime, days, FloorMod(t, kSecondsPerDay));
    return &g_localtime;
}

time_t mktime(tm *timeptr) noexcept
{
    if (!timeptr) return static_cast<time_t>(-1);

    long long mon  = timeptr->tm_mon;
    long long year = static_cast<long long>(timeptr->tm_year) + 1900;

    year += FloorDiv(mon, 12);
    mon = FloorMod(mon, 12);

    const long long days = days_from_civil(year, static_cast<unsigned>(mon) + 1, 1) +
                           (static_cast<long long>(timeptr->tm_mday) - 1);
    const long long secs = days * kSecondsPerDay +
                           static_cast<long long>(timeptr->tm_hour) * 3600 +
                           static_cast<long long>(timeptr->tm_min) * 60 +
                           static_cast<long long>(timeptr->tm_sec);

    FillTm(*timeptr, FloorDiv(secs, kSecondsPerDay), FloorMod(secs, kSecondsPerDay));
    return static_cast<time_t>(secs);
}

char *asctime(const tm *timeptr) noexcept
{
    if (!timeptr) return nullptr;
    const int y = timeptr->tm_year + 1900;
    if (y < 0 || y > 9999) return nullptr;
    if (static_cast<unsigned>(timeptr->tm_wday) > 6u ||
        static_cast<unsigned>(timeptr->tm_mon) > 11u ||
        timeptr->tm_mday < 1 || timeptr->tm_mday > 31 ||
        static_cast<unsigned>(timeptr->tm_hour) > 23u ||
        static_cast<unsigned>(timeptr->tm_min) > 59u ||
        static_cast<unsigned>(timeptr->tm_sec) > 60u)
        return nullptr;

    char *p = g_asctime;
    ::std::memcpy(p + 0, kWday[timeptr->tm_wday], 3);
    p[3] = ' ';
    ::std::memcpy(p + 4, kMon[timeptr->tm_mon], 3);
    p[7] = ' ';
    Put2(p + 8, timeptr->tm_mday, ' ');
    p[10] = ' ';
    Put2(p + 11, timeptr->tm_hour, '0');
    p[13] = ':';
    Put2(p + 14, timeptr->tm_min, '0');
    p[16] = ':';
    Put2(p + 17, timeptr->tm_sec, '0');
    p[19] = ' ';

    int n = 20;
    if (y >= 1000) p[n++] = static_cast<char>('0' + (y / 1000) % 10);
    if (y >= 100)  p[n++] = static_cast<char>('0' + (y / 100) % 10);
    if (y >= 10)   p[n++] = static_cast<char>('0' + (y / 10) % 10);
    p[n++] = static_cast<char>('0' + y % 10);
    p[n++] = '\n';
    p[n]   = '\0';
    return g_asctime;
}

char *ctime(const time_t *timer) noexcept
{
    const tm *t = localtime(timer);
    if (!t) return nullptr;
    char *s = asctime(t);
    if (!s) return nullptr;
    const ::std::size_t n = ::std::strlen(s);
    ::std::memcpy(g_ctime, s, n + 1);
    return g_ctime;
}

static __chrono_fmt::Parts PartsFromTm(const tm &t) noexcept
{
    __chrono_fmt::Parts p;
    p.year      = static_cast<long long>(t.tm_year) + 1900;
    p.month     = static_cast<unsigned>(t.tm_mon) + 1u;
    p.day       = static_cast<unsigned>(t.tm_mday);
    p.h         = t.tm_hour;
    p.mi        = t.tm_min;
    p.s         = t.tm_sec;
    p.sub       = 0;
    p.subw      = 0;
    p.wd        = static_cast<unsigned>(t.tm_wday);
    p.yday      = static_cast<long long>(t.tm_yday) + 1;
    p.have_date = true;
    p.have_time = true;
    p.mon_ok    = static_cast<unsigned>(t.tm_mon) < 12u;
    p.wd_ok     = static_cast<unsigned>(t.tm_wday) < 7u;
    p.date_ok   = true;
    p.zone     = "UTC";
    p.has_zone = true;
    p.has_zoff = true;
    p.zoff_sec = 0;
    return p;
}

namespace __timeput {

bool RenderTm(string &out, char spec, char mod, const tm &t) noexcept
{
    try {
        __chrono_fmt::RenderOne(out, spec, mod, PartsFromTm(t));
    } catch (...) {
        return false;
    }
    return true;
}

}

size_t strftime(char *s, size_t maxsize, const char *format, const tm *timeptr) noexcept
{
    if (!s || !format || !timeptr || maxsize == 0) return 0;

    const __chrono_fmt::Parts p = PartsFromTm(*timeptr);

    string out;
    try {
        __chrono_fmt::render(out, string_view(format), p);
    } catch (...) {
        return 0;
    }

    if (out.size() >= maxsize) return 0;
    ::std::memcpy(s, out.data(), out.size());
    s[out.size()] = '\0';
    return out.size();
}

}