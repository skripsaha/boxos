/* numbers.h — what the pictures of hangman count with: the same number for the
 * same place, waves, and the arithmetic of colours. Nothing of BoxOS in it. */
#ifndef HANGMAN_NUMBERS_H
#define HANGMAN_NUMBERS_H

#include <algorithm>
#include <cstdint>

#include "painter/core/color.h"

namespace hangman {

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

inline float Clamped(float Value, float Low = 0.0f, float High = 1.0f)
{
    return std::clamp(Value, Low, High);
}

inline float Smooth(float Low, float High, float Value)
{
    const float T = Clamped((Value - Low) / (High - Low));
    return T * T * (3.0f - 2.0f * T);
}

// Weight 0..1 of To in the mix.
inline Color Mixed(Color From, Color To, float Weight)
{
    return Color::Mix(From, To, static_cast<std::uint8_t>(Clamped(Weight) * 255.0f + 0.5f));
}

inline Color Darker(Color Ink, float Amount) { return Mixed(Ink, painter::core::Colors::Black, Amount); }
inline Color Lighter(Color Ink, float Amount) { return Mixed(Ink, painter::core::Colors::White, Amount); }

constexpr Color Rgb(std::uint8_t Red, std::uint8_t Green, std::uint8_t Blue) noexcept
{
    return Color::Rgb(Red, Green, Blue);
}

}  // namespace hangman

#endif
