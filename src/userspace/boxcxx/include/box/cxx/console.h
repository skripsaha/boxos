#ifndef BOXCXX_BOX_CONSOLE_H
#define BOXCXX_BOX_CONSOLE_H

#include <cstdint>
#include <format>

#include "box/color.h"
#include "box/vga.h"

namespace box {

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
    constexpr bool         is_rgb() const noexcept { return !is_default() && !is_inherit(); }

    friend constexpr bool operator==(color a, color b) noexcept { return a.v_ == b.v_; }
};

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
}

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

inline color current_color() noexcept { return color::from_raw(::get_color()); }
inline color current_background() noexcept { return color::from_raw(::get_color_bg()); }
inline void  set_color(color fg) noexcept { ::set_color(fg.raw()); }
inline void  set_background(color bg) noexcept { ::set_color_bg(bg.raw()); }

namespace vga {

using dimensions = vga_dimensions_t;
using position   = vga_pos_t;

class session {
    bool open_{true};

public:
    session() noexcept { vga_begin(); }
    ~session() { if (open_) vga_commit(); }
    int commit() noexcept { if (!open_) return 0; open_ = false; return vga_commit(); }
    session(const session &)            = delete;
    session &operator=(const session &) = delete;
};

inline bool put(char c) noexcept { return vga_putchar(c) == 0; }
inline bool put(const char *s) noexcept { return vga_puts(s) >= 0; }
inline bool newline() noexcept { return vga_newline() == 0; }

inline bool set_color(color fg, color bg = colors::inherit) noexcept
{
    return vga_setcolor_rgb(fg.raw(), bg.raw()) == 0;
}
inline bool get_color(color &fg, color &bg) noexcept
{
    Color f = 0, b = 0;
    if (vga_getcolor_rgb(&f, &b) != 0) return false;
    fg = color::from_raw(f);
    bg = color::from_raw(b);
    return true;
}

inline bool clear(color bg = colors::black) noexcept
{
    return vga_clear_rgb(colors::light_gray.raw(), bg.raw()) == 0;
}
inline bool clear_to_eol() noexcept
{
    return vga_clear_to_eol() == 0;
}
inline bool clear_line(std::uint8_t row, color bg = colors::black) noexcept
{
    return vga_clear_line_rgb(row, colors::light_gray.raw(), bg.raw()) == 0;
}
inline bool scroll_up() noexcept
{
    return vga_scroll_up() == 0;
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

}

}

template <> struct std::formatter<box::color> : std::formatter<std::string_view, char> {
    auto format(const box::color &c, std::format_context &ctx) const
    {
        static constexpr char kFmt[] = "#{:02X}{:02X}{:02X}";
        char             buf[sizeof kFmt + 3 * 2];
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

#endif