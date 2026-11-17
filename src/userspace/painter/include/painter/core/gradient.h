#ifndef PAINTER_CORE_GRADIENT_H
#define PAINTER_CORE_GRADIENT_H

#include <cstdint>
#include <vector>

#include "painter/core/color.h"
#include "painter/core/geometry.h"
#include "painter/core/transform.h"

namespace painter::core {

// What lies past a gradient's ends, or past a picture's edge: its end colours
// or edge pixels held on (Pad), begun again (Repeat), run back (Reflect), or
// nothing (None).
enum class Extend : std::uint8_t { Pad, Repeat, Reflect, None };

// A colour Offset of the way along a gradient, 0 at its start, 1 at its end.
struct GradientStop {
    float Offset = 0.0f;
    core::Color Color;
};

// Colours along a line, out from a middle, or round it — the colours between
// two stops walked straight in their numbers (Color::Between), worked in
// sixteen bits so a wide, dark gradient keeps no bands. Stops are kept in the
// order of their offsets, each within 0 to 1.
class Gradient {
public:
    enum class Kind : std::uint8_t { Linear, Radial, Conic };

    // From From to To, square across the line between them.
    static Gradient Linear(PointF From, PointF To, std::vector<GradientStop> Stops, core::Extend Past = core::Extend::Pad);
    // Out from Middle to Radius; drawn from Focus — a place within the circle
    // — when it is not the middle.
    static Gradient Radial(PointF Middle, float Radius, std::vector<GradientStop> Stops,
                           core::Extend Past = core::Extend::Pad);
    static Gradient Radial(PointF Middle, float Radius, PointF Focus, std::vector<GradientStop> Stops,
                           core::Extend Past = core::Extend::Pad);
    // Round Middle, clockwise from StartDegrees — 0 straight up, as CSS's.
    static Gradient Conic(PointF Middle, float StartDegrees, std::vector<GradientStop> Stops);

    // Laid in a space of its own first: a radial gradient stretched one way is
    // an ellipse.
    Gradient &Transformed(const Transform &Within);

    core::Extend                     Past() const noexcept { return PastEnds; }
    Kind                             Of() const noexcept { return Shape; }
    const std::vector<GradientStop> &Stops() const noexcept { return StopList; }
    // The gradient's own space onto the path's: its start at 0, 0, its end
    // at 1, 0 (Linear) or its circle the unit circle (Radial, Conic).
    const Transform &Space() const noexcept { return Own; }
    // Radial: the focal point in its own space, within the unit circle.
    PointF Focus() const noexcept { return FocusAt; }

private:
    Gradient(Kind Shape, Transform Own, std::vector<GradientStop> Stops, core::Extend Past);

    Kind                      Shape;
    Transform                 Own;
    std::vector<GradientStop> StopList;
    core::Extend              PastEnds;
    PointF                    FocusAt{};
};

}  // namespace painter::core

#endif  // PAINTER_CORE_GRADIENT_H
