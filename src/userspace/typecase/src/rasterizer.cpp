/* rasterizer.cpp — an outline to a picture of coverage, by the one coverage
 * every hand of BoxOS draws with (boxos_shape.h). */

#include "typecase/rasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "boxos_shape.h"

namespace typecase {

namespace {

/* A place of the picture, in pixels from its corner, as a place of a shape:
 * 1/BOX_SHAPE_ONE of a pixel, kept within the shape's reach — a control point
 * a damaged font throws past it is drawn at the reach's edge, so the glyph is
 * still drawn, and drawn within its picture. */
std::int32_t Place(float Pixels) noexcept
{
    constexpr float Reach = static_cast<float>(BOX_SHAPE_REACH - 1) * BOX_SHAPE_ONE;
    return static_cast<std::int32_t>(std::lround(std::clamp(Pixels * BOX_SHAPE_ONE, -Reach, Reach)));
}

}  // namespace

GlyphImage Rasterizer::Draw(const Outline &Shape, float Scale, float Rise, Bounds Limit, float Advance)
{
    GlyphImage Image;
    Image.Advance = Advance;

    /* Font units to pixels against the pen, y down; the ink's extent from
     * every point, controls too — a curve keeps within its controls. */
    std::vector<Point> At(Shape.Points.size());
    float Low = std::numeric_limits<float>::max(), High = std::numeric_limits<float>::lowest();
    float Up = Low, Down = High;
    for (std::size_t Index = 0; Index < At.size(); Index++) {
        At[Index] = Point{Shape.Points[Index].X * Scale, -Shape.Points[Index].Y * Rise};
        Low = std::min(Low, At[Index].X), High = std::max(High, At[Index].X);
        Up = std::min(Up, At[Index].Y), Down = std::max(Down, At[Index].Y);
    }
    if (At.empty()) return Image;

    const auto Clamp = [](float Value, std::int32_t Least, std::int32_t Most) {
        return static_cast<std::int32_t>(std::clamp(Value, static_cast<float>(Least), static_cast<float>(Most)));
    };
    const std::int32_t Left   = Clamp(std::floor(Low), Limit.Left, Limit.Right);
    const std::int32_t Right  = Clamp(std::ceil(High), Limit.Left, Limit.Right);
    const std::int32_t Top    = Clamp(std::floor(Up), Limit.Top, Limit.Bottom);
    const std::int32_t Bottom = Clamp(std::ceil(Down), Limit.Top, Limit.Bottom);
    if (Right <= Left || Bottom <= Top) return Image;
    const std::int32_t Width = Right - Left, Height = Bottom - Top;

    /* The outline as a shape on the picture, its corner at 0, 0; built again
     * in room enough when the room was short. */
    const BoxMatrix Plain = {BOX_SHAPE_UNIT, 0, 0, BOX_SHAPE_UNIT, 0, 0};
    BoxShape        Drawn;
    for (;;) {
        BoxShapeBegin(&Drawn, Edges.data(), static_cast<std::uint32_t>(Edges.size()), &Plain);
        std::size_t Next = 0;
        for (Step Each : Shape.Steps) {
            const auto X = [&](std::size_t I) { return Place(At[I].X - static_cast<float>(Left)); };
            const auto Y = [&](std::size_t I) { return Place(At[I].Y - static_cast<float>(Top)); };
            switch (Each) {
            case Step::Move: BoxShapeMoveTo(&Drawn, X(Next), Y(Next)); Next += 1; break;
            case Step::Line: BoxShapeLineTo(&Drawn, X(Next), Y(Next)); Next += 1; break;
            case Step::Quad:
                BoxShapeQuadTo(&Drawn, X(Next), Y(Next), X(Next + 1), Y(Next + 1));
                Next += 2;
                break;
            }
        }
        BoxShapeClose(&Drawn);
        if (!Drawn.Short) break;
        if (Drawn.Spilled || Drawn.Wanted > BOX_SHAPE_EDGES_MOST) return Image;
        Edges.resize(Drawn.Wanted);
    }
    if (!BoxShapeDrawable(&Drawn)) return Image;

    Image.Left   = Left;
    Image.Top    = Top;
    Image.Width  = Width;
    Image.Height = Height;
    Image.Coverage.assign(static_cast<std::size_t>(Width) * static_cast<std::size_t>(Height), 0);
    const BoxRect Held = BoxShapeReach(&Drawn, BoxRect{0, 0, Width, Height});
    if (Held.Width <= 0 || Held.Height <= 0) return Image;
    Cells.resize(static_cast<std::size_t>(Width) + 2u);
    BoxShapeSweep Sweep;
    BoxShapeSweepBegin(&Sweep, &Drawn, Cells.data(), 0, Width);
    for (std::int32_t Row = Held.Y; Row < Held.Y + Held.Height; Row++) {
        BoxShapeSweepRow(&Sweep, Row);
        std::int64_t   Run  = Cells[0];
        std::uint8_t  *Line = &Image.Coverage[static_cast<std::size_t>(Row) * static_cast<std::size_t>(Width)];
        for (std::int32_t Column = 0; Column < Width; Column++) {
            Run += Cells[static_cast<std::size_t>(Column) + 1u];
            Line[Column] = static_cast<std::uint8_t>(BoxShapeCoverage(Run, BOX_SHAPE_NONZERO));
        }
    }
    return Image;
}

}  // namespace typecase
