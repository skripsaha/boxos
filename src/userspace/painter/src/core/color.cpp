/*
 * color.cpp — a colour laid over another, and walked toward another, the way
 * every colour of the system is (boxos_color.h). See painter/core/color.h.
 */

#include "painter/core/color.h"

namespace painter::core {

Color Color::Over(Color Below, Color Above, std::uint8_t Weight) noexcept
{
    if (Below.IsScreen() || Above.IsScreen()) return Weight < 128 ? Below : Above;
    const std::uint32_t Share = (static_cast<std::uint32_t>(Above.Alpha) * Weight + 127u) / 255u;
    std::uint32_t Opacity = Below.Alpha;
    const std::uint32_t Seen = BoxColorOverPart(Below.Value, Above.Value, Share, &Opacity);
    return Color(Seen, static_cast<std::uint8_t>(Opacity), false);
}

Color Color::Between(Color A, Color B, std::uint8_t Weight) noexcept
{
    if (A.IsScreen() || B.IsScreen()) return Weight < 128 ? A : B;
    const std::uint32_t Way = (static_cast<std::uint32_t>(Weight) * BOX_COLOR_WAY + 127u) / 255u;
    const std::uint32_t Walked = BoxColorBetween(A.Value | static_cast<std::uint32_t>(A.Alpha) << 24,
                                                 B.Value | static_cast<std::uint32_t>(B.Alpha) << 24, Way);
    return Color(Walked & 0x00FFFFFFu, static_cast<std::uint8_t>(Walked >> 24), false);
}

}  // namespace painter::core
