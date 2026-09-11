#ifndef BOXCXX_BOX_CLOCK_H
#define BOXCXX_BOX_CLOCK_H

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "box/cxx/error.h"
#include "box/cxx/tagfs.h"
#include "box/cxx/touch.h"

namespace box {
namespace clock {

inline constexpr const char *zone_tag  = "clock:zone";
inline constexpr const char *zone_file = "zone";

inline constexpr std::size_t zone_name_max = 63;

namespace _detail {

inline std::string trimmed(const char *p, std::size_t n)
{
    std::string_view text(p, n);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'
                             || text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    return std::string(text);
}

}

inline std::string zone_setting()
{
    auto files = box::tagfs::query(zone_tag);
    if (files.empty()) return {};

    char buf[zone_name_max + 1] = {};
    auto got = files.front().read_at(0, buf, sizeof buf - 1);
    if (!got || *got == 0) return {};
    return _detail::trimmed(buf, *got);
}

inline box::status set_zone(std::string_view name)
{
    const std::chrono::time_zone *tz = nullptr;
    try {
        tz = std::chrono::locate_zone(name);
    } catch (...) {
        return std::unexpected(box::error{box::errc::invalid_argument});
    }

    const std::string canonical(tz->name());

    auto files = box::tagfs::query(zone_tag);
    box::result<box::tagfs::file> f =
        files.empty() ? box::tagfs::create(zone_file, zone_tag)
                      : box::result<box::tagfs::file>(files.front());
    if (!f) return std::unexpected(f.error());

    if (auto shortened = f->truncate(0); !shortened) return shortened;

    auto wrote = f->write_at(0, canonical.data(), canonical.size());
    if (!wrote) return std::unexpected(wrote.error());
    if (*wrote != canonical.size())
        return std::unexpected(box::error{box::errc::write_failed});

    (void)box::publish(box::tag(zone_tag),
                       static_cast<const void *>(canonical.data()),
                       static_cast<std::uint32_t>(canonical.size()));
    return {};
}

}
}

#endif