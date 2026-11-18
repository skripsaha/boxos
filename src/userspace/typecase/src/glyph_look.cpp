/* glyph_look.cpp — a font's glyph drawn for text. See glyph_look.h. */

#include "typecase/glyph_look.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace typecase {

namespace {

/* tan 12°: how far a made-to-lean glyph moves across for every unit up. */
constexpr float Lean = 0.21255656f;

/* The scale up, Scale's across: the lowercase a whole number of pixels high. */
float RiseFor(const Font &Face, float Scale) noexcept
{
    const float Lowercase = Face.Metrics().XHeight;
    if (Lowercase <= 0) return Scale;
    const float Whole = std::round(Lowercase * Scale);
    return Whole >= 1 ? Whole / Lowercase : Scale;
}

/* Every contour's points, in turn: a contour begins at a Move and closes
 * back to it. */
template <typename Each>
void ForContours(const Outline &Shape, Each &&Work)
{
    std::size_t Point = 0, Step = 0;
    while (Step < Shape.Steps.size()) {
        const std::size_t First = Point;
        Point += 1, Step += 1;
        while (Step < Shape.Steps.size() && Shape.Steps[Step] != typecase::Step::Move) {
            Point += Shape.Steps[Step] == typecase::Step::Quad ? 2 : 1;
            Step += 1;
        }
        Work(First, std::min(Point, Shape.Points.size()));
    }
}

float Length(Point Of) noexcept { return std::sqrt(Of.X * Of.X + Of.Y * Of.Y); }

/* The outline grown by Strength in all — half of it outward on each side,
 * every point moved along the bisector of its two edges' outward normals so
 * that both edges move out by as much, a point between two edges that turn
 * back on each other moved less, so no edge folds over — then moved by half
 * of Strength right and up, so its left edge and its foot stay where they
 * were. A curve's control is moved as the points on it are. */
void Embolden(Outline &Shape, float Strength)
{
    /* Which way round the ink lies: TrueType's outer contours run clockwise
     * (the area negative), and their holes the other way. */
    float Area = 0;
    ForContours(Shape, [&](std::size_t First, std::size_t End) {
        for (std::size_t I = First; I < End; I++) {
            const Point A = Shape.Points[I], B = Shape.Points[I + 1 < End ? I + 1 : First];
            Area += A.X * B.Y - B.X * A.Y;
        }
    });
    const float Out  = Area < 0 ? 1.0f : -1.0f;
    const float Half = Strength / 2;

    std::vector<Point> Moved = Shape.Points;
    ForContours(Shape, [&](std::size_t First, std::size_t End) {
        const std::size_t Count = End - First;
        if (Count < 2) return;
        for (std::size_t I = 0; I < Count; I++) {
            const Point Here = Shape.Points[First + I];
            /* The nearest points before and after that are not this one. */
            std::size_t Back = I, Ahead = I;
            Point       In{}, Next{};
            for (std::size_t K = 1; K < Count; K++) {
                Back = (I + Count - K) % Count;
                In   = Point{Here.X - Shape.Points[First + Back].X, Here.Y - Shape.Points[First + Back].Y};
                if (In.X != 0 || In.Y != 0) break;
            }
            for (std::size_t K = 1; K < Count; K++) {
                Ahead = (I + K) % Count;
                Next  = Point{Shape.Points[First + Ahead].X - Here.X, Shape.Points[First + Ahead].Y - Here.Y};
                if (Next.X != 0 || Next.Y != 0) break;
            }
            const float InLength = Length(In), NextLength = Length(Next);
            if (InLength == 0 || NextLength == 0) continue;
            In   = Point{In.X / InLength, In.Y / InLength};
            Next = Point{Next.X / NextLength, Next.Y / NextLength};
            /* Outward normals: left of the way along for a clockwise contour. */
            const Point A{-In.Y * Out, In.X * Out}, B{-Next.Y * Out, Next.X * Out};
            const float Turn = 1 + A.X * B.X + A.Y * B.Y;   /* 1 + cos of the turn */
            if (Turn < 0.06f) continue;                     /* a spike turning back: left as it is */
            float       Factor = Half / Turn;
            const float Sine   = std::fabs(Next.X * In.Y - Next.Y * In.X);
            const float Room   = std::min(InLength, NextLength);
            if (Sine > 0 && Factor * Sine > Room) Factor = Room / Sine;
            Moved[First + I] = Point{Here.X + (A.X + B.X) * Factor, Here.Y + (A.Y + B.Y) * Factor};
        }
    });
    for (Point &Each : Moved) Each = Point{Each.X + Half, Each.Y + Half};
    Shape.Points = std::move(Moved);
}

/* The glyph's outline in font units as Look draws it — bold, leaning, its
 * pen's quarter of a pixel — with the scale across and up it is drawn at;
 * none for a glyph that does not read. */
std::optional<Outline> Shaped(const Font &Face, GlyphId Glyph, const GlyphLook &Look, float &Scale, float &Rise)
{
    if (!Face.Readable(Glyph)) return std::nullopt;
    auto Shape = Face.OutlineOf(Glyph);
    if (!Shape) return std::nullopt;
    Scale = Look.Em / Face.Metrics().UnitsPerEm;
    Rise  = RiseFor(Face, Scale);
    if (Scale <= 0) return std::nullopt;
    if (Look.Bold) Embolden(*Shape, BoldGrowth(Look.Em) / Scale);
    const float Shift = static_cast<float>(Look.Phase & 3u) / 4.0f / Scale;
    if (Look.Slant || Shift != 0)
        for (Point &Each : Shape->Points) Each.X += (Look.Slant ? Each.Y * Lean : 0.0f) + Shift;
    return std::move(*Shape);
}

}  // namespace

