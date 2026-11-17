/*
 * shadow.cpp — the shadow a shape casts, made once as a mask. See
 * painter/core/shadow.h.
 */

#include "painter/core/shadow.h"

#include <algorithm>
#include <cmath>

#include "blur.h"
#include "painter/core/stroke.h"
#include "shape_room.h"

namespace painter::core {

namespace {

/* The canvas's whole reach: every pixel a shape may land on. */
constexpr Rect Everywhere{-BOX_SHAPE_REACH, -BOX_SHAPE_REACH, 2 * BOX_SHAPE_REACH, 2 * BOX_SHAPE_REACH};

/* Shape laid by Place, grown by Spread — the line round it, Spread on either
 * side with round corners, joined to it — or shrunk by it, the line cut
 * away; within Within. */
std::optional<Mask> Grown(const Path &Shape, const Transform &Place, float Spread, Rect Within)
{
    std::optional<Mask> Body = Mask::Of(Shape, Place, FillRule::NonZero, Within);
    if (!Body || Spread == 0.0f) return Body;
    core::Stroke Edge;
    Edge.Width = 2.0f * std::fabs(Spread);
    Edge.Join  = LineJoin::Round;
    Edge.Cap   = LineCap::Round;
    engine::ShapeRoom Room;
    if (!Room.Stroke(Shape, Edge, Place, 1.0f)) return std::nullopt;
    BoxShape     &S    = Room.Built();
    const BoxRect Held = BoxShapeReach(&S, BoxRect{Within.X, Within.Y, Within.Width, Within.Height});
    const Rect    Area = Spread > 0 ? Body->Area().Join(Rect{Held.X, Held.Y, Held.Width, Held.Height}) : Body->Area();
    Mask          Made(Area);
    if (Made.Empty()) return Made;
    std::vector<std::int32_t> Cells(static_cast<std::size_t>(Area.Width) + 2u);
    BoxShapeSweep             Sweep;
    BoxShapeSweepBegin(&Sweep, &S, Cells.data(), Area.X, Area.X + Area.Width);
    for (std::int32_t Y = Area.Y; Y < Area.Y + Area.Height; Y++) {
        BoxShapeSweepRow(&Sweep, Y);
        std::int64_t  Run  = Cells[0];
        std::uint8_t *Line = Made.Row(Y);
        for (std::int32_t I = 0; I < Area.Width; I++) {
            Run += Cells[static_cast<std::size_t>(I) + 1u];
            const std::uint32_t Band = BoxShapeCoverage(Run, BOX_SHAPE_NONZERO);
            const std::uint32_t Fill = Body->At(Area.X + I, Y);
            Line[I] = static_cast<std::uint8_t>(Spread > 0 ? std::max(Fill, Band) : (Fill * (255u - Band) + 127u) / 255u);
        }
    }
    return Made;
}

}  // namespace

std::optional<Mask> Shadow::MaskFor(const Path &Shape, const Transform &Place) const
{
    const Transform Moved = Place.Then(Transform::Translate(Offset.X, Offset.Y));
    if (!Inner) {
        std::optional<Mask> Cast = Grown(Shape, Moved, Spread, Everywhere);
        if (Cast) Cast->Blur(Blur);
        return Cast;
    }
    /* Within: what lies outside the shape moved and shrunk, blurred, seen
     * through the shape itself. */
    const std::optional<Mask> Body = Mask::Of(Shape, Place, FillRule::NonZero, Everywhere);
    if (!Body) return std::nullopt;
    if (Body->Empty()) return Body;
    const std::int32_t Reach = engine::BoxesFor(Blur / 2.0f).Reach + 1 +
                               static_cast<std::int32_t>(std::ceil(std::fabs(Offset.X) + std::fabs(Offset.Y)));
    const Rect Around{Body->Area().X - Reach, Body->Area().Y - Reach, Body->Area().Width + 2 * Reach,
                      Body->Area().Height + 2 * Reach};
    const std::optional<Mask> Hole = Grown(Shape, Moved, -Spread, Around);
    if (!Hole) return std::nullopt;
    Mask Outside(Around);
    for (std::int32_t Y = Around.Y; Y < Around.Y + Around.Height; Y++) {
        std::uint8_t *Line = Outside.Row(Y);
        for (std::int32_t I = 0; I < Around.Width; I++) Line[I] = static_cast<std::uint8_t>(255u - Hole->At(Around.X + I, Y));
    }
    Outside.Blur(Blur);
    Mask Made(Body->Area());
    for (std::int32_t Y = Made.Area().Y; Y < Made.Area().Y + Made.Area().Height; Y++) {
        std::uint8_t *Line = Made.Row(Y);
        for (std::int32_t I = 0; I < Made.Area().Width; I++) {
            const std::int32_t X = Made.Area().X + I;
            Line[I] = static_cast<std::uint8_t>((Body->At(X, Y) * Outside.At(X, Y) + 127u) / 255u);
        }
    }
    return Made;
}

}  // namespace painter::core
