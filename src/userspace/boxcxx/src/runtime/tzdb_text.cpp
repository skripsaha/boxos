
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

}
}
}