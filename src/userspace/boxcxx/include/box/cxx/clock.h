// boxcxx — box::clock  (which reckoning of time this machine keeps)
//
// The reading half of this already exists and is standard: std::chrono's
// current_zone() answers what zone the machine is in, and system_clock answers
// what moment it is. What no standard has a spelling for is SETTING it, because
// setting it is not a question about time — it is a question about this
// machine. So it lives here, in box::, beside the tag it writes.
//
//   box::clock::set_zone("Europe/Moscow")   — validate, store, announce
//   box::clock::zone_setting()              — the stored name, or empty
//
// The tag is `clock:zone`, and the namespace is its mirror: the machine's
// reckoning of time is a space beside time, not beside switching the machine
// off, which is why none of this is in box::system next to reboot().
//
// ── Why setting it validates ────────────────────────────────────────────────
// A name the zone database does not know reads back as Etc/UTC. Storing one
// unchecked means finding out an hour later, from a clock that is quietly
// wrong, that you typed Europe/Moskva. set_zone() resolves the name against
// the database FIRST and stores the canonical spelling the database gave back,
// so what is on disk is always a name that resolves.
//
// ── Why setting it announces ────────────────────────────────────────────────
// A program that asked once and kept the answer has no other way to learn the
// machine moved. The write publishes a Touch on `clock:zone` carrying the new
// name, so a clock on screen can redraw without ever polling — a program that
// merely calls current_zone() again does not need it, because that call looks
// every time.
//
// This is a box:: extension, not part of std.
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

// The tag the setting lives under, and the name of the object that carries it.
inline constexpr const char *zone_tag  = "clock:zone";
inline constexpr const char *zone_file = "zone";

// The longest name in IANA 2026c is 32 characters; this is the buffer the
// setting is read through, generous enough that a longer one in some future
// release still fits whole rather than being silently cut in half.
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

}  // namespace _detail

// What is STORED, which is not always what the machine uses: a setting naming
// a zone the database does not have reads back here as itself and out of
// current_zone() as Etc/UTC. Keeping the two answers distinct is what lets a
// command say so instead of hiding it. Empty when nothing has been set.
inline std::string zone_setting()
{
    auto files = box::tagfs::query(zone_tag);
    if (files.empty()) return {};

    char buf[zone_name_max + 1] = {};
    auto got = files.front().read_at(0, buf, sizeof buf - 1);
    if (!got || *got == 0) return {};
    return _detail::trimmed(buf, *got);
}

// Set the zone this machine keeps.
//
// invalid_argument when no zone by that name exists — checked against the
// database before anything is written, so a rejected name leaves the machine
// exactly as it was. Otherwise whatever TagFS said, or empty on success.
inline box::status set_zone(std::string_view name)
{
    const std::chrono::time_zone *tz = nullptr;
    try {
        tz = std::chrono::locate_zone(name);
    } catch (...) {
        return std::unexpected(box::error{box::errc::invalid_argument});
    }

    // The canonical spelling, so a Link ("US/Eastern") is stored as the Zone
    // it resolves to and the setting never has to be resolved twice.
    const std::string canonical(tz->name());

    auto files = box::tagfs::query(zone_tag);
    box::result<box::tagfs::file> f =
        files.empty() ? box::tagfs::create(zone_file, zone_tag)
                      : box::result<box::tagfs::file>(files.front());
    if (!f) return std::unexpected(f.error());

    // Truncate first. A shorter name written over a longer one would otherwise
    // leave the tail of the old one behind — "Etc/UTC" over "Europe/Moscow"
    // reads back as "Etc/UTCe/Moscow". TagFS learned to shorten a file in Ф36;
    // before that this could not have been written correctly at all.
    if (auto shortened = f->truncate(0); !shortened) return shortened;

    auto wrote = f->write_at(0, canonical.data(), canonical.size());
    if (!wrote) return std::unexpected(wrote.error());
    if (*wrote != canonical.size())
        return std::unexpected(box::error{box::errc::write_failed});

    // Tell whoever is already running. Best-effort on purpose: the setting is
    // stored and correct whether or not anybody was listening, and failing the
    // call because nobody was would be a lie about what happened.
    //
    // The cast to const void* is what picks the raw (pointer, length) overload
    // rather than the by-value template — see the note beside box::publish.
    (void)box::publish(box::tag(zone_tag),
                       static_cast<const void *>(canonical.data()),
                       static_cast<std::uint32_t>(canonical.size()));
    return {};
}

}  // namespace clock
}  // namespace box

#endif
