// tzdb.cpp — the one translation unit that carries the IANA table.
//
// <__bits/chrono_zone> declares four functions and nothing else reaches the
// data; they are defined here, and <__bits/tzdb_table> is included here and
// nowhere else in the library. That is not tidiness — it is what makes the
// 116 KB cost conditional. The linker pulls an archive member only when
// something references one of its symbols, so a program that includes <chrono>
// for durations, calendars or formatting never links this object and never
// carries a byte of zone data. A program that asks a zone a question links all
// of it, once.
//
// The reader itself is <__bits/tzdb_read>: no std types, seconds as long long,
// so the same source compiles on a development host and is measured there
// against libstdc++ and against tzcode's own zdump over 919 417 instants.
// What this file adds is the standard's shape — strings, vectors, exceptions —
// and the two things only BoxOS can answer: which zone this machine is in, and
// whether a newer table has been laid in TagFS.
//
// The text of the two [time.zone.exception] classes is NOT here, and the split
// is load-bearing rather than cosmetic. Those messages render dates, so they
// reference the <format> engine — and a linker pulls whole archive members, so
// keeping them beside the table made every program that merely calls
// locate_zone() drag in the formatter and its Unicode tables. They live in
// tzdb_text.cpp, which is pulled only by a program that actually converts a
// local time and can therefore actually throw.

#include <__bits/tzdb_read>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <box/cxx/tagfs.h>

namespace tz = std::__boxcxx::tzdata;

