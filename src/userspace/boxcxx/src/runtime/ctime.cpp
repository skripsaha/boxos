// boxcxx — <ctime> runtime
//
// The calendar arithmetic is <chrono>'s, and deliberately: the tree already had
// three civil-date conversions when this file was written and did not need a
// fourth. days_from_civil / civil_from_days / weekday_from_days are the same
// functions every year_month_day in the library goes through, so a date that
// prints one way through std::format prints the same way through strftime.

#include <chrono>
#include <cstring>
#include <ctime>
#include <string>
#include <__bits/chrono_calendar>
#include <__bits/chrono_format>

#include "box/clock.h"
#include "box/system.h"

namespace {

using ::std::chrono::__detail::civil_from_days;
using ::std::chrono::__detail::days_from_civil;
using ::std::chrono::__detail::weekday_from_days;

constexpr long long kSecondsPerDay = 86400;

// Floor division, not truncation: a time_t before the epoch must map to the day
// that contains it, and C's / rounds toward zero, which would put 1969-12-31
// 23:59:59 on the wrong day.
constexpr long long FloorDiv(long long a, long long b) noexcept
{
    const long long q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

constexpr long long FloorMod(long long a, long long b) noexcept
{
    return a - FloorDiv(a, b) * b;
}

// Fills a tm from a day number and a second-of-day. Shared by gmtime and by
// mktime's write-back, so the normalised fields a caller reads after mktime are
// produced by the same code that gmtime would have produced.
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

// Per strand, not per process. C makes these buffers process-wide, which in a
// program running strands over one address space means two calls to gmtime can
// hand back the same storage — the standard permits it and BoxOS does not have
// to. Same decision as rand, strtok and the <cuchar> conversion states.
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

} // namespace

namespace std {

clock_t clock() noexcept
{
    uint64_t us = 0;   // matches proc_cpu_time's out parameter exactly
    // The kernel accumulates this at every context switch; a failure here means
    // the manifest call did not land, and C's answer for that is -1 rather than
    // a zero that reads like "no time has passed".
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
    // No timezone database, so local time IS UTC — said plainly rather than
    // approximated. The separate buffer is not cosmetic: C lets a program hold
    // the result of one across a call to the other.
    if (!timer) return nullptr;
    const long long t    = *timer;
    const long long days = FloorDiv(t, kSecondsPerDay);
    FillTm(g_localtime, days, FloorMod(t, kSecondsPerDay));
    return &g_localtime;
}

time_t mktime(tm *timeptr) noexcept
{
    if (!timeptr) return static_cast<time_t>(-1);

    // Every field may arrive out of range — that is what mktime is FOR, and the
    // usual caller is "add a month to this date" written as tm_mon += 1.
    long long mon  = timeptr->tm_mon;
    long long year = static_cast<long long>(timeptr->tm_year) + 1900;

    // The month has to be folded into the year first: the length of the others
    // depends on which month it lands in.
    year += FloorDiv(mon, 12);
    mon = FloorMod(mon, 12);

    const long long days = days_from_civil(year, static_cast<unsigned>(mon) + 1, 1) +
                           (static_cast<long long>(timeptr->tm_mday) - 1);
    const long long secs = days * kSecondsPerDay +
                           static_cast<long long>(timeptr->tm_hour) * 3600 +
                           static_cast<long long>(timeptr->tm_min) * 60 +
                           static_cast<long long>(timeptr->tm_sec);

    // The caller reads the normalised fields back out of *timeptr, so they are
    // produced from the result, not from the input.
    FillTm(*timeptr, FloorDiv(secs, kSecondsPerDay), FloorMod(secs, kSecondsPerDay));
    return static_cast<time_t>(secs);
}

char *asctime(const tm *timeptr) noexcept
{
    if (!timeptr) return nullptr;
    // C fixes the RESULT at 26 bytes, which bounds the year to four digits even
    // though the year itself is written with %d. Outside that the behaviour is
    // undefined; returning nullptr is the version of undefined that cannot
    // write past a buffer whose size C also fixed.
    const int y = timeptr->tm_year + 1900;
    if (y < 0 || y > 9999) return nullptr;
    // Every field feeds a fixed-width column, so a value that cannot fit one
    // would write a character that is not a digit. C leaves that undefined;
    // refusing is the version of undefined that cannot produce a corrupt line.
    if (static_cast<unsigned>(timeptr->tm_wday) > 6u ||
        static_cast<unsigned>(timeptr->tm_mon) > 11u ||
        timeptr->tm_mday < 1 || timeptr->tm_mday > 31 ||
        static_cast<unsigned>(timeptr->tm_hour) > 23u ||
        static_cast<unsigned>(timeptr->tm_min) > 59u ||
        static_cast<unsigned>(timeptr->tm_sec) > 60u)
        return nullptr;

    // C specifies the result as exactly
    //     "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n"
    // and the details matter: the day is %3d, so it carries its OWN leading
    // space and there is none between the month and it; and the year is %d, NOT
    // %4d — year 500 prints as "500" and the string is one character shorter.
    // Zero-padding it to four would be a different string from the one C names.
    char *p = g_asctime;
    ::std::memcpy(p + 0, kWday[timeptr->tm_wday], 3);
    p[3] = ' ';
    ::std::memcpy(p + 4, kMon[timeptr->tm_mon], 3);
    p[7] = ' ';                            // the first column of %3d
    Put2(p + 8, timeptr->tm_mday, ' ');    // its other two
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
    // asctime's buffer belongs to asctime: a program is allowed to hold what
    // ctime returned across a call to asctime, so this cannot alias it. The
    // length is not fixed — the year is %d — so it is measured, not assumed.
    const ::std::size_t n = ::std::strlen(s);
    ::std::memcpy(g_ctime, s, n + 1);
    return g_ctime;
}

size_t strftime(char *s, size_t maxsize, const char *format, const tm *timeptr) noexcept
{
    if (!s || !format || !timeptr || maxsize == 0) return 0;

    __chrono_fmt::Parts p;
    p.year      = static_cast<long long>(timeptr->tm_year) + 1900;
    p.month     = static_cast<unsigned>(timeptr->tm_mon) + 1u;
    p.day       = static_cast<unsigned>(timeptr->tm_mday);
    p.h         = timeptr->tm_hour;
    p.mi        = timeptr->tm_min;
    p.s         = timeptr->tm_sec;
    p.sub       = 0;
    p.subw      = 0; // a tm has no subsecond, so %S prints no fraction
    p.wd        = static_cast<unsigned>(timeptr->tm_wday);
    p.yday      = static_cast<long long>(timeptr->tm_yday) + 1; // render counts from 1
    p.have_date = true;
    p.have_time = true;
    p.mon_ok    = static_cast<unsigned>(timeptr->tm_mon) < 12u;
    p.wd_ok     = static_cast<unsigned>(timeptr->tm_wday) < 7u;
    p.date_ok   = true;
    // localtime is gmtime here, so the zone is not unknown — it is UTC, and %Z
    // and %z say so rather than throwing or printing nothing.
    p.zone     = "UTC";
    p.has_zone = true;
    p.has_zoff = true;
    p.zoff_sec = 0;

    string out;
    try {
        // string_view is explicit now that render() is a template over the
        // format string's character type: strftime's is a narrow C string, and
        // a const char* does not deduce a basic_string_view.
        __chrono_fmt::render(out, string_view(format), p);
    } catch (...) {
        // render throws for a conversion it does not know; C calls that
        // undefined and every implementation returns 0. An exception must not
        // cross into a C caller.
        return 0;
    }

    // C counts the terminator against maxsize, and on overflow returns 0 with
    // the buffer left unspecified.
    if (out.size() >= maxsize) return 0;
    ::std::memcpy(s, out.data(), out.size());
    s[out.size()] = '\0';
    return out.size();
}

} // namespace std
