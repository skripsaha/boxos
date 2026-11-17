/*
 * gradient.cpp — colours along a line, out from a middle, or round it. See
 * painter/core/gradient.h.
 */

#include "painter/core/gradient.h"

#include <algorithm>
#include <cmath>

namespace painter::core {

namespace {

/* Stops in the order of their offsets, each within 0 to 1. */
std::vector<GradientStop> Ordered(std::vector<GradientStop> Stops)
{
    for (GradientStop &Each : Stops) Each.Offset = std::isfinite(Each.Offset) ? std::clamp(Each.Offset, 0.0f, 1.0f) : 0.0f;
    std::stable_sort(Stops.begin(), Stops.end(),
                     [](const GradientStop &A, const GradientStop &B) { return A.Offset < B.Offset; });
    return Stops;
}

}  // namespace

Gradient::Gradient(Kind Shape, Transform Own, std::vector<GradientStop> Stops, core::Extend Past)
    : Shape(Shape), Own(Own), StopList(Ordered(std::move(Stops))), PastEnds(Past)
{
}

Gradient Gradient::Linear(PointF From, PointF To, std::vector<GradientStop> Stops, core::Extend Past)
{
    const float DX = To.X - From.X, DY = To.Y - From.Y;
    return Gradient(Kind::Linear, Transform{DX, DY, -DY, DX, From.X, From.Y}, std::move(Stops), Past);
}

Gradient Gradient::Radial(PointF Middle, float Radius, std::vector<GradientStop> Stops, core::Extend Past)
{
    return Gradient(Kind::Radial, Transform{Radius, 0, 0, Radius, Middle.X, Middle.Y}, std::move(Stops), Past);
}

Gradient Gradient::Radial(PointF Middle, float Radius, PointF Focus, std::vector<GradientStop> Stops,
                          core::Extend Past)
{
    Gradient Made = Radial(Middle, Radius, std::move(Stops), Past);
    if (Radius != 0.0f) Made.FocusAt = {(Focus.X - Middle.X) / Radius, (Focus.Y - Middle.Y) / Radius};
    return Made;
}

Gradient Gradient::Conic(PointF Middle, float StartDegrees, std::vector<GradientStop> Stops)
{
    /* Its own turn begins at +x; CSS's at the top. */
    Transform Turn = Transform::Rotate(StartDegrees - 90.0f);
    Turn.E         = Middle.X;
    Turn.F         = Middle.Y;
    return Gradient(Kind::Conic, Turn, std::move(Stops), core::Extend::Pad);
}

Gradient &Gradient::Transformed(const Transform &Within)
{
    Own = Own.Then(Within);
    return *this;
}

}  // namespace painter::core