float BoldGrowth(float Em) noexcept
{
    const float Part = std::clamp((Em - 9.0f) / 27.0f, 0.0f, 1.0f);
    return Em * (1.0f / 24.0f + (1.0f / 32.0f - 1.0f / 24.0f) * Part);
}

float AdvanceOf(const Font &Face, GlyphId Glyph, const GlyphLook &Look) noexcept
{
    const float Plain = static_cast<float>(Face.AdvanceOf(Glyph)) * Look.Em / Face.Metrics().UnitsPerEm;
    return Look.Bold ? Plain + BoldGrowth(Look.Em) : Plain;
}

Outline GlyphOutline(const Font &Face, GlyphId Glyph, const GlyphLook &Look)
{
    float      Scale = 0, Rise = 0;
    const auto Shape = Shaped(Face, Glyph, Look, Scale, Rise);
    if (!Shape) return {};
    Outline Drawn{Shape->Steps, Shape->Points};
    for (Point &Each : Drawn.Points) Each = Point{Each.X * Scale, -Each.Y * Rise};
    return Drawn;
}

GlyphImage DrawGlyph(const Font &Face, GlyphId Glyph, const GlyphLook &Look, Bounds Limit, Rasterizer &Pen)
{
    float      Scale = 0, Rise = 0;
    const auto Shape = Shaped(Face, Glyph, Look, Scale, Rise);
    if (!Shape) return GlyphImage{0, 0, 0, 0, AdvanceOf(Face, Glyph, Look), {}};
    return Pen.Draw(*Shape, Scale, Rise, Limit, AdvanceOf(Face, Glyph, Look));
}

GlyphImage DrawGlyph(const Font &Face, GlyphId Glyph, const GlyphLook &Look, Rasterizer &Pen)
{
    const std::int32_t Margin  = static_cast<std::int32_t>(std::ceil(2.0f * Look.Em)) + 1;
    const std::int32_t Advance = static_cast<std::int32_t>(std::ceil(std::clamp(AdvanceOf(Face, Glyph, Look), 0.0f, 4.0f * Look.Em + 4.0f)));
    return DrawGlyph(Face, Glyph, Look, Bounds{-Margin, -Margin, Advance + Margin, Margin}, Pen);
}

}  // namespace typecase
