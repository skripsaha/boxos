/*
 * blur.cpp — a Gaussian blur as three passes of a box, each way. See blur.h.
 *
 * A box's mean is its sum over its width, to the nearest — worked as a
 * multiply and a shift, not a division: the width's reciprocal rounded up,
 * which gives the very quotient for every sum a box of these values reaches
 * (Mean). Along a row a running sum, the line's ends apart from its middle so
 * the middle asks nothing of where it is; down the columns the same running
 * sums for every column at once, a row at a time, so the plane is read in
 * the order it lies.
 */

#include "blur.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace painter::core::engine {

namespace {

/* The widest blur drawn: a sigma past it is held to it, so no blur costs
 * past a bound. */
constexpr float SigmaMost = 256.0f;

/* (Sum + Wide / 2) / Wide, as Sum + Wide / 2 times 2^Shift / Wide rounded up,
 * shifted down. The product exceeds the quotient by less than Sum / 2^Shift,
 * so it is the quotient while (Sum + Wide / 2) · Wide ≤ 2^Shift: 2^27 for a
 * mask's values, none above 255 (a box under 724 wide), 2^40 for a pixel's,
 * none above 65535 (under 4096) — and a box is at most 513 wide. A mask's
 * multiplier fits 32 bits, so its products are widened ones. */
template <typename Times> struct Mean {
    Times         By;
    std::uint32_t Half;
    unsigned      Shift;

    Mean(std::int32_t Wide, unsigned Shift)
        : By(static_cast<Times>(((std::uint64_t{1} << Shift) + static_cast<std::uint64_t>(Wide) - 1) /
                                static_cast<std::uint64_t>(Wide))),
          Half(static_cast<std::uint32_t>(Wide / 2)), Shift(Shift)
    {
    }

    std::uint32_t operator()(std::uint32_t Sum) const noexcept
    {
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(Sum + Half) * By) >> Shift);
    }
};

/* Count values Step apart from First, blurred by a box of Radius into Out —
 * past the line's ends nothing (Held false) or its end value (Held true). */
template <typename Times>
void BoxLine(const std::uint32_t *First, std::size_t Step, std::int32_t Count, std::int32_t Radius, bool Held,
             const Mean<Times> &Of, std::uint32_t *Out)
{
    const auto At = [&](std::int32_t I) -> std::uint32_t {
        if (I < 0 || I >= Count) {
            if (!Held) return 0;
            I = std::clamp(I, 0, Count - 1);
        }
        return First[static_cast<std::size_t>(I) * Step];
    };
    std::uint32_t Sum = 0;
    for (std::int32_t K = -Radius; K <= Radius; K++) Sum += At(K);
    /* Where the box reaches past the line's start, where it lies within it,
     * and where it reaches past its end. */
    const std::int32_t Within = std::min(Radius, Count);
    const std::int32_t Past   = std::max(Within, Count - Radius - 1);
    std::int32_t       I      = 0;
    for (; I < Within; I++) {
        Out[I] = Of(Sum);
        Sum += At(I + Radius + 1);
        Sum -= At(I - Radius);
    }
    for (; I < Past; I++) {
        Out[I] = Of(Sum);
        Sum += First[static_cast<std::size_t>(I + Radius + 1) * Step];
        Sum -= First[static_cast<std::size_t>(I - Radius) * Step];
    }
    for (; I < Count; I++) {
        Out[I] = Of(Sum);
        Sum += At(I + Radius + 1);
        Sum -= At(I - Radius);
    }
}

/* Every column of the plane — Across values a row, Height rows — blurred by
 * a box of Radius into Out, a running sum for each column carried down a row
 * at a time: past the top and the bottom nothing, or the edge row. */
