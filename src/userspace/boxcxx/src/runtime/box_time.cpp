// BoxTime's timezone-aware calendar view.
//
// The absolute fields stay on the system/Unix timeline. Only the civil fields
// are projected through the machine's configured IANA zone. Keeping this
// conversion beside boxcxx's tzdb makes the C API usable by C applications
// without duplicating timezone rules in boxlib.

#include <chrono>

#include "box/clock.h"

namespace {

int ConvertToLocal(const BoxTime &utc, BoxTime &out)
{
    using namespace std::chrono;

    try {
        const auto instant = sys_time<nanoseconds>(
            seconds(static_cast<long long>(utc.seconds))
            + nanoseconds(static_cast<long long>(utc.nanosec)));
        const zoned_time<nanoseconds> local{current_zone(), instant};
        const auto local_time = floor<seconds>(local.get_local_time());
        const auto local_day  = floor<days>(local_time);
        const year_month_day date{local_day};
        const hh_mm_ss tod{local_time - local_day};
        const weekday weekday_value{local_day};

        out = utc;
        out.year    = static_cast<uint16_t>(static_cast<int>(date.year()));
        out.month   = static_cast<uint8_t>(unsigned(date.month()));
        out.day     = static_cast<uint8_t>(unsigned(date.day()));
        out.hour    = static_cast<uint8_t>(tod.hours().count());
        out.minute  = static_cast<uint8_t>(tod.minutes().count());
        out.second  = static_cast<uint8_t>(tod.seconds().count());
        out.weekday = static_cast<uint8_t>(weekday_value.c_encoding());
        return OK;
    } catch (...) {
        return ERR_INTERNAL;
    }
}

}  // namespace

extern "C" int clock_boxtime_to_local(const BoxTime *utc, BoxTime *out)
{
    if (!utc || !out) return ERR_NULL_POINTER;

    const BoxTime source = *utc;
    return ConvertToLocal(source, *out);
}

extern "C" int clock_boxtime_local(BoxTime *out)
{
    if (!out) return ERR_NULL_POINTER;

    BoxTime utc{};
    const int rc = clock_boxtime(&utc);
    if (rc != OK) return rc;
    return ConvertToLocal(utc, *out);
}
