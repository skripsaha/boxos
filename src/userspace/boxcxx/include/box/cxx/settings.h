// boxcxx — box::settings  (what the person set in this program)
//
// The C++ face of box/settings.h for a program's own settings. The program
// says what can be set with BOX_SETTINGS — a tree of things and what each
// has; the person sets it with the utility settings (`settings clock digits
// color amber`) or by hand in <program>.settings. A setting is named by its
// path, its names from the top down:
//
//   box::settings::on("seconds")         — an on-or-off setting
//   box::settings::number("width")       — a number setting: 0 for off
//   box::settings::word("label")         — a word, text or one-of setting, as a copy
//   box::settings::text("greeting")
//   box::settings::choice("hours")
//   box::settings::color("digits color") — a color, 0xRRGGBB
//   box::settings::reload()              — the person's file and the Use Context read again:
//                                          did a value that holds change
//
//   box::settings::watch watch;          — the person's file and the Use Context watched,
//                                          on this strand
//   if (watch.changed()) ...             — saved or changed since the last look, and a value
//                                          changed: read what the program shows again
//
// What the person set in a context's block, [work], holds while the Use
// Context holds its tags (box/settings.h says which block of several). A
// watch looks without waiting, so a program that draws in a loop asks it once
// a turn; the file is read again only when it was saved whole. Asking for a
// setting the program does not declare, or as a kind it is not, is the
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
    static bool          on(const char *path) { return ::settings_on(path); }
    static std::int64_t  number(const char *path) { return ::settings_number(path); }
    static std::string   word(const char *path) { return ::settings_word(path); }
    static std::string   text(const char *path) { return ::settings_text(path); }
    static std::string   choice(const char *path) { return ::settings_choice(path); }
    static std::uint32_t color(const char *path) { return ::settings_color(path); }
    static bool          reload() { return ::settings_reload(); }

    // The person's file and the Use Context watched: Touch on the tag every
    // settings file wears and on the context's word, claimed on the strand
    // that makes the watch, released with it. False when they could not be
    // claimed — changed() then never answers true.
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
