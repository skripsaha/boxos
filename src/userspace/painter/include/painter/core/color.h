#ifndef PAINTER_CORE_COLOR_H
#define PAINTER_CORE_COLOR_H

#include <cstdint>

#include "box/color.h"

namespace painter::core {

class Color {
public:
    constexpr Color() noexcept = default;

    static constexpr Color Rgb(std::uint8_t Red, std::uint8_t Green, std::uint8_t Blue) noexcept
    {
        return Color((static_cast<std::uint32_t>(Red) << 16) |
                     (static_cast<std::uint32_t>(Green) << 8) |
                     static_cast<std::uint32_t>(Blue));
    }

    static constexpr Color FromRaw(std::uint32_t Raw) noexcept { return Color(Raw & 0x00FFFFFFu); }

    // The screen's own colour: as a background the page colour the display
    // draws, as ink its text colour. It follows what the person sets for the
    // screen, and a Color of any other kind never equals it.
    static constexpr Color Screen() noexcept { return Color(COLOR_DEFAULT, ScreenTag{}); }
    constexpr bool         IsScreen() const noexcept { return Value == COLOR_DEFAULT; }

    constexpr std::uint32_t Raw() const noexcept { return Value; }
    constexpr std::uint8_t  Red() const noexcept { return static_cast<std::uint8_t>((Value >> 16) & 0xFFu); }
    constexpr std::uint8_t  Green() const noexcept { return static_cast<std::uint8_t>((Value >> 8) & 0xFFu); }
    constexpr std::uint8_t  Blue() const noexcept { return static_cast<std::uint8_t>(Value & 0xFFu); }

    // The colour from A toward B by Weight of 255 — boxlib's one blend. The
    // screen's own colour has no triple to blend: it is whichever end is nearer.
    static Color Mix(Color A, Color B, std::uint8_t Weight) noexcept
    {
        if (A.IsScreen() || B.IsScreen()) return Weight < 128 ? A : B;
        return FromRaw(color_mix(A.Raw(), B.Raw(), Weight));
    }

    friend constexpr bool operator==(Color, Color) noexcept = default;

private:
    struct ScreenTag {};

    explicit constexpr Color(std::uint32_t Raw) noexcept : Value(Raw & 0x00FFFFFFu) {}
    constexpr Color(std::uint32_t Sentinel, ScreenTag) noexcept : Value(Sentinel) {}

    std::uint32_t Value = 0;
};

// The console's named colours (box/color.h), named for Painter.
namespace Colors {
inline constexpr Color Black     = Color::FromRaw(COLOR_BLACK);
inline constexpr Color White     = Color::FromRaw(COLOR_WHITE);
inline constexpr Color DarkGray  = Color::FromRaw(COLOR_DARK_GRAY);
inline constexpr Color LightGray = Color::FromRaw(COLOR_LIGHT_GRAY);
inline constexpr Color Red       = Color::FromRaw(COLOR_RED);
inline constexpr Color Green     = Color::FromRaw(COLOR_GREEN);
inline constexpr Color Blue      = Color::FromRaw(COLOR_BLUE);
inline constexpr Color Yellow    = Color::FromRaw(COLOR_YELLOW);
inline constexpr Color Cyan      = Color::FromRaw(COLOR_CYAN);
inline constexpr Color Magenta   = Color::FromRaw(COLOR_MAGENTA);
inline constexpr Color DarkRed   = Color::FromRaw(COLOR_DARK_RED);
inline constexpr Color DarkGreen = Color::FromRaw(COLOR_DARK_GREEN);
inline constexpr Color DarkBlue  = Color::FromRaw(COLOR_DARK_BLUE);
inline constexpr Color Brown     = Color::FromRaw(COLOR_BROWN);
inline constexpr Color DarkCyan  = Color::FromRaw(COLOR_DARK_CYAN);
inline constexpr Color DarkMagenta = Color::FromRaw(COLOR_DARK_MAGENTA);
inline constexpr Color Berry     = Color::FromRaw(COLOR_BERRY);
inline constexpr Color Ocean     = Color::FromRaw(COLOR_OCEAN);
inline constexpr Color Leaf      = Color::FromRaw(COLOR_LEAF);
inline constexpr Color Amber     = Color::FromRaw(COLOR_AMBER);
inline constexpr Color Violet    = Color::FromRaw(COLOR_VIOLET);
inline constexpr Color Teal      = Color::FromRaw(COLOR_TEAL);
inline constexpr Color Coral     = Color::FromRaw(COLOR_CORAL);
inline constexpr Color Slate     = Color::FromRaw(COLOR_SLATE);
}  // namespace Colors

}  // namespace painter::core

#endif  // PAINTER_CORE_COLOR_H
