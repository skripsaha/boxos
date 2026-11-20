#ifndef PAINTER_CORE_LIGHT_H
#define PAINTER_CORE_LIGHT_H

/* light.h — the sixteen-bit numbers of the engine: light on sRGB's own curve
 * (boxos_color.h), shares of 65535, and the numbers written to the glass. One
 * text for every way rows are worked (lanes.h): each gives these very
 * numbers. */

#include <cstdint>

#include "box/color.h"
#include "painter/core/pixel.h"

namespace painter::core::light {

constexpr std::uint32_t Whole = 65535u;

/* A times B over 65535, to the nearest whole — exact for any two shares:
 * the rounding term carried once more past the shift. */
constexpr std::uint32_t Times(std::uint32_t A, std::uint32_t B) noexcept
{
    const std::uint32_t X = A * B + 32768u;
    return (X + (X >> 16)) >> 16;
}

/* A share of 255 as a share of 65535: exactly, 255 · 257 being 65535. */
constexpr std::uint32_t Widened(std::uint32_t Share) noexcept { return Share * 257u; }

/* The light of an sRGB number given in 1/256 of a number, 0 to 255·256: the
 * curve between the two whole numbers either side, straight — exact at each
 * whole number, and within a hair of the curve between. */
inline std::uint32_t OfFine(std::uint32_t Fine) noexcept
{
    const std::uint32_t Code = Fine >> 8, Part = Fine & 255u;
    if (Code >= 255u) return BoxColorLight(255u);
    const std::uint32_t Low = BoxColorLight(Code), High = BoxColorLight(Code + 1u);
    return Low + (((High - Low) * Part + 128u) >> 8);
}

/* A colour, 0xAARRGGBB, as a pixel: its numbers' light times its opacity. */
inline Pixel OfArgb(std::uint32_t Argb) noexcept
{
    const std::uint32_t Alpha = Widened(Argb >> 24);
    return Pixel{static_cast<std::uint16_t>(Times(BoxColorLight(Argb >> 16), Alpha)),
                 static_cast<std::uint16_t>(Times(BoxColorLight(Argb >> 8), Alpha)),
                 static_cast<std::uint16_t>(Times(BoxColorLight(Argb), Alpha)), static_cast<std::uint16_t>(Alpha)};
}

/* A pixel at Share of 65535 of itself. */
inline Pixel Scaled(Pixel Of, std::uint32_t Share) noexcept
{
    return Pixel{static_cast<std::uint16_t>(Times(Of.Red, Share)), static_cast<std::uint16_t>(Times(Of.Green, Share)),
                 static_cast<std::uint16_t>(Times(Of.Blue, Share)), static_cast<std::uint16_t>(Times(Of.Alpha, Share))};
}

/* Above at Share laid over Below: Above's light added to what it leaves of
 * Below's — the one way a colour is laid, premultiplied. */
inline Pixel Over(Pixel Below, Pixel Above, std::uint32_t Share) noexcept
{
    const Pixel         Laid = Scaled(Above, Share);
    const std::uint32_t Left = Whole - Laid.Alpha;
    return Pixel{static_cast<std::uint16_t>(Laid.Red + Times(Below.Red, Left)),
                 static_cast<std::uint16_t>(Laid.Green + Times(Below.Green, Left)),
                 static_cast<std::uint16_t>(Laid.Blue + Times(Below.Blue, Left)),
                 static_cast<std::uint16_t>(Laid.Alpha + Times(Below.Alpha, Left))};
}

/* The numbers a light lies between, for writing it to the glass: Floor[L >> 4]
 * the number at or below the start of L's step of sixteen — a step holds at
 * most one number's start, the closest two lying twenty apart — and
 * Reciprocal[n] 2^24 over the light from n to n + 1, rounded up, so the part
 * of the way L lies from n to n + 1, of 256, is
 * ((L − Light(n)) · Reciprocal[n]) >> 16. */
struct Steps {
    std::uint8_t  Floor[4096];
    std::uint32_t Reciprocal[256];
};
const Steps &StepsOfLight() noexcept;

/* Where a light lies between the numbers it is written as: N the number at
 * or below it, Part the part of the way to the next, of 256 — none at 255,
 * past which there is no number. */
inline void Between(std::uint32_t Light, const Steps &S, std::uint32_t &N, std::uint32_t &Part) noexcept
{
    N = S.Floor[Light >> 4];
    if (N < 255u && Light >= BoxColorLight(N + 1u)) N++;
    Part = N == 255u ? 0u : ((Light - BoxColorLight(N)) * S.Reciprocal[N]) >> 16;
}

/* The number a light is written as, dithered: the number below it, or the
 * next when the part of the way to it lies past Threshold (0 to 255) — so
 * over many pixels the numbers average the light. Undithered, the nearest is
 * BoxColorCode's. */
inline std::uint32_t Dithered(std::uint32_t Light, const Steps &S, std::uint32_t Threshold) noexcept
{
    std::uint32_t N, Part;
    Between(Light, S, N, Part);
    return N + (Part > Threshold ? 1u : 0u);
}

/* Every light's place between the numbers, worked out once to be looked up
 * (Between): Part << 8 | N — and one entry past the last light, so eight
 * entries can be gathered as 32-bit words. Made on the heap the first time
 * it is asked, so only a program that dithers carries it; none when there
 * was no room for it, and the numbers are then worked out as Dithered does. */
struct DitherTable {
    std::uint16_t Of[65537];
};
const DitherTable *DitherTableOfLight() noexcept;

/* The number Dithered gives a light, from its entry in the table. */
inline std::uint32_t DitheredBy(std::uint32_t Entry, std::uint32_t Threshold) noexcept
{
    return (Entry & 255u) + ((Entry >> 8) > Threshold ? 1u : 0u);
}

}  // namespace painter::core::light

#endif  // PAINTER_CORE_LIGHT_H
