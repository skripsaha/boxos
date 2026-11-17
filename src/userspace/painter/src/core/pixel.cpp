/*
 * pixel.cpp — a colour as a canvas's pixel, and back. See painter/core/pixel.h.
 */

#include "painter/core/pixel.h"

#include "light.h"

namespace painter::core {

Pixel Pixel::Of(Color From) noexcept
{
    if (From.IsScreen() || From.IsThrough()) return Pixel{};
    return light::OfArgb(static_cast<std::uint32_t>(From.Opacity()) << 24 |
                         static_cast<std::uint32_t>(From.Red()) << 16 | static_cast<std::uint32_t>(From.Green()) << 8 |
                         From.Blue());
}

Color Pixel::ToColor() const noexcept
{
    if (Alpha == 0) return Color::Through();
    const auto Unlaid = [this](std::uint32_t Light) {
        return static_cast<std::uint8_t>(BoxColorCode((Light * light::Whole + Alpha / 2u) / Alpha));
    };
    return Color::Rgba(Unlaid(Red), Unlaid(Green), Unlaid(Blue),
                       static_cast<std::uint8_t>((static_cast<std::uint32_t>(Alpha) * 255u + light::Whole / 2u) /
                                                 light::Whole));
}

}  // namespace painter::core
