#ifndef PAINTER_CORE_TRANSFORM_H
#define PAINTER_CORE_TRANSFORM_H

#include <optional>

#include "painter/core/geometry.h"

namespace painter::core {

// Where a place of a shape's own space lands, in pixels — SVG's matrix(a b c
// d e f):
//     x' = A·x + C·y + E        y' = B·x + D·y + F
// Moved, stretched, turned and slanted, one after another with Then.
struct Transform {
    float A = 1, B = 0, C = 0, D = 1, E = 0, F = 0;

    static constexpr Transform Identity() noexcept { return {}; }
    static constexpr Transform Translate(float X, float Y) noexcept { return {1, 0, 0, 1, X, Y}; }
    static constexpr Transform Scale(float X, float Y) noexcept { return {X, 0, 0, Y, 0, 0}; }
    static constexpr Transform Scale(float Both) noexcept { return {Both, 0, 0, Both, 0, 0}; }
    // Turned by Degrees about 0, 0, clockwise on the screen (y runs down); a
    // whole number of quarter turns turns exactly.
    static Transform Rotate(float Degrees) noexcept;
    // Slanted: x by the tangent of XDegrees of y, y by that of YDegrees of x.
    static Transform Skew(float XDegrees, float YDegrees) noexcept;

    // This, and then Next.
    Transform Then(const Transform &Next) const noexcept;
    PointF    Apply(PointF Place) const noexcept;
    float     Determinant() const noexcept { return A * D - B * C; }
    // The transform that undoes this one; none when it folds the plane flat.
    std::optional<Transform> Inverse() const noexcept;

    friend constexpr bool operator==(const Transform &, const Transform &) noexcept = default;
};

}  // namespace painter::core

#endif  // PAINTER_CORE_TRANSFORM_H
