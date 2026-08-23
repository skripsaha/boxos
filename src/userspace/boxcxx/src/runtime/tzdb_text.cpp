// tzdb_text.cpp — the words of the two [time.zone.exception] classes.
//
// Split out of tzdb.cpp on purpose. These two functions render dates, so they
// reference the <format> engine and its Unicode tables; a linker pulls whole
// archive members, so leaving them next to the 116 KB zone table would have
// made a program that only calls locate_zone() pay for the formatter as well.
// Here, they are pulled only by a program that converts a local time — which
// is exactly the program that can throw one of these.

#include <chrono>
#include <format>
#include <string>

namespace std {
namespace chrono {
namespace __detail {

string nonexistent_text(local_seconds tp, const local_info &i)
{
    return format("{:%F %T} is in a gap between {:%F %T} {} and {:%F %T} {}, "
                  "which are both {:%F %T} UTC",
                  tp,
                  local_seconds(i.first.end.time_since_epoch() + i.first.offset),
                  i.first.abbrev,
                  local_seconds(i.second.begin.time_since_epoch() + i.second.offset),
                  i.second.abbrev,
                  i.first.end);
}

string ambiguous_text(local_seconds tp, const local_info &i)
{
    return format("{:%F %T} is ambiguous: it is {:%F %T} UTC as {} and "
                  "{:%F %T} UTC as {}",
                  tp,
                  sys_seconds(tp.time_since_epoch() - i.first.offset),
                  i.first.abbrev,
                  sys_seconds(tp.time_since_epoch() - i.second.offset),
                  i.second.abbrev);
}

} // namespace __detail
} // namespace chrono
} // namespace std
