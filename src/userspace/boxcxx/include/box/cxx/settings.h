// boxcxx — box::settings  (what the person set in this program)
//
// The C++ face of box/settings.h for a program's own settings. The program
// says what can be set with BOX_SETTINGS; the person sets it with the utility
// settings (`settings clock hours 12`) or by hand in <program>.settings:
//
//   box::settings::yes("seconds")        — a yes-or-no setting
//   box::settings::number("width")       — a number setting
//   box::settings::word("label")         — a word, text or one-of setting, as a copy
//   box::settings::text("greeting")
//   box::settings::choice("hours")
//   box::settings::color("digits")       — a colour, 0xRRGGBB
//   box::settings::reload()              — the person's file read again: did a value change
//
//   box::settings::watch watch;          — the person's file watched, on this strand
//   if (watch.changed()) ...             — saved since the last look, and a value changed:
//                                          read what the program shows again
//
// A watch looks without waiting, so a program that draws in a loop asks it
// once a turn; the file is read again only when it was saved whole. Asking
// for a setting the program does not declare, or as a kind it is not, is the
// program's mistake: said once, and the answer is false, 0, "" or black.
//
// This is a box:: extension, not part of std.
#ifndef BOXCXX_BOX_SETTINGS_H
#define BOXCXX_BOX_SETTINGS_H

#include <cstdint>
#include <string>

#include "box/settings.h"

namespace box {

struct settings {
    static bool          yes(const char *name) { return ::settings_yes(name); }
    static std::int64_t  number(const char *name) { return ::settings_number(name); }
    static std::string   word(const char *name) { return ::settings_word(name); }
    static std::string   text(const char *name) { return ::settings_text(name); }
    static std::string   choice(const char *name) { return ::settings_choice(name); }
    static std::uint32_t color(const char *name) { return ::settings_color(name); }
    static bool          reload() { return ::settings_reload(); }

    // The person's file watched: Touch on the tag every settings file wears,
    // claimed on the strand that makes the watch, released with it. False
    // when it could not be claimed — changed() then never answers true.
    class watch {
        SettingsWatch *watch_ = nullptr;

    public:
        watch() noexcept
        {
            if (::settings_watch_begin(&watch_) != 0) watch_ = nullptr;
        }
        watch(const watch &)            = delete;
        watch &operator=(const watch &) = delete;
        watch(watch &&other) noexcept : watch_(other.watch_) { other.watch_ = nullptr; }
        watch &operator=(watch &&other) noexcept
        {
            if (this != &other) {
                ::settings_watch_end(watch_);
                watch_       = other.watch_;
                other.watch_ = nullptr;
            }
            return *this;
        }
        ~watch() { ::settings_watch_end(watch_); }

        explicit operator bool() const noexcept { return watch_ != nullptr; }
        bool     changed() noexcept { return ::settings_watch_poll(watch_); }
    };
};

}  // namespace box

#endif  // BOXCXX_BOX_SETTINGS_H
