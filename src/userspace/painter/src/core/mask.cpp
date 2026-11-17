/*
 * mask.cpp — how much of each pixel of a rectangle something covers. See
 * painter/core/mask.h.
 */

#include "painter/core/mask.h"

#include <algorithm>
#include <cmath>

#include "blur.h"
#include "shape_room.h"

namespace painter::core {

Mask::Mask(Rect Area)
    : Box(Area.Empty() ? Rect{} : Area),
      Values(static_cast<std::size_t>(Box.Empty() ? 0 : Box.Width) * static_cast<std::size_t>(Box.Empty() ? 0 : Box.Height), 0)
{
}

std::optional<Mask> Mask::Of(const Path &Shape, const Transform &Place, FillRule Rule, Rect Within)
{
    engine::ShapeRoom Room;
    if (!Room.Fill(Shape, Place, Rule)) return std::nullopt;
    BoxShape     &S    = Room.Built();
    const BoxRect Held = BoxShapeReach(&S, BoxRect{Within.X, Within.Y, Within.Width, Within.Height});
    Mask          Made(Rect{Held.X, Held.Y, Held.Width, Held.Height});
    if (Made.Empty()) return Made;
    std::vector<std::int32_t> Cells(static_cast<std::size_t>(Held.Width) + 2u);
    BoxShapeSweep             Sweep;
    BoxShapeSweepBegin(&Sweep, &S, Cells.data(), Held.X, Held.X + Held.Width);
    for (std::int32_t Y = Held.Y; Y < Held.Y + Held.Height; Y++) {
        BoxShapeSweepRow(&Sweep, Y);
        std::uint8_t *Line = Made.Row(Y);
        std::int64_t  Run  = Cells[0];
        for (std::int32_t X = 0; X < Held.Width; X++) {
            Run += Cells[static_cast<std::size_t>(X) + 1u];
            Line[X] = static_cast<std::uint8_t>(BoxShapeCoverage(Run, S.Rule));
        }
    }
    return Made;
}

std::uint8_t Mask::At(std::int32_t X, std::int32_t Y) const noexcept
{
    if (X < Box.X || Y < Box.Y || X >= Box.X + Box.Width || Y >= Box.Y + Box.Height) return 0;
    return Row(Y)[X - Box.X];
}

void Mask::Blur(float Radius)
{
    const engine::BlurBoxes Boxes = engine::BoxesFor(Radius / 2.0f);
    if (Empty() || Boxes.Reach == 0) return;
    Rect Grown{Box.X - Boxes.Reach, Box.Y - Boxes.Reach, Box.Width + 2 * Boxes.Reach, Box.Height + 2 * Boxes.Reach};
    std::vector<std::uint32_t> Plane(static_cast<std::size_t>(Grown.Width) * static_cast<std::size_t>(Grown.Height), 0);
    for (std::int32_t Y = 0; Y < Box.Height; Y++)
        for (std::int32_t X = 0; X < Box.Width; X++)
            Plane[static_cast<std::size_t>(Y + Boxes.Reach) * static_cast<std::size_t>(Grown.Width) +
                  static_cast<std::size_t>(X + Boxes.Reach)] = Row(Box.Y + Y)[X];
    engine::BlurPlane(Plane.data(), Grown.Width, Grown.Height, Boxes);
    Box = Grown;
    Values.resize(Plane.size());
    for (std::size_t I = 0; I < Plane.size(); I++) Values[I] = static_cast<std::uint8_t>(std::min<std::uint32_t>(Plane[I], 255u));
}

Mask Mask::Moved(Point By) const
{
    Mask Made = *this;
    Made.Box.X += By.X;
    Made.Box.Y += By.Y;
    return Made;
}

}  // namespace painter::core
