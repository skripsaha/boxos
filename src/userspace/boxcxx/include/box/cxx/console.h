// boxcxx — box::console  (color, styled text, VGA batch session)
//
// The idiomatic C++ surface over the boxlib screen-presentation API
// (box/color.h, box/vga.h). BoxOS color is NOT ANSI escape codes — it is
// structured metadata attached to a text run (one printf → one syscall), so:
//
//   box::color        — a 24-bit RGB value (r/g/b, named ctors, == ).
//   box::colors::*    — the standard BoxOS palette (black/red/.../berry/ocean…).
//   box::styled       — RAII: set the process-local text color for a scope and
//                       restore it on exit (text printed in the scope inherits
//                       it). It flips state, it does NOT emit escape bytes.
//   box::vga::session — RAII batch: collect VGA ops between begin/commit into a
//                       single Manifest, one kernel re-entry for the whole run.
//   std::formatter<box::color> — formats the color *value* as "#RRGGBB" (a
//                       representation, never a control sequence).
//
// This is a box:: extension, not part of std. The on-screen *rendering* of a
// color is a display-daemon property (GOP 24-bit / VGA 16-color quantized) —
// not observable from a byte log; the color STATE, the formatter, and the VGA
// batch syscall are the parts validated here.
#ifndef BOXCXX_BOX_CONSOLE_H
#define BOXCXX_BOX_CONSOLE_H

#include <cstdint>
#include <format>

#include "box/color.h"  // Color / COLOR_* palette / set_color / get_color[_bg]
#include "box/vga.h"     // vga_begin/commit + putchar/puts/clear/cursor/color/dims

