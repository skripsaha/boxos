/*
 * color.cpp — a colour laid over another, and walked toward another, the way
 * every colour of the system is (boxos_color.h). See painter/core/color.h.
 */

#include "painter/core/color.h"

#include <algorithm>
#include <cmath>

namespace painter::core {

namespace {

/* sRGB's number as light, and light as its number. */
float Light(float Number) noexcept
{
    return Number <= 0.04045f ? Number / 12.92f : std::pow((Number + 0.055f) / 1.055f, 2.4f);
}

float Number(float Light) noexcept
{
    Light = std::clamp(Light, 0.0f, 1.0f);
    return Light <= 0.0031308f ? Light * 12.92f : 1.055f * std::pow(Light, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

Oklab OklabOf(float Red, float Green, float Blue) noexcept
{
    const float R = Light(Red), G = Light(Green), B = Light(Blue);
    const float L = std::cbrt(0.4122214708f * R + 0.5363325363f * G + 0.0514459929f * B);
    const float M = std::cbrt(0.2119034982f * R + 0.6806995451f * G + 0.1073969566f * B);
    const float S = std::cbrt(0.0883024619f * R + 0.2817188376f * G + 0.6299787005f * B);
    return Oklab{0.2104542553f * L + 0.7936177850f * M - 0.0040720468f * S,
                 1.9779984951f * L - 2.4285922050f * M + 0.4505937099f * S,
                 0.0259040371f * L + 0.7827717662f * M - 0.8086757660f * S};
}

void RgbOf(Oklab Of, float &Red, float &Green, float &Blue) noexcept
{
    const float L0 = Of.L + 0.3963377774f * Of.A + 0.2158037573f * Of.B;
    const float M0 = Of.L - 0.1055613458f * Of.A - 0.0638541728f * Of.B;
    const float S0 = Of.L - 0.0894841775f * Of.A - 1.2914855480f * Of.B;
    const float L = L0 * L0 * L0, M = M0 * M0 * M0, S = S0 * S0 * S0;
    Red   = Number(4.0767416621f * L - 3.3077115913f * M + 0.2309699292f * S);
    Green = Number(-1.2684380046f * L + 2.6097574011f * M - 0.3413193965f * S);
    Blue  = Number(-0.0041960863f * L - 0.7034186147f * M + 1.7076147010f * S);
}

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

Color Color::Transition(Color From, Color To, float Share) noexcept
{
    if (From.IsScreen() || To.IsScreen()) return Share < 0.5f ? From : To;
    Share = std::clamp(Share, 0.0f, 1.0f);
    const float A0 = From.Alpha / 255.0f, A1 = To.Alpha / 255.0f;
    const float Alpha = A0 + (A1 - A0) * Share;
    if (Alpha <= 0) return Through();
    const Oklab P = OklabOf(From.Red() / 255.0f, From.Green() / 255.0f, From.Blue() / 255.0f);
    const Oklab Q = OklabOf(To.Red() / 255.0f, To.Green() / 255.0f, To.Blue() / 255.0f);
    const float W0 = A0 * (1 - Share) / Alpha, W1 = A1 * Share / Alpha;
    float R = 0, G = 0, B = 0;
    RgbOf(Oklab{P.L * W0 + Q.L * W1, P.A * W0 + Q.A * W1, P.B * W0 + Q.B * W1}, R, G, B);
    const auto Byte = [](float Of) { return static_cast<std::uint8_t>(std::lround(std::clamp(Of, 0.0f, 1.0f) * 255.0f)); };
    return Rgba(Byte(R), Byte(G), Byte(B), Byte(Alpha));
}

}  // namespace painter::core