namespace std {
namespace chrono {
namespace __detail {

// ── the four functions <__bits/chrono_zone> is allowed to call ──────────

int zone_find(string_view name) noexcept
{
    return tz::Find(name.data(), static_cast<unsigned>(name.size()));
}

string_view zone_name(int idx) noexcept
{
    if (idx < 0 || idx >= tz::NameCount()) return {};
    return tz::NameAt(idx);
}

static sys_info from_reader(const tz::Info &in)
{
    sys_info out;
    out.begin = in.begin == tz::kBeginning ? sys_seconds::min()
                                           : sys_seconds(seconds(in.begin));
    out.end   = in.end == tz::kForever ? sys_seconds::max()
                                       : sys_seconds(seconds(in.end));
    out.offset = seconds(in.offset);
    // [time.zone.info] types `save` as minutes, and the generator refuses to
    // write a table where any save is not a whole number of them.
    out.save   = duration_cast<minutes>(seconds(in.save));
    out.abbrev = in.abbrev;
    return out;
}

sys_info zone_info_sys(int idx, sys_seconds t)
{
    tz::Info info{};
    if (idx < 0 || !tz::InfoAtSys(tz::BodyOf(idx), t.time_since_epoch().count(),
                                  info))
        throw runtime_error("chrono: no zone information");
    return from_reader(info);
}

local_info zone_info_local(int idx, local_seconds t)
{
    local_info out{};
    if (idx < 0) throw runtime_error("chrono: no zone information");

    tz::Info a{}, b{};
    const tz::LocalResult r = tz::InfoAtLocal(
        tz::BodyOf(idx), t.time_since_epoch().count(), a, b);
    switch (r) {
    case tz::LocalResult::unique:
        out.result = local_info::unique;
        out.first  = from_reader(a);
        break;
    case tz::LocalResult::nonexistent:
        out.result = local_info::nonexistent;
        out.first  = from_reader(a);
        out.second = from_reader(b);
        break;
    case tz::LocalResult::ambiguous:
        out.result = local_info::ambiguous;
        out.first  = from_reader(a);
        out.second = from_reader(b);
        break;
    }
    return out;
}


// ── the leap-second arithmetic the clocks stand on ──────────────────────
// Declared in <__bits/chrono_clock>, computed in <__bits/tzdb_read>, measured
// on the host against libstdc++ at every second around every insertion.
//
// These live beside the zone data rather than in an object of their own, and
// that is a decision rather than an oversight: a program that asks how many
// leap seconds have passed is asking the time zone database a question, and
// the database is one table. Splitting them would save 116 KB for a program
// that wants utc_clock and no zones, at the price of a second format and a
// second thing to verify — the exception text was worth splitting out because
// that split cost nothing, and this one would not be free.

long long leap_elapsed_at_sys(long long t) noexcept
{
    return tz::LeapElapsedAtSys(t);
}

void leap_at_utc(long long u, bool &is_leap, long long &elapsed) noexcept
{
    tz::LeapAtUtc(u, is_leap, elapsed);
}

} // namespace __detail

// ── the database ────────────────────────────────────────────────────────

namespace {

// nameIdx -> position in tzdb::zones, for the names that are Zones.
// A Link resolves through its target's name index first.
struct Built {
    tzdb            db;
    vector<int32_t> zone_at;   // -1 for a Link
};

Built &built()
{
    static Built b = [] {
        Built made;
        made.db.version = tz::Version();

        const int n = tz::NameCount();
        made.zone_at.assign(static_cast<size_t>(n), -1);

        size_t zones = 0, links = 0;
        for (int i = 0; i < n; ++i)
            (tz::LinkTarget(i) == tz::kNoLink ? zones : links) += 1;
        made.db.zones.reserve(zones);      // addresses must not move: locate_zone
        made.db.links.reserve(links);      // hands out pointers into these

        for (int i = 0; i < n; ++i) {
            const int target = tz::LinkTarget(i);
            if (target == tz::kNoLink) {
                made.zone_at[static_cast<size_t>(i)] =
                    static_cast<int32_t>(made.db.zones.size());
                made.db.zones.emplace_back(i);
            } else {
                made.db.links.emplace_back(i, target);
            }
        }

        const int leaps = tz::LeapCount();
        made.db.leap_seconds.reserve(static_cast<size_t>(leaps));
        for (int i = 0; i < leaps; ++i) {
            long long when = 0;
            int value = 0;
            tz::LeapAt(i, when, value);
            made.db.leap_seconds.emplace_back(sys_seconds(seconds(when)),
                                              seconds(value));
        }
        return made;
    }();
    return b;
}

// The machine's own zone, as a TagFS object under the well-known tag.
// Read on every call rather than cached: nothing yet tells a running program
// that the setting changed, and an answer that is merely fast is worth less
// than one that is current. When the Touch multicast on `clock:zone` lands,
// caching becomes an optimisation instead of a way to be wrong.
constexpr const char *kZoneTag      = "clock:zone";
constexpr const char *kZoneFallback = "Etc/UTC";

string configured_zone()
{
    auto files = box::tagfs::query(kZoneTag);
    if (files.empty()) return kZoneFallback;

    char buf[64] = {};
    auto got = files.front().read_at(0, buf, sizeof buf - 1);
    if (!got || *got == 0) return kZoneFallback;

    string_view text(buf, *got);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'
                             || text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    if (text.empty()) return kZoneFallback;
    return string(text);
}

} // namespace

const time_zone *tzdb::locate_zone(string_view tz_name) const
{
    Built &b = built();
    int idx = __detail::zone_find(tz_name);
    if (idx < 0)
        throw runtime_error("chrono: no time zone named " + string(tz_name));

    const int target = tz::LinkTarget(idx);
    if (target != tz::kNoLink) idx = target;

    const int32_t at = b.zone_at[static_cast<size_t>(idx)];
    if (at < 0)
        throw runtime_error("chrono: no time zone named " + string(tz_name));
    return &zones[static_cast<size_t>(at)];
}

const time_zone *tzdb::current_zone() const
{
    const string name = configured_zone();
    // A machine that names a zone the table does not have is a machine whose
    // setting is wrong; UTC is the honest answer, not a throw out of a
    // function every clock display calls.
    int idx = __detail::zone_find(name);
    if (idx < 0) return locate_zone(kZoneFallback);
    return locate_zone(name);
}

tzdb_list &get_tzdb_list()
{
    static tzdb_list list{tzdb_list::__private_tag{}};
    (void)built();
    return list;
}

const tzdb &get_tzdb() { return get_tzdb_list().front(); }

const tzdb &tzdb_list::front() const noexcept { return built().db; }

tzdb_list::const_iterator tzdb_list::begin() const noexcept
{
    return const_iterator(&built().db);
}

tzdb_list::const_iterator tzdb_list::end() const noexcept
{
    return const_iterator(nullptr);
}

tzdb_list::const_iterator tzdb_list::cbegin() const noexcept { return begin(); }
tzdb_list::const_iterator tzdb_list::cend() const noexcept { return end(); }

tzdb_list::const_iterator tzdb_list::erase_after(const_iterator p)
{
    // [time.zone.db.list]/6: erases the entry AFTER p. There is exactly one
    // entry, so there is never one after it, and the answer is end().
    (void)p;
    return end();
}

// [time.zone.db.remote]. BoxOS has no network and no clock that ticks a
// release out of date on its own; what it has is the TagFS door the table was
// designed around. Until something is laid behind that door, the newest
// database in reach IS the one in the image, and saying so is the truth rather
// than a placeholder.
string remote_version() { return built().db.version; }

const tzdb &reload_tzdb()
{
    // [time.zone.db.remote]/2: when the remote version equals the loaded one,
    // reload_tzdb returns the current database and nothing is pushed.
    return get_tzdb();
}

const time_zone *locate_zone(string_view tz_name)
{
    return get_tzdb().locate_zone(tz_name);
}

const time_zone *current_zone() { return get_tzdb().current_zone(); }

} // namespace chrono
} // namespace std