template <typename Times>
void BoxColumns(const std::uint32_t *Plane, std::size_t Across, std::int32_t Height, std::int32_t Radius, bool Held,
                const Mean<Times> &Of, std::uint32_t *Out, std::vector<std::uint32_t> &Sums)
{
    const auto Line = [&](std::int32_t Y) -> const std::uint32_t * {
        if (Y < 0 || Y >= Height) {
            if (!Held) return nullptr;
            Y = std::clamp(Y, 0, Height - 1);
        }
        return Plane + static_cast<std::size_t>(Y) * Across;
    };
    Sums.assign(Across, 0);
    std::uint32_t *Sum = Sums.data();
    for (std::int32_t K = -Radius; K <= Radius; K++)
        if (const std::uint32_t *Row = Line(K))
            for (std::size_t J = 0; J < Across; J++) Sum[J] += Row[J];
    for (std::int32_t Y = 0; Y < Height; Y++) {
        std::uint32_t *To = Out + static_cast<std::size_t>(Y) * Across;
        for (std::size_t J = 0; J < Across; J++) To[J] = Of(Sum[J]);
        const std::uint32_t *In = Line(Y + Radius + 1), *Leaving = Line(Y - Radius);
        if (In && Leaving) {
            for (std::size_t J = 0; J < Across; J++) Sum[J] = Sum[J] + In[J] - Leaving[J];
        } else if (In) {
            for (std::size_t J = 0; J < Across; J++) Sum[J] += In[J];
        } else if (Leaving) {
            for (std::size_t J = 0; J < Across; J++) Sum[J] -= Leaving[J];
        }
    }
}

/* Every line of the plane — rows, then columns — through the three boxes, for
 * each of Channels values a pixel. */
template <typename Times>
void Blur(std::uint32_t *Plane, std::int32_t Width, std::int32_t Height, std::int32_t Channels, const BlurBoxes &Boxes,
          bool Held, unsigned Shift)
{
    const std::size_t          Across = static_cast<std::size_t>(Width) * static_cast<std::size_t>(Channels);
    const std::size_t          Values = Across * static_cast<std::size_t>(Height);
    std::vector<std::uint32_t> Line(static_cast<std::size_t>(Width)), Out(Values), Sums;
    for (int Pass = 0; Pass < 3; Pass++) {
        if (Boxes.Radius[Pass] == 0) continue;
        const Mean<Times> Of(2 * Boxes.Radius[Pass] + 1, Shift);
        for (std::int32_t Y = 0; Y < Height; Y++)
            for (std::int32_t C = 0; C < Channels; C++) {
                std::uint32_t *First = Plane + static_cast<std::size_t>(Y) * Across + static_cast<std::size_t>(C);
                BoxLine(First, static_cast<std::size_t>(Channels), Width, Boxes.Radius[Pass], Held, Of, Line.data());
                for (std::int32_t X = 0; X < Width; X++)
                    First[static_cast<std::size_t>(X) * static_cast<std::size_t>(Channels)] = Line[static_cast<std::size_t>(X)];
            }
        BoxColumns(Plane, Across, Height, Boxes.Radius[Pass], Held, Of, Out.data(), Sums);
        std::memcpy(Plane, Out.data(), Values * sizeof(std::uint32_t));
    }
}

}  // namespace

BlurBoxes BoxesFor(float Sigma) noexcept
{
    BlurBoxes Boxes;
    if (!(Sigma > 0.25f)) return Boxes;
    Sigma               = std::min(Sigma, SigmaMost);
    const float  Square = 12.0f * Sigma * Sigma;
    std::int32_t Low    = static_cast<std::int32_t>(std::floor(std::sqrt(Square / 3.0f + 1.0f)));
    if (Low % 2 == 0) Low--;
    const std::int32_t High = Low + 2;
    const std::int32_t Lows = static_cast<std::int32_t>(
        std::lround((Square - 3.0f * static_cast<float>(Low * Low) - 12.0f * static_cast<float>(Low) - 9.0f) /
                    (-4.0f * static_cast<float>(Low) - 4.0f)));
    for (std::int32_t I = 0; I < 3; I++) {
        Boxes.Radius[I] = ((I < Lows ? Low : High) - 1) / 2;
        Boxes.Reach += Boxes.Radius[I];
    }
    return Boxes;
}

void BlurPlane(std::uint32_t *Plane, std::int32_t Width, std::int32_t Height, const BlurBoxes &Boxes)
{
    Blur<std::uint32_t>(Plane, Width, Height, 1, Boxes, false, 27);
}

void BlurPixels(std::uint32_t *Plane, std::int32_t Width, std::int32_t Height, const BlurBoxes &Boxes)
{
    Blur<std::uint64_t>(Plane, Width, Height, 4, Boxes, true, 40);
}

}  // namespace painter::core::engine
