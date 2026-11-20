#ifndef PAINTER_CORE_PAINT_ROWS_H
#define PAINTER_CORE_PAINT_ROWS_H

/* paint_rows.h — a gradient's and a smooth picture's colours along a row,
 * one pixel at a time: the measure every way of working rows (lanes.h) is
 * held to, and what a way falls back on where it has no faster rule. */

#include <cstdint>

#include "lanes.h"
#include "light.h"

namespace painter::core::lanes {

/* The colour At of a gradient's way: between the two kept every 16 of it,
 * straight. */
inline Pixel Along(const Pixel *Table, std::uint32_t At) noexcept
{
    const Pixel         A = Table[At >> 4], B = Table[(At >> 4) + 1];
    const std::uint32_t Part = At & 15u, Rest = 16u - Part;
    return Pixel{static_cast<std::uint16_t>((A.Red * Rest + B.Red * Part + 8u) >> 4),
                 static_cast<std::uint16_t>((A.Green * Rest + B.Green * Part + 8u) >> 4),
                 static_cast<std::uint16_t>((A.Blue * Rest + B.Blue * Part + 8u) >> 4),
                 static_cast<std::uint16_t>((A.Alpha * Rest + B.Alpha * Part + 8u) >> 4)};
}

/* Count pixels of a gradient's row, from the walk's sums SumU, SumV on, one
 * at a time: each sum rounded to its place, the way there brought within the
 * gradient's ends, and the colour there; clear where it is clear. */
inline void GradientPlain(Pixel *Out, const GradientWalk &Walk, std::int64_t SumU, std::int64_t SumV, std::uint32_t Count) noexcept
{
    const BoxPaint     P     = *Walk.Paint;
    const Pixel       *Table = Walk.Table;
    const std::int64_t StepU = Walk.StepU, StepV = Walk.StepV;
    for (std::uint32_t I = 0; I < Count; I++, SumU += StepU, SumV += StepV) {
        std::uint32_t At;
        Out[I] = BoxPaintWayAt(&P, BoxShapeRound(SumU), BoxShapeRound(SumV), &At) ? Along(Table, At) : Pixel{};
    }
}

/* … of a smooth picture's: the four pixels whose middles lie round each
 * place, weighed by nearness — read straight where all four lie within the
 * picture, where every way of going on past it leaves a pixel where it is,
 * the weights making 65536 so the sums keep to 32 bits; else each asked how
 * the picture goes on past its edge, in 64 — at the walk's opacity. */
inline void PicturePlain(Pixel *Out, const PictureWalk &Walk, std::int64_t SumU, std::int64_t SumV, std::uint32_t Count) noexcept
{
    const BoxPaint      P       = *Walk.Paint;
    const Pixel        *Texels  = Walk.Texels;
    const std::uint32_t Opacity = Walk.Opacity;
    const std::int64_t  Wide = P.PictureWidth, High = P.PictureHeight;
    const std::int64_t  StepU = Walk.StepU, StepV = Walk.StepV;
    for (std::uint32_t I = 0; I < Count; I++, SumU += StepU, SumV += StepV) {
        std::int64_t  TI, TJ;
        std::uint32_t FX, FY;
        BoxPaintBetween(BoxShapeRound(SumU), BoxShapeRound(SumV), &TI, &TJ, &FX, &FY);
        if (TI >= 0 && TJ >= 0 && TI + 1 < Wide && TJ + 1 < High) {
            const Pixel        *Up   = Texels + static_cast<std::size_t>(TJ) * P.PictureStride + static_cast<std::size_t>(TI);
            const Pixel        *Down = Up + P.PictureStride;
            const std::uint32_t W0 = (256u - FX) * (256u - FY), W1 = FX * (256u - FY), W2 = (256u - FX) * FY, W3 = FX * FY;
            const auto          Mix = [&](std::uint16_t Pixel::*C) {
                return static_cast<std::uint16_t>((32768u + W0 * Up[0].*C + W1 * Up[1].*C + W2 * Down[0].*C + W3 * Down[1].*C) >> 16);
            };
            Out[I] = Pixel{Mix(&Pixel::Red), Mix(&Pixel::Green), Mix(&Pixel::Blue), Mix(&Pixel::Alpha)};
            if (Opacity < light::Whole) Out[I] = light::Scaled(Out[I], Opacity);
            continue;
        }
        const std::uint64_t Weights[4] = {(256u - FX) * (256u - FY), FX * (256u - FY), (256u - FX) * FY, FX * FY};
        std::uint64_t       Sum[4]     = {32768u, 32768u, 32768u, 32768u};
        std::size_t         Index;
        for (std::uint32_t K = 0; K < 4u; K++) {
            if (!Weights[K] || !BoxPaintTexel(&P, TI + (K & 1u), TJ + (K >> 1), &Index)) continue;
            const Pixel T = Texels[Index];
            Sum[0] += Weights[K] * T.Red;
            Sum[1] += Weights[K] * T.Green;
            Sum[2] += Weights[K] * T.Blue;
            Sum[3] += Weights[K] * T.Alpha;
        }
        Out[I] = Pixel{static_cast<std::uint16_t>(Sum[0] >> 16), static_cast<std::uint16_t>(Sum[1] >> 16),
                       static_cast<std::uint16_t>(Sum[2] >> 16), static_cast<std::uint16_t>(Sum[3] >> 16)};
        if (Opacity < light::Whole) Out[I] = light::Scaled(Out[I], Opacity);
    }
}

}  // namespace painter::core::lanes

#endif  // PAINTER_CORE_PAINT_ROWS_H
