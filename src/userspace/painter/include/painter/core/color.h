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
    static constexpr Color Screen() noexcept { return Color(COLOR_DEFAULT, WireTag{}); }
    constexpr bool         IsScreen() const noexcept { return Value == COLOR_DEFAULT; }

    // See-through: whatever lies beneath the cell, in this place. It is for a
    // surface that leaves part of a cell unpainted — the display draws it as
    // the background of the cell below (COLOR_THROUGH), so the unpainted half
    // of an edge cell is neither black nor any colour of the program's.
    static constexpr Color Through() noexcept { return Color(COLOR_THROUGH, WireTag{}); }
    constexpr bool         IsThrough() const noexcept { return Value == COLOR_THROUGH; }

    // This colour at Opacity of 255 over what is beneath, as the display draws
    // it (COLOR_OPACITY_STEPS of them on the wire, boxos_color.h): none is
    // Through, 255 the plain colour. The screen's own colour and see-through
    // have no triple to take an opacity, and are returned as they are.
    Color WithOpacity(std::uint8_t Opacity) const noexcept
    {
        if (IsScreen() || IsThrough()) return *this;
        return Color(BoxColorTranslucent(Value, (static_cast<std::uint32_t>(Opacity) * COLOR_OPACITY_STEPS + 127u) / 255u), WireTag{});
    }

    // How opaque the colour is, of 255: every colour but a translucent one and
    // Through is whole.
    std::uint8_t Opacity() const noexcept
    {
        return static_cast<std::uint8_t>((BoxColorOpacity(Value) * 255u + COLOR_OPACITY_STEPS / 2u) / COLOR_OPACITY_STEPS);
    }
    bool IsTranslucent() const noexcept { return BoxColorIsTranslucent(Value) != 0; }

    constexpr std::uint32_t Raw() const noexcept { return Value; }
    constexpr std::uint8_t  Red() const noexcept { return static_cast<std::uint8_t>((Value >> 16) & 0xFFu); }
    constexpr std::uint8_t  Green() const noexcept { return static_cast<std::uint8_t>((Value >> 8) & 0xFFu); }
    constexpr std::uint8_t  Blue() const noexcept { return static_cast<std::uint8_t>(Value & 0xFFu); }

    // B laid over A at Weight of 255 of its own opacity: Porter-Duff "over".
    // Two whole colours are boxlib's one blend, A toward B. Over a part of the
    // picture that is not whole B's share is its opacity times Weight, the
    // result's opacity the two composed, and its colour the two weighted by
    // what each contributes — so B over Through is B at that share, and over a
    // translucent colour the pair as the display would show them stacked. The
    // screen's own colour has no triple to blend: the result is whichever end
    // is nearer.
    static Color Mix(Color A, Color B, std::uint8_t Weight) noexcept
    {
        if (A.IsScreen() || B.IsScreen()) return Weight < 128 ? A : B;
        const std::uint32_t Below = A.Opacity();
        if (Below == 255u && B.Opacity() == 255u) return FromRaw(color_mix(A.Raw(), B.Raw(), Weight));
        const std::uint32_t Laid = (static_cast<std::uint32_t>(B.Opacity()) * Weight + 127u) / 255u;
        if (Laid == 0u) return A;
        const std::uint32_t Kept = Below * (255u - Laid) / 255u;
        const std::uint32_t Total = Laid + Kept;
        const auto Channel = [&](std::uint8_t Over, std::uint8_t Under) {
            return static_cast<std::uint8_t>((Over * Laid + Under * Kept + Total / 2u) / Total);
        };
        return Rgb(Channel(B.Red(), A.Red()), Channel(B.Green(), A.Green()), Channel(B.Blue(), A.Blue())).WithOpacity(static_cast<std::uint8_t>(Total));
    }

    friend constexpr bool operator==(Color, Color) noexcept = default;

private:
    struct WireTag {};

    explicit constexpr Color(std::uint32_t Raw) noexcept : Value(Raw & 0x00FFFFFFu) {}
    constexpr Color(std::uint32_t Wire, WireTag) noexcept : Value(Wire) {}

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
