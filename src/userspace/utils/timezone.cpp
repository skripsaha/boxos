
#include <box/cxx/clock.h>
#include <box/cxx/luggage.h>

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
    std::printf("offset:   %s\n", std::format("{:%z}", here).c_str());

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
    for (const auto &l : db.links) {
        const std::string_view name = l.name();
        if (n && name.substr(0, n) != want) continue;
        const std::string_view target = l.target();
        std::printf("%.*s -> %.*s\n", static_cast<int>(name.size()), name.data(),
                    static_cast<int>(target.size()), target.data());
        ++shown;
    }

    if (shown == 0) {
        if (want.empty()) std::printf("timezone: this database has no zones\n");
        else std::printf("timezone: nothing matches `%.*s`\n",
                         static_cast<int>(want.size()), want.data());
        return 1;
    }
    return 0;
}

}

int main()
{
    const std::size_t words = box::luggage::size();
    if (words <= 1) return Show();

    if (box::luggage::word(1) == "list")
        return List(words > 2 ? box::luggage::word(2) : std::string_view{});
    if (words == 2) return Set(box::luggage::word(1));

    std::printf("usage: timezone [<name> | list [prefix]]\n");
    return 1;
}