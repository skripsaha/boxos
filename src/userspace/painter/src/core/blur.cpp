/*
 * blur.cpp — a Gaussian blur as three passes of a box, each way. See blur.h.
 */

#include "blur.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace painter::core::engine {

namespace {

/* The widest blur drawn: a sigma past it is held to it, so no blur costs
 * past a bound. */
constexpr float SigmaMost = 256.0f;

/* Count values Step apart from First, blurred by a box of Radius into Out —
 * past the line's ends nothing (Held false) or its end value (Held true). */
void BoxLine(const std::uint32_t *First, std::size_t Step, std::int32_t Count, std::int32_t Radius, bool Held,
             std::uint32_t *Out)
{
    const std::int32_t Wide = 2 * Radius + 1;
    const auto         At   = [&](std::int32_t I) -> std::uint64_t {
        if (I < 0 || I >= Count) {
            if (!Held) return 0;
            I = std::clamp(I, 0, Count - 1);
        }
        return First[static_cast<std::size_t>(I) * Step];
    };
    std::uint64_t Sum = 0;
    for (std::int32_t K = -Radius; K <= Radius; K++) Sum += At(K);
    for (std::int32_t I = 0; I < Count; I++) {
        Out[I] = static_cast<std::uint32_t>((Sum + static_cast<std::uint64_t>(Wide / 2)) / static_cast<std::uint64_t>(Wide));
        Sum += At(I + Radius + 1);
        Sum -= At(I - Radius);
    }
}

/* Every line of the plane — rows, then columns — through the three boxes, for
 * each of Channels values a pixel. */
void Blur(std::uint32_t *Plane, std::int32_t Width, std::int32_t Height, std::int32_t Channels, const BlurBoxes &Boxes,
          bool Held)
{
    std::vector<std::uint32_t> Line(static_cast<std::size_t>(std::max(Width, Height)));
    const std::size_t          Across = static_cast<std::size_t>(Channels);
    const std::size_t          Down   = static_cast<std::size_t>(Width) * Across;
    for (int Pass = 0; Pass < 3; Pass++) {
        if (Boxes.Radius[Pass] == 0) continue;
        for (std::int32_t Y = 0; Y < Height; Y++)
            for (std::int32_t C = 0; C < Channels; C++) {
                std::uint32_t *First = Plane + static_cast<std::size_t>(Y) * Down + static_cast<std::size_t>(C);
                BoxLine(First, Across, Width, Boxes.Radius[Pass], Held, Line.data());
                for (std::int32_t X = 0; X < Width; X++) First[static_cast<std::size_t>(X) * Across] = Line[static_cast<std::size_t>(X)];
            }
        for (std::int32_t X = 0; X < Width; X++)
            for (std::int32_t C = 0; C < Channels; C++) {
                std::uint32_t *First = Plane + static_cast<std::size_t>(X) * Across + static_cast<std::size_t>(C);
                BoxLine(First, Down, Height, Boxes.Radius[Pass], Held, Line.data());
                for (std::int32_t Y = 0; Y < Height; Y++) First[static_cast<std::size_t>(Y) * Down] = Line[static_cast<std::size_t>(Y)];
            }
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
    Blur(Plane, Width, Height, 1, Boxes, false);
}

void BlurPixels(std::uint32_t *Plane, std::int32_t Width, std::int32_t Height, const BlurBoxes &Boxes)
{
    Blur(Plane, Width, Height, 4, Boxes, true);
}

}  // namespace painter::core::engine
