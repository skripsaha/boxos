/* numbers.h — what the wizard's pictures count with: the same number for the
 * same place, waves, and the arithmetic of colours. Nothing of BoxOS in it. */
#ifndef WIZARD_NUMBERS_H
#define WIZARD_NUMBERS_H

#include <algorithm>
#include <cstdint>

#include "painter/core/color.h"

namespace wizard {

using Color = painter::core::Color;

// The same number for the same place, always: grain that does not crawl.
inline std::uint32_t Hash(int X, int Y, std::uint32_t Salt = 0)
{
    std::uint32_t Value = static_cast<std::uint32_t>(X) * 73856093u ^ static_cast<std::uint32_t>(Y) * 19349663u ^
                          Salt * 83492791u;
    Value ^= Value >> 13;
    Value *= 0x5BD1E995u;
    return Value ^ (Value >> 15);
}

// 0..1 from a hash.
inline float Grain(int X, int Y, std::uint32_t Salt = 0)
{
    return static_cast<float>(Hash(X, Y, Salt) & 0xFFFFu) / 65536.0f;
}

// A sine of `Turns` whole turns, -1..1, by two parabolas.
inline float Wave(float Turns)
{
    Turns -= static_cast<float>(static_cast<std::int32_t>(Turns));
    if (Turns < 0.0f) Turns += 1.0f;
    const bool  Below = Turns >= 0.5f;
    const float Place = Below ? Turns - 0.5f : Turns;
    const float Raw   = 16.0f * Place * (0.5f - Place);
    const float Sine  = Raw + 0.225f * (Raw * Raw - Raw);
    return Below ? -Sine : Sine;
}

// 0 to 1 and back to 0 again, once in `PeriodMs`.
inline float Beat(std::uint64_t Ms, std::uint32_t PeriodMs, float Phase = 0.0f)
{
    return 0.5f + 0.5f * Wave(static_cast<float>(Ms % PeriodMs) / static_cast<float>(PeriodMs) + Phase);
}

inline float Clamped(float Value, float Low = 0.0f, float High = 1.0f)
{
    return std::clamp(Value, Low, High);
}

inline float Smooth(float Low, float High, float Value)
{
    const float T = Clamped((Value - Low) / (High - Low));
    return T * T * (3.0f - 2.0f * T);
}

// Weight 0..256 of the way from From to To (Color::Between).
inline Color Mixed(Color From, Color To, int Weight)
{
    Weight = std::clamp(Weight, 0, 256);
    return Color::Between(From, To, static_cast<std::uint8_t>((Weight * 255 + 128) / 256));
}

inline Color Mixed(Color From, Color To, float Weight)
{
    return Mixed(From, To, static_cast<int>(Weight * 256.0f));
}

inline Color Darker(Color Ink, int Amount) { return Mixed(Ink, painter::core::Colors::Black, Amount); }
inline Color Lighter(Color Ink, int Amount) { return Mixed(Ink, painter::core::Colors::White, Amount); }

// A colour brightened or dimmed by a 256th for each, and the same for each channel.
inline Color Scaled(Color Ink, int Red, int Green, int Blue)
{
    return Color::Rgb(static_cast<std::uint8_t>(std::min(255, (Ink.Red() * Red) >> 8)),
                      static_cast<std::uint8_t>(std::min(255, (Ink.Green() * Green) >> 8)),
                      static_cast<std::uint8_t>(std::min(255, (Ink.Blue() * Blue) >> 8)));
}

// A colour with its light changed by a fraction, 1.0 as it is.
inline Color Toned(Color Base, float Level)
{
    const int Amount = static_cast<int>(Level * 256.0f);
    return Scaled(Base, Amount, Amount, Amount);
}

// Light added to a colour, each channel as much as is given.
inline Color Added(Color Ink, int Red, int Green, int Blue)
{
    return Color::Rgb(static_cast<std::uint8_t>(std::clamp(Ink.Red() + Red, 0, 255)),
                      static_cast<std::uint8_t>(std::clamp(Ink.Green() + Green, 0, 255)),
                      static_cast<std::uint8_t>(std::clamp(Ink.Blue() + Blue, 0, 255)));
}

// A colour on a ramp of tones, dark to light, at `Place` from 0 to 1.
template <std::size_t Count>
inline Color OnRamp(const Color (&Tones)[Count], float Place)
{
    const float Along = Clamped(Place) * static_cast<float>(Count - 1);
    const auto  Low   = std::min(static_cast<std::size_t>(Along), Count - 2);
    return Mixed(Tones[Low], Tones[Low + 1], Along - static_cast<float>(Low));
}

constexpr Color Rgb(std::uint8_t Red, std::uint8_t Green, std::uint8_t Blue) noexcept
{
    return Color::Rgb(Red, Green, Blue);
}

}  // namespace wizard

#endif
