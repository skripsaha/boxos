// timezone — show or set the reckoning of time this machine keeps.
//
// The reading half of `clock:zone` lives in the library: std::chrono's
// current_zone() consults the tag on every call. This is the other half, and
// it is a C++ program rather than a shell builtin for one reason — the shell is
// C and the zone database is not, so a builtin could only write whatever name
// it was handed. A name the database does not know reads back as Etc/UTC, and
// finding that out an hour later from a clock that is quietly wrong is not a
// good way to learn you typed Europe/Moskva. box::clock::set_zone validates
// before it writes, and stores the canonical spelling it got back.
//
//   timezone                     what the machine keeps, and the time there now
//   timezone <name>              set it
//   timezone list [prefix]       the names that match
//
// Setting it multicasts a Touch on `clock:zone`, so a clock already on screen
// can redraw without polling.

#include <box/cxx/clock.h>
#include <box/cxx/luggage.h>   // box::luggage — how BoxOS delivers arguments

#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>

namespace {

int Show()
{
    const std::string stored = box::clock::zone_setting();
    const std::chrono::time_zone *tz = std::chrono::current_zone();
    const std::string in_use(tz->name());

    const auto now = std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::now());
    const std::chrono::zoned_time here{tz, now};

    if (stored.empty())
        std::printf("timezone: not set, so the machine keeps %s\n",
                    in_use.c_str());
    else
        std::printf("timezone: %s\n", in_use.c_str());

    std::printf("now:      %s\n", std::format("{:%F %T %Z}", here).c_str());
    // Asked of the zoned_time, not of its offset: a bare duration has no zone,
    // so %z on one is meaningless and <format> says so at compile time. And
    // the standard does not require a formatter for sys_info, so the offset
    // cannot be printed through the info struct either.
    std::printf("offset:   %s\n", std::format("{:%z}", here).c_str());

    // The one case where what is stored and what is used disagree. It can only
    // happen to a setting written before the zone database changed under it,
    // because set_zone refuses a name the database does not know.
    if (!stored.empty() && stored != in_use)
        std::printf("note:     `%s` is stored, but this database has no zone by "
                    "that name, so the machine is on %s\n",
                    stored.c_str(), in_use.c_str());
    return 0;
}

int Set(std::string_view want)
{
    if (auto done = box::clock::set_zone(want); !done) {
        if (done.error().code() == box::errc::invalid_argument)
            // ASCII only in what reaches the console: the guest's text screen
            // rendered an em dash here as a bare `?`.
            std::printf("timezone: no zone named `%.*s` - try `timezone list %.*s`\n",
                        static_cast<int>(want.size()), want.data(),
                        static_cast<int>(want.size()), want.data());
        else
            std::printf("timezone: could not store the setting (%.*s)\n",
                        static_cast<int>(done.error().message().size()),
                        done.error().message().data());
        return 1;
    }
    return Show();
}

int List(std::string_view want)
{
    const auto &db = std::chrono::get_tzdb();
    const std::size_t n = want.size();
    int shown = 0;

    for (const auto &z : db.zones) {
        const std::string_view name = z.name();
        if (n && name.substr(0, n) != want) continue;
        std::printf("%.*s\n", static_cast<int>(name.size()), name.data());
        ++shown;
    }
    // Links last and marked, because a link is a name that resolves elsewhere
    // and a list that hid that would make two different things look alike.
    for (const auto &l : db.links) {
        const std::string_view name = l.name();
        if (n && name.substr(0, n) != want) continue;
        const std::string_view target = l.target();
        std::printf("%.*s -> %.*s\n", static_cast<int>(name.size()), name.data(),
                    static_cast<int>(target.size()), target.data());
        ++shown;
    }

    if (shown == 0) {
        // No prefix and nothing shown means an empty database, which is not a
        // question about the prefix. Split out because an empty string_view's
        // data() is a null pointer, and handing that to printf is undefined
        // even where the precision says not to read it.
        if (want.empty()) std::printf("timezone: this database has no zones\n");
        else std::printf("timezone: nothing matches `%.*s`\n",
                         static_cast<int>(want.size()), want.data());
        return 1;
    }
    return 0;
}

}  // namespace

// ‼ No (argc, argv), and that is the point rather than an omission. BoxOS does
// not deliver arguments through the entry point at all: boxlib_start.asm passes
// argc=0 and argv=NULL purely so a C main() signature still compiles, and the
// real arguments are the program's Luggage — the line as typed, in its cabin
// before main runs (box/cxx/luggage.h). Writing the POSIX shape here does not
// fail to find the arguments — it reads argv[1] through a null pointer and
// takes a #PF at address 8, which is exactly what this command did the first
// time it was run in the guest.
int main()
{
    const std::size_t words = box::luggage::size();   // word 0 is the command's name
    if (words <= 1) return Show();

    if (box::luggage::word(1) == "list")
        return List(words > 2 ? box::luggage::word(2) : std::string_view{});
    if (words == 2) return Set(box::luggage::word(1));

    std::printf("usage: timezone [<name> | list [prefix]]\n");
    return 1;
}
