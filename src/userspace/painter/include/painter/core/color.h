#ifndef PAINTER_CORE_COLOR_H
#define PAINTER_CORE_COLOR_H

#include <cstdint>

#include "box/color.h"

namespace painter::core {

// A colour: #RRGGBB and how opaque it is, of 255 — or the screen's own
// colour, which the display draws. Laid over another, a colour is mixed as
// light; walked toward another, straight in its numbers — the two ways every
// colour of the system is mixed (boxos_color.h), so Painter's colours are the
// pixels the display and the loader would make of them.
class Color {
public:
    constexpr Color() noexcept = default;

    static constexpr Color Rgb(std::uint8_t Red, std::uint8_t Green, std::uint8_t Blue) noexcept
    {
        return Rgba(Red, Green, Blue, 255);
    }
    // #RRGGBBAA: a colour at Alpha of 255 over whatever lies beneath it.
    static constexpr Color Rgba(std::uint8_t Red, std::uint8_t Green, std::uint8_t Blue, std::uint8_t Alpha) noexcept
    {
        return Color((static_cast<std::uint32_t>(Red) << 16) | (static_cast<std::uint32_t>(Green) << 8) |
                         static_cast<std::uint32_t>(Blue),
                     Alpha, false);
    }
    // #RRGGBB, whole; a sentinel's byte is not part of it.
    static constexpr Color FromRaw(std::uint32_t Raw) noexcept { return Color(Raw & 0x00FFFFFFu, 255, false); }

    // The screen's own colour: as a background the page colour the display
    // draws, as ink its text colour. It follows what the person sets for the
    // screen, and a Color of any other kind never equals it.
    static constexpr Color Screen() noexcept { return Color(0, 255, true); }
    constexpr bool         IsScreen() const noexcept { return ScreenOwn; }

    // See-through: nothing of its own, whatever lies beneath shows. It is for
    // a picture that leaves part of a cell unpainted, so the unpainted half
    // of an edge cell is neither black nor any colour of the program's.
    static constexpr Color Through() noexcept { return Color(0, 0, false); }
    constexpr bool         IsThrough() const noexcept { return !ScreenOwn && Alpha == 0; }

    // This colour at Opacity of 255. The screen's own colour and see-through
    // have no triple to take an opacity, and are returned as they are.
    constexpr Color WithOpacity(std::uint8_t Opacity) const noexcept
    {
        return ScreenOwn || IsThrough() ? *this : Color(Value, Opacity, false);
    }
    // How opaque the colour is, of 255; the screen's own is whole.
    constexpr std::uint8_t Opacity() const noexcept { return Alpha; }
    constexpr bool         IsTranslucent() const noexcept { return !ScreenOwn && Alpha != 0 && Alpha != 255; }

    // The colour as a cell carries it to the display (boxos_color.h): the
    // screen's own as COLOR_DEFAULT, see-through as COLOR_THROUGH, a whole
    // colour as its triple, and one between in the cell's steps of opacity.
    std::uint32_t Raw() const noexcept
    {
        if (ScreenOwn) return COLOR_DEFAULT;
        return BoxColorTranslucent(Value, (static_cast<std::uint32_t>(Alpha) * COLOR_OPACITY_STEPS + 127u) / 255u);
    }
    constexpr std::uint8_t Red() const noexcept { return static_cast<std::uint8_t>((Value >> 16) & 0xFFu); }
    constexpr std::uint8_t Green() const noexcept { return static_cast<std::uint8_t>((Value >> 8) & 0xFFu); }
    constexpr std::uint8_t Blue() const noexcept { return static_cast<std::uint8_t>(Value & 0xFFu); }

    // Above laid over Below at Weight of 255 of its own opacity, mixed as
    // light — Porter-Duff "over" (BoxColorOverPart): what a pixel shows where
    // a shape covers that share of it, or a colour at that opacity lies over
    // another. Over a part of the picture that is not whole, the result's
    // opacity is the two composed, so Above over Through is Above at that
    // share. The screen's own colour has no triple to mix: the result is
    // whichever end is nearer.
    static Color Over(Color Below, Color Above, std::uint8_t Weight) noexcept;
    // Weight of 255 of the way from A to B, each number and the opacity walked
    // straight (BoxColorBetween): a gradient's step, a palette's shade, one
    // colour turning into another. The screen's own colour has no triple to
    // walk: the result is whichever end is nearer.
    static Color Between(Color A, Color B, std::uint8_t Weight) noexcept;

    friend constexpr bool operator==(Color, Color) noexcept = default;

private:
    constexpr Color(std::uint32_t Rgb, std::uint8_t Opacity, bool Screen) noexcept
        : Value(Opacity == 0 && !Screen ? 0 : Rgb), Alpha(Opacity), ScreenOwn(Screen)
    {
    }
    std::uint32_t Value = 0;       // #RRGGBB; 0 when see-through or the screen's own
    std::uint8_t  Alpha = 255;
    bool          ScreenOwn = false;
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
