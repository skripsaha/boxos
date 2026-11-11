#ifndef BOXOS_DISPLAY_EFFECTS_MATH_H
#define BOXOS_DISPLAY_EFFECTS_MATH_H

#include <cstdint>

// What the effects' parts count with, and the frame they are all drawn at:
// nothing of the display in it.

namespace effects {

constexpr std::uint32_t Whole = 256;    // a level, or a mix, at its fullest

// The effect as one frame shows it: what was asked, how far it has swelled in,
// and the time of the frame — the same for every cell drawn in it.
struct Moment {
    std::uint32_t Kinds = 0;
    std::uint32_t Strength = 0;
    std::uint32_t Color = 0;
    std::uint8_t  Axes = 0;
    std::uint32_t Level = 0;        // 0..Whole
    std::uint64_t PhaseMs = 0;
    std::uint32_t Number = 0;       // the frame's own, counted from the effect's start
    std::uint32_t Cols = 0;
    std::uint32_t Rows = 0;

    // How far the effect's colours and shapes go: level and strength together, 0..Whole.
    std::uint32_t Mix() const noexcept { return Level * Strength / 255u; }
};

// A sine of a turn counted in 1024ths, by the parabola that fits it: -1024..1024.
inline std::int32_t Sine(std::uint32_t Turn) noexcept
{
    std::int32_t Place = static_cast<std::int32_t>(Turn & 1023u);
    const bool Below = Place >= 512;
    if (Below) Place -= 512;
    const std::int32_t Height = Place * (512 - Place) / 64;
    return Below ? -Height : Height;
}

inline std::uint32_t Hash(std::uint32_t Value) noexcept
{
    Value ^= Value >> 16;
    Value *= 0x7FEB352Du;
    Value ^= Value >> 15;
    Value *= 0x846CA68Bu;
    return Value ^ (Value >> 16);
}

inline std::int32_t Wrap(std::int32_t Place, std::int32_t Size) noexcept
{
    if (Place >= 0 && Place < Size) return Place;
    const std::int32_t Left = Place % Size;
    return Left < 0 ? Left + Size : Left;
}

inline std::int32_t Clamp(std::int32_t Place, std::int32_t Size) noexcept
{
    return Place < 0 ? 0 : (Place >= Size ? Size - 1 : Place);
}

// The square root of Value, rounded down.
inline std::uint32_t Root(std::uint32_t Value) noexcept
{
    float Result;
    asm("sqrtss %1, %0" : "=x"(Result) : "x"(static_cast<float>(Value)));
    return static_cast<std::uint32_t>(Result);
}

inline std::uint32_t Channel(std::uint32_t Colour, std::uint32_t Shift) noexcept
{
    return (Colour >> Shift) & 0xFFu;
}

inline std::uint32_t Pack(std::uint32_t Red, std::uint32_t Green, std::uint32_t Blue) noexcept
{
    return (Red << 16) | (Green << 8) | Blue;
}

// One channel `Mix` of the way (0..Whole) from `From` to `To`.
inline std::uint32_t Between(std::uint32_t From, std::uint32_t To, std::uint32_t Mix) noexcept
{
    const std::int32_t Step = ((static_cast<std::int32_t>(To) - static_cast<std::int32_t>(From)) *
                               static_cast<std::int32_t>(Mix)) >> 8;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(From) + Step);
}

inline std::uint32_t Brightened(std::uint32_t Colour, std::uint32_t Mix) noexcept
{
    return Pack(Between(Channel(Colour, 16), 255, Mix), Between(Channel(Colour, 8), 255, Mix),
                Between(Channel(Colour, 0), 255, Mix));
}

}  // namespace effects

#endif
