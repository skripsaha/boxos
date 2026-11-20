/*
 * lanes_plain.cpp — the engine's rows worked one pixel at a time: the very
 * numbers of light.h, the measure every other way is held to. See lanes.h.
 */

#include "lanes.h"

#include <new>

#include "boxos_pixel.h"
#include "light.h"
#include "paint_rows.h"

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
    /* Whole over whole is the paint itself, to the last bit (light.h Times
     * by 65535 gives back what it took, and leaves nothing of what was
     * beneath): the light is copied, not worked out. */
    if (Share == light::Whole) {
        for (std::uint32_t I = 0; I < Count; I++) Row[I] = From[I].Alpha == light::Whole ? From[I] : light::Over(Row[I], From[I], Share);
        return;
    }
    for (std::uint32_t I = 0; I < Count; I++) Row[I] = light::Over(Row[I], From[I], Share);
}

void Show(std::uint32_t *Out, const Pixel *Row, std::uint32_t Count, std::uint32_t Format,
          const std::uint8_t *Thresholds)
{
    const light::DitherTable *Table = Thresholds ? light::DitherTableOfLight() : nullptr;
    const light::Steps       &S     = light::StepsOfLight();
    for (std::uint32_t I = 0; I < Count; I++) {
        const Pixel P = Row[I];
        std::uint32_t R, G, B;
        if (Table) {
            R = light::DitheredBy(Table->Of[P.Red], Thresholds[I]);
            G = light::DitheredBy(Table->Of[P.Green], Thresholds[I]);
            B = light::DitheredBy(Table->Of[P.Blue], Thresholds[I]);
        } else if (Thresholds) {
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

void Gradient(Pixel *Out, const GradientWalk &Walk, std::uint32_t Count) { GradientPlain(Out, Walk, Walk.U, Walk.V, Count); }

void Held(Pixel *Into, const Pixel *From, std::uint32_t Count)
{
    for (std::uint32_t I = 0; I < Count; I++) Into[I] = light::Held(From[I]);
}

void Picture(Pixel *Out, const PictureWalk &Walk, std::uint32_t Count) { PicturePlain(Out, Walk, Walk.U, Walk.V, Count); }

}  // namespace

const Kit &Plain() noexcept
{
    static const Kit Made{LayColor, LayPaint, LayRow, Show, Gradient, Picture, Held};
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

const DitherTable *DitherTableOfLight() noexcept
{
    static const DitherTable *const Made = []() -> const DitherTable * {
        DitherTable *T = new (std::nothrow) DitherTable;
        if (!T) return nullptr;
        const Steps &S = StepsOfLight();
        for (std::uint32_t Light = 0; Light < 65536u; Light++) {
            std::uint32_t N, Part;
            Between(Light, S, N, Part);
            T->Of[Light] = static_cast<std::uint16_t>(Part << 8 | N);
        }
        T->Of[65536] = 0;
        return T;
    }();
    return Made;
}

}  // namespace painter::core::light