namespace box {

// ── box::color — a 24-bit RGB color value (0x00RRGGBB, or a sentinel) ───────
class color {
    Color v_{COLOR_DEFAULT};

public:
    constexpr color() noexcept = default;
    constexpr explicit color(Color raw) noexcept : v_(raw) {}
    constexpr color(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
        : v_(COLOR_RGB(r, g, b)) {}

    static constexpr color rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept { return color(r, g, b); }
    static constexpr color from_raw(Color raw) noexcept { return color(raw); }
    static constexpr color use_default() noexcept { return color(COLOR_DEFAULT); }
    static constexpr color inherit() noexcept { return color(COLOR_INHERIT); }

    constexpr Color        raw() const noexcept { return v_; }
    constexpr std::uint8_t r() const noexcept { return static_cast<std::uint8_t>(COLOR_R(v_)); }
    constexpr std::uint8_t g() const noexcept { return static_cast<std::uint8_t>(COLOR_G(v_)); }
    constexpr std::uint8_t b() const noexcept { return static_cast<std::uint8_t>(COLOR_B(v_)); }
    constexpr bool         is_default() const noexcept { return v_ == COLOR_DEFAULT; }
    constexpr bool         is_inherit() const noexcept { return v_ == COLOR_INHERIT; }
    // True for a real RGB triple (not a sentinel).
    constexpr bool         is_rgb() const noexcept { return !is_default() && !is_inherit(); }

    friend constexpr bool operator==(color a, color b) noexcept { return a.v_ == b.v_; }
};

// ── box::colors — the standard BoxOS palette (RGB, readable on black) ───────
namespace colors {
inline constexpr color black {COLOR_BLACK};
inline constexpr color white {COLOR_WHITE};
inline constexpr color dark_gray {COLOR_DARK_GRAY};
inline constexpr color light_gray {COLOR_LIGHT_GRAY};
inline constexpr color red {COLOR_RED};
inline constexpr color green {COLOR_GREEN};
inline constexpr color blue {COLOR_BLUE};
inline constexpr color yellow {COLOR_YELLOW};
inline constexpr color cyan {COLOR_CYAN};
inline constexpr color magenta {COLOR_MAGENTA};
inline constexpr color dark_red {COLOR_DARK_RED};
inline constexpr color dark_green {COLOR_DARK_GREEN};
inline constexpr color dark_blue {COLOR_DARK_BLUE};
inline constexpr color brown {COLOR_BROWN};
inline constexpr color dark_cyan {COLOR_DARK_CYAN};
inline constexpr color dark_magenta {COLOR_DARK_MAGENTA};
// BoxOS-flavour accents — unique names, not ANSI clones.
inline constexpr color berry {COLOR_BERRY};
inline constexpr color ocean {COLOR_OCEAN};
inline constexpr color leaf {COLOR_LEAF};
inline constexpr color amber {COLOR_AMBER};
inline constexpr color violet {COLOR_VIOLET};
inline constexpr color teal {COLOR_TEAL};
inline constexpr color coral {COLOR_CORAL};
inline constexpr color slate {COLOR_SLATE};
inline constexpr color use_default = color::use_default();
inline constexpr color inherit = color::inherit();
}  // namespace colors

// ── box::styled — scoped text color (set on entry, restore on exit) ─────────
// Flips the process-local text color (foreground, and optionally background)
// for its lifetime; text written in the scope inherits it. Restores the prior
// color on destruction. No escape bytes are emitted — color is metadata.
class styled {
    Color saved_fg_;
    Color saved_bg_;
    bool  bg_set_;

public:
    explicit styled(color fg) noexcept
        : saved_fg_(::get_color()), saved_bg_(0), bg_set_(false)
    {
        ::set_color(fg.raw());
    }
    styled(color fg, color bg) noexcept
        : saved_fg_(::get_color()), saved_bg_(::get_color_bg()), bg_set_(true)
    {
        ::set_color(fg.raw());
        ::set_color_bg(bg.raw());
    }
    ~styled()
    {
        ::set_color(saved_fg_);
        if (bg_set_) ::set_color_bg(saved_bg_);
    }
    styled(const styled &)            = delete;
    styled &operator=(const styled &) = delete;
};

// Read / set the process-local text color directly (styled is the scoped form).
inline color current_color() noexcept { return color::from_raw(::get_color()); }
inline color current_background() noexcept { return color::from_raw(::get_color_bg()); }
inline void  set_color(color fg) noexcept { ::set_color(fg.raw()); }
inline void  set_background(color bg) noexcept { ::set_color_bg(bg.raw()); }

// ── box::vga — VGA text-mode ops + the batch session ────────────────────────
namespace vga {

using dimensions = vga_dimensions_t;
using position   = vga_pos_t;

// RAII batch: ops between construction and commit() / destruction are collected
// into one Manifest and submitted as a single syscall. Nests correctly (only
// the outermost commit flushes). Getter ops resolve immediately, bypassing it.
class session {
    bool open_{true};

public:
    session() noexcept { vga_begin(); }
    ~session() { if (open_) vga_commit(); }
    // Flush early; further ops in this scope would start a fresh (un-batched) op.
    int commit() noexcept { if (!open_) return 0; open_ = false; return vga_commit(); }
    session(const session &)            = delete;
    session &operator=(const session &) = delete;
};

// Write (quantized to the 16-color VGA palette at the boxlib boundary).
inline bool put(char c) noexcept { return vga_putchar(c) == 0; }
inline bool put(const char *s) noexcept { return vga_puts(s) == 0; }
inline bool newline() noexcept { return vga_newline() == 0; }

// Color: box::color → VGA attribute (fg+bg packed) for the current run.
inline bool set_color(color fg, color bg = colors::inherit) noexcept
{
    return vga_setcolor(color_to_vga_attr(fg.raw(), bg.raw())) == 0;
}
inline std::uint8_t color_attr() noexcept
{
    int a = vga_getcolor();
    return a < 0 ? 0u : static_cast<std::uint8_t>(a);
}

inline bool clear(color bg = colors::black) noexcept
{
    return vga_clear(color_to_vga4(bg.raw())) == 0;
}
inline bool clear_to_eol(color bg = colors::black) noexcept
{
    return vga_clear_to_eol(color_to_vga4(bg.raw())) == 0;
}
inline bool clear_line(std::uint8_t row, color bg = colors::black) noexcept
{
    return vga_clear_line(row, color_to_vga4(bg.raw())) == 0;
}
inline bool scroll_up(std::uint8_t lines, color fill = colors::black) noexcept
{
    return vga_scroll_up(lines, color_to_vga4(fill.raw())) == 0;
}
inline bool move_cursor(std::uint8_t row, std::uint8_t col) noexcept
{
    return vga_setcursor(row, col) == 0;
}
inline position cursor() noexcept
{
    position p{};
    vga_getcursor(&p);
    return p;
}
inline dimensions size() noexcept
{
    dimensions d{};
    vga_getdimensions(&d);
    return d;
}

}  // namespace vga

}  // namespace box

// ── std::formatter<box::color> — the color VALUE as "#RRGGBB" (not a control
//    sequence; sentinels render as "default" / "inherit") ────────────────────
// Inherits formatter<string_view> for the spec, so a palette can be printed in
// columns; the three renderings are all seven characters wide either way.
template <> struct std::formatter<box::color> : std::formatter<std::string_view, char> {
    auto format(const box::color &c, std::format_context &ctx) const
    {
        static constexpr char kFmt[] = "#{:02X}{:02X}{:02X}";
        char             buf[sizeof kFmt + 3 * 2];   // three two-digit fields
        std::string_view sv;
        if (c.is_default())      sv = "default";
        else if (c.is_inherit()) sv = "inherit";
        else {
            auto r = std::format_to_n(buf, (std::ptrdiff_t)sizeof buf, kFmt,
                                      static_cast<unsigned>(c.r()),
                                      static_cast<unsigned>(c.g()),
                                      static_cast<unsigned>(c.b()));
            sv = std::string_view(buf, (std::size_t)(r.out - buf));
        }
        return std::formatter<std::string_view, char>::format(sv, ctx);
    }
};

#endif  // BOXCXX_BOX_CONSOLE_H
