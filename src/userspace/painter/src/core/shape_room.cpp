/*
 * shape_room.cpp — a path built as a shape of the one coverage. See
 * shape_room.h.
 */

#include "shape_room.h"

#include <cmath>

#include "boxos_stroke.h"

namespace painter::core::engine {

namespace {

/* A length or place of the path's own, in places — one past the most a shape
 * takes when it is past it, or not a number, so the shape is refused. */
std::int32_t Own(float Pixels) noexcept
{
    constexpr float Most = static_cast<float>(BOX_SHAPE_OWN_MOST);
    const float     Places = Pixels * BOX_SHAPE_ONE;
    if (!(std::fabs(Places) <= Most)) return BOX_SHAPE_OWN_MOST + 1;
    return static_cast<std::int32_t>(std::lround(Places));
}

/* A rectangle of the path's, its corner where its sides are least. */
void Upright(PointF &Corner, PointF &Size) noexcept
{
    if (Size.X < 0) Corner.X += Size.X, Size.X = -Size.X;
    if (Size.Y < 0) Corner.Y += Size.Y, Size.Y = -Size.Y;
}

}  // namespace

std::optional<BoxMatrix> MatrixOf(const Transform &Place) noexcept
{
    constexpr float Stretch = static_cast<float>(BOX_SHAPE_STRETCH_MOST);
    constexpr float Reach   = static_cast<float>(BOX_SHAPE_REACH);
    const float     Parts[4] = {Place.A, Place.B, Place.C, Place.D};
    BoxMatrix       M;
    std::int32_t   *Into[4] = {&M.A, &M.B, &M.C, &M.D};
    for (int I = 0; I < 4; I++) {
        if (!(std::fabs(Parts[I]) <= Stretch)) return std::nullopt;
        *Into[I] = static_cast<std::int32_t>(std::lround(Parts[I] * BOX_SHAPE_UNIT));
    }
    if (!(std::fabs(Place.E) <= Reach) || !(std::fabs(Place.F) <= Reach)) return std::nullopt;
    M.E = static_cast<std::int32_t>(std::lround(Place.E * BOX_SHAPE_ONE));
    M.F = static_cast<std::int32_t>(std::lround(Place.F * BOX_SHAPE_ONE));
    return M;
}

template <typename Draw> bool ShapeRoom::Build(const Transform &Place, Draw &&Lay)
{
    const std::optional<BoxMatrix> Matrix = MatrixOf(Place);
    if (!Matrix) return false;
    for (;;) {
        BoxShapeBegin(&Shape, Edges.data(), static_cast<std::uint32_t>(Edges.size()), &*Matrix);
        Lay();
        BoxShapeClose(&Shape);
        if (!Shape.Short) return !Shape.Spilled;
        if (Shape.Spilled || Shape.Wanted > BOX_SHAPE_EDGES_MOST) return false;
        Edges.resize(Shape.Wanted);
    }
}

bool ShapeRoom::Fill(const Path &From, const Transform &Place, FillRule Rule)
{
    return Build(Place, [&] {
        Shape.Rule = Rule == FillRule::EvenOdd ? BOX_SHAPE_EVENODD : BOX_SHAPE_NONZERO;
        const auto  P    = From.Points();
        std::size_t Next = 0;
        for (Path::Step Each : From.Steps()) {
            switch (Each) {
            case Path::Step::Move: BoxShapeMoveTo(&Shape, Own(P[Next].X), Own(P[Next].Y)); Next += 1; break;
            case Path::Step::Line: BoxShapeLineTo(&Shape, Own(P[Next].X), Own(P[Next].Y)); Next += 1; break;
            case Path::Step::Quad:
                BoxShapeQuadTo(&Shape, Own(P[Next].X), Own(P[Next].Y), Own(P[Next + 1].X), Own(P[Next + 1].Y));
                Next += 2;
                break;
            case Path::Step::Cubic:
                BoxShapeCubicTo(&Shape, Own(P[Next].X), Own(P[Next].Y), Own(P[Next + 1].X), Own(P[Next + 1].Y),
                                Own(P[Next + 2].X), Own(P[Next + 2].Y));
                Next += 3;
                break;
            case Path::Step::Close: BoxShapeClose(&Shape); break;
            case Path::Step::Rect: {
                PointF Corner = P[Next], Size = P[Next + 1];
                Upright(Corner, Size);
                BoxShapeAddRect(&Shape, Own(Corner.X), Own(Corner.Y), Own(Size.X), Own(Size.Y), 0);
                Next += 2;
                break;
            }
            case Path::Step::Rounded: {
                PointF Corner = P[Next], Size = P[Next + 1];
                Upright(Corner, Size);
                const std::int32_t Radii[4] = {Own(P[Next + 2].X), Own(P[Next + 2].Y), Own(P[Next + 3].X),
                                               Own(P[Next + 3].Y)};
                BoxShapeAddRounded(&Shape, Own(Corner.X), Own(Corner.Y), Own(Size.X), Own(Size.Y), Radii, 0);
                Next += 4;
                break;
            }
            case Path::Step::Ellipse:
                BoxShapeAddEllipse(&Shape, Own(P[Next].X), Own(P[Next].Y), Own(std::fabs(P[Next + 1].X)),
                                   Own(std::fabs(P[Next + 1].Y)), 0);
                Next += 2;
                break;
            }
        }
    });
}

bool ShapeRoom::Stroke(const Path &From, const core::Stroke &Line, const Transform &Place, float Widen)
{
    Dashes.clear();
    for (float Length : Line.Dashes) Dashes.push_back(Own(Length));
    return Build(Place, [&] {
        BoxStroke K;
        BoxStrokeBegin(&K, &Shape, Own(Line.Width * Widen), static_cast<std::uint32_t>(Line.Cap),
                       static_cast<std::uint32_t>(Line.Join));
        BoxStrokeMiterLimit(&K, static_cast<std::uint32_t>(std::lround(std::fmin(std::fmax(Line.MiterLimit, 1.0f), 255.0f) * 256.0f)));
        if (!Dashes.empty())
            BoxStrokeDash(&K, Dashes.data(), static_cast<std::uint32_t>(Dashes.size()), Own(Line.DashOffset));
        const auto  P    = From.Points();
        std::size_t Next = 0;
        for (Path::Step Each : From.Steps()) {
            switch (Each) {
            case Path::Step::Move: BoxStrokeMoveTo(&K, Own(P[Next].X), Own(P[Next].Y)); Next += 1; break;
            case Path::Step::Line: BoxStrokeLineTo(&K, Own(P[Next].X), Own(P[Next].Y)); Next += 1; break;
            case Path::Step::Quad:
                BoxStrokeQuadTo(&K, Own(P[Next].X), Own(P[Next].Y), Own(P[Next + 1].X), Own(P[Next + 1].Y));
                Next += 2;
                break;
            case Path::Step::Cubic:
                BoxStrokeCubicTo(&K, Own(P[Next].X), Own(P[Next].Y), Own(P[Next + 1].X), Own(P[Next + 1].Y),
                                 Own(P[Next + 2].X), Own(P[Next + 2].Y));
                Next += 3;
                break;
            case Path::Step::Close: BoxStrokeClose(&K); break;
            case Path::Step::Rect: {
                PointF Corner = P[Next], Size = P[Next + 1];
                Upright(Corner, Size);
                BoxStrokeEnd(&K);
                BoxStrokeRect(&K, Own(Corner.X), Own(Corner.Y), Own(Size.X), Own(Size.Y));
                Next += 2;
                break;
            }
            case Path::Step::Rounded: {
                PointF Corner = P[Next], Size = P[Next + 1];
                Upright(Corner, Size);
                const std::int32_t Radii[4] = {Own(P[Next + 2].X), Own(P[Next + 2].Y), Own(P[Next + 3].X),
                                               Own(P[Next + 3].Y)};
                BoxStrokeEnd(&K);
                BoxStrokeRounded(&K, Own(Corner.X), Own(Corner.Y), Own(Size.X), Own(Size.Y), Radii);
                Next += 4;
                break;
            }
            case Path::Step::Ellipse:
                BoxStrokeEnd(&K);
                BoxStrokeEllipse(&K, Own(P[Next].X), Own(P[Next].Y), Own(std::fabs(P[Next + 1].X)),
                                 Own(std::fabs(P[Next + 1].Y)));
                Next += 2;
                break;
            }
        }
        BoxStrokeEnd(&K);
    });
}

}  // namespace painter::core::engine
