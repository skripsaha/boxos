/*
 * lanes_plain.cpp — the engine's rows worked one pixel at a time: the very
 * numbers of light.h, the measure every other way is held to. See lanes.h.
 */

#include "lanes.h"

#include "boxos_pixel.h"
#include "light.h"

namespace painter::core::lanes {

namespace {

void LayColor(Pixel *Row, const std::uint16_t *Share, std::uint32_t Count, Pixel Color)
{
    for (std::uint32_t I = 0; I < Count; I++)
        if (Share[I]) Row[I] = light::Over(Row[I], Color, Share[I]);
}

void LayPaint(Pixel *Row, const Pixel *Paint, const std::uint16_t *Share, std::uint32_t Count)
{
    for (std::uint32_t I = 0; I < Count; I++)
        if (Share[I]) Row[I] = light::Over(Row[I], Paint[I], Share[I]);
}

void LayRow(Pixel *Row, const Pixel *From, std::uint32_t Count, std::uint32_t Share)
{
    for (std::uint32_t I = 0; I < Count; I++) Row[I] = light::Over(Row[I], From[I], Share);
}

void Show(std::uint32_t *Out, const Pixel *Row, std::uint32_t Count, std::uint32_t Format,
          const std::uint8_t *Thresholds)
{
    const light::Steps &S = light::StepsOfLight();
    for (std::uint32_t I = 0; I < Count; I++) {
        const Pixel P = Row[I];
        std::uint32_t R, G, B;
        if (Thresholds) {
            R = light::Dithered(P.Red, S, Thresholds[I]);
            G = light::Dithered(P.Green, S, Thresholds[I]);
            B = light::Dithered(P.Blue, S, Thresholds[I]);
        } else {
            R = BoxColorCode(P.Red);
            G = BoxColorCode(P.Green);
            B = BoxColorCode(P.Blue);
        }
        Out[I] = BoxPixelEncode(R << 16 | G << 8 | B, Format);
    }
}

}  // namespace

const Kit &Plain() noexcept
{
    static const Kit Made{LayColor, LayPaint, LayRow, Show};
    return Made;
}

}  // namespace painter::core::lanes

namespace painter::core::light {

const Steps &StepsOfLight() noexcept
{
    static const Steps Made = [] {
        Steps S{};
        std::uint32_t N = 0;
        for (std::uint32_t Step = 0; Step < 4096u; Step++) {
            while (N < 255u && BoxColorLight(N + 1u) <= Step * 16u) N++;
            S.Floor[Step] = static_cast<std::uint8_t>(N);
        }
        for (std::uint32_t Code = 0; Code < 255u; Code++) {
            const std::uint32_t Gap = BoxColorLight(Code + 1u) - BoxColorLight(Code);
            S.Reciprocal[Code]      = ((1u << 24) + Gap - 1u) / Gap;
        }
        S.Reciprocal[255] = 0;
        return S;
    }();
    return Made;
}

}  // namespace painter::core::light
