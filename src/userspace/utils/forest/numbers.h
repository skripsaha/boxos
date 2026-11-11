/* numbers.h — the forest's dice, its waves and its pixel arithmetic: what every
 * other part counts with, and nothing of BoxOS. */
#ifndef FOREST_NUMBERS_H
#define FOREST_NUMBERS_H

#include <algorithm>
#include <cstdint>

#include "painter/core/color.h"

namespace forest {

using Color = painter::core::Color;

// xorshift, seeded once, so a run is its own.
class Dice {
public:
    explicit Dice(std::uint64_t Seed) : State(Seed ? Seed : 0x9E3779B97F4A7C15ull) {}

    std::uint64_t Next()
    {
        State ^= State << 13;
        State ^= State >> 7;
        State ^= State << 17;
        return State;
    }
    float Unit() { return static_cast<float>(Next() >> 40) / 16777216.0f; }
    float Between(float Low, float High) { return Low + (High - Low) * Unit(); }
    int   Count(int Low, int High) { return Low + static_cast<int>(Next() % static_cast<std::uint64_t>(High - Low + 1)); }
    bool  Chance(float Probability) { return Unit() < Probability; }

private:
    std::uint64_t State;
};

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

// A wave, -1..1, of `Turns` whole turns: a sine read from a table made once.
inline constexpr int WaveSteps = 1024;
extern float         WaveTable[WaveSteps];
void                 MakeWaves();

inline float Wave(float Turns)
{
    auto Step = static_cast<std::int64_t>(Turns * WaveSteps);
    return WaveTable[static_cast<std::size_t>(Step & (WaveSteps - 1))];
}

inline float Ease(float From, float To, float Rate, float Seconds)
{
    float Step = std::min(1.0f, Rate * Seconds);
    return From + (To - From) * Step;
}

inline float Smooth(float Low, float High, float Value)
{
    float T = std::clamp((Value - Low) / (High - Low), 0.0f, 1.0f);
    return T * T * (3.0f - 2.0f * T);
}

// Weight 0..256 of To in the mix.
inline Color Mixed(Color From, Color To, int Weight)
{
    Weight   = std::clamp(Weight, 0, 256);
    int Back = 256 - Weight;
    return Color::Rgb(static_cast<std::uint8_t>((From.Red() * Back + To.Red() * Weight) >> 8),
                      static_cast<std::uint8_t>((From.Green() * Back + To.Green() * Weight) >> 8),
                      static_cast<std::uint8_t>((From.Blue() * Back + To.Blue() * Weight) >> 8));
}

// What light does to a colour, a 256th for each of its channels.
struct Tint {
    int Red   = 256;
    int Green = 256;
    int Blue  = 256;
};

inline Color Tinted(Color Ink, const Tint &Light)
{
    return Color::Rgb(static_cast<std::uint8_t>(std::min(255, (Ink.Red() * Light.Red) >> 8)),
                      static_cast<std::uint8_t>(std::min(255, (Ink.Green() * Light.Green) >> 8)),
                      static_cast<std::uint8_t>(std::min(255, (Ink.Blue() * Light.Blue) >> 8)));
}

inline Color Darker(Color Ink, int Amount) { return Mixed(Ink, painter::core::Colors::Black, Amount); }
inline Color Lighter(Color Ink, int Amount) { return Mixed(Ink, painter::core::Colors::White, Amount); }

}  // namespace forest

#endif
