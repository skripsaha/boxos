// boxcxx — box::clock  (which reckoning of time this machine keeps)
//
// The reading half of this already exists and is standard: std::chrono's
// current_zone() answers what zone the machine is in, and system_clock answers
// what moment it is. What no standard has a spelling for is SETTING it, because
// setting it is not a question about time — it is a question about this
// machine. So it lives here, in box::, beside the file it writes.
//
//   box::clock::set_zone("Europe/Moscow")   — validate, store, announce
//   box::clock::zone_setting()              — the stored name, or empty
//
// The machine's reckoning of time is a space beside time, not beside switching
// the machine off, which is why none of this is in box::system next to
// reboot().
//
// ── Where it is stored ──────────────────────────────────────────────────────
// In a file of its own kind, told by what it begins with: the line
// "timezone 1", then the zone's name on the next. What a file is is told by
// its first bytes, which the volume keeps on every file's card, so any program
// finds the setting by asking which files begin so — no file is read for it
// and nobody's tags are asked for. Of several, the one made last is the
// machine's: the one set_zone writes and every reader reads.
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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "box/cxx/error.h"
#include "box/cxx/tagfs.h"
#include "box/cxx/touch.h"

namespace box {
namespace clock {

// What the setting's file begins with, the name a new one is given, and the
// Touch tag a change of the zone is announced on.
inline constexpr std::string_view zone_header = "timezone 1\n";
inline constexpr const char      *zone_file   = "zone";
inline constexpr const char      *zone_tag    = "clock:zone";

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

// The setting's file: the one made last of those that begin as one.
inline std::optional<box::tagfs::file> setting_file()
{
    std::vector<box::tagfs::file> files = box::tagfs::files_starting_with(zone_header);
    if (files.empty()) return std::nullopt;
    return files.back();
}

}  // namespace _detail

// What is STORED, which is not always what the machine uses: a setting naming
// a zone the database does not have reads back here as itself and out of
// current_zone() as Etc/UTC. Keeping the two answers distinct is what lets a
// command say so instead of hiding it. Empty when nothing has been set.
inline std::string zone_setting()
{
    std::optional<box::tagfs::file> f = _detail::setting_file();
    if (!f) return {};

    char buf[zone_header.size() + zone_name_max + 1] = {};
    auto got = f->read_at(0, buf, sizeof buf - 1);
    if (!got || *got <= zone_header.size() || std::string_view(buf, zone_header.size()) != zone_header) return {};
    std::string_view name(buf + zone_header.size(), *got - zone_header.size());
    return _detail::trimmed(name.data(), name.substr(0, name.find('\n')).size());
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

    // A new setting's file is the program's own and wears no tag: what it is
    // is what it begins with.
    std::optional<box::tagfs::file> found = _detail::setting_file();
    box::result<box::tagfs::file>   f     = found ? box::result<box::tagfs::file>(*found)
                                                  : box::tagfs::create(box::tagfs::my_tags, zone_file);
    if (!f) return std::unexpected(f.error());

    // Written whole, then cut to its length. A shorter name written over a
    // longer one would otherwise leave the tail of the old one behind —
    // "Etc/UTC" over "Europe/Moscow" reads back as "Etc/UTCe/Moscow" — and
    // cutting after the write, not before, keeps the file beginning as a
    // setting at every moment, so no reader meanwhile finds none and no other
    // set_zone makes a second.
    const std::string text = std::string(zone_header) + canonical + "\n";
    auto wrote = f->write_at(0, text.data(), text.size());
    if (!wrote) return std::unexpected(wrote.error());
    if (*wrote != text.size())
        return std::unexpected(box::error{box::errc::write_failed});
    if (auto cut = f->span(text.size()); !cut) return cut;

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
