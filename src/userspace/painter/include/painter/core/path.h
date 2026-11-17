#ifndef PAINTER_CORE_PATH_H
#define PAINTER_CORE_PATH_H

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "painter/core/geometry.h"

namespace painter::core {

// How round each corner of a rectangle is: the radius of its quarter circle,
// in the path's units. Where two corners of a side would overlap, every radius
// is shrunk alike until they meet, as CSS shrinks them.
struct Corners {
    float TopLeft = 0, TopRight = 0, BottomRight = 0, BottomLeft = 0;

    static constexpr Corners All(float Radius) noexcept { return {Radius, Radius, Radius, Radius}; }
};

// A shape's outline: contours of straight lines and curves, in its own units
// (pixels, until a Transform lays it elsewhere), and ready rectangles, rounded
// rectangles and ellipses kept as themselves — drawn as the brush draws them
// (boxos_shape.h), the same edges for a fill and a stroke. A contour wound the
// other way cuts a hole where the fill rule is NonZero.
class Path {
public:
    enum class Step : std::uint8_t {
        Move,      // a new contour at the next point
        Line,      // a straight line to the next point
        Quad,      // a curve pulled toward the next point, to the one after
        Cubic,     // a curve pulled by the next two points, to the third
        Close,     // a straight line back to the contour's first point
        Rect,      // a rectangle: its corner, then its width and height
        Rounded,   // a rounded rectangle: corner, size, then its Corners as two points
        Ellipse,   // an ellipse: its middle, then its two radii
    };

    Path &MoveTo(PointF To);
    Path &LineTo(PointF To);
    Path &QuadTo(PointF Pull, PointF To);
    Path &CubicTo(PointF Pull1, PointF Pull2, PointF To);
    // SVG's arc: along an ellipse of RadiusX and RadiusY, turned by
    // TurnDegrees, to To — the Large way round or not, Clockwise or not; kept
    // as cubic curves, a quarter turn each at most.
    Path &ArcTo(float RadiusX, float RadiusY, float TurnDegrees, bool Large, bool Clockwise, PointF To);
    Path &Close();

    Path &AddRect(RectF Area);
    Path &AddRoundedRect(RectF Area, Corners Round);
    Path &AddEllipse(PointF Middle, float RadiusX, float RadiusY);
    Path &AddCircle(PointF Middle, float Radius) { return AddEllipse(Middle, Radius, Radius); }

    static Path Rect(RectF Area) { return Path().AddRect(Area); }
    static Path RoundedRect(RectF Area, Corners Round) { return Path().AddRoundedRect(Area, Round); }
    static Path Ellipse(PointF Middle, float RadiusX, float RadiusY) { return Path().AddEllipse(Middle, RadiusX, RadiusY); }
    static Path Circle(PointF Middle, float Radius) { return Path().AddEllipse(Middle, Radius, Radius); }
    static Path Line(PointF From, PointF To) { return Path().MoveTo(From).LineTo(To); }

    // SVG's path data — M L H V C S Q T A Z, each also relative — read into a
    // path; none when any of it is not path data, so a broken icon is seen to
    // be broken rather than drawn in part.
    static std::optional<Path> FromSvg(std::string_view Data);

    std::span<const Step>   Steps() const noexcept { return StepList; }
    std::span<const PointF> Points() const noexcept { return PointList; }
    bool                    Empty() const noexcept { return StepList.empty(); }
    // The least rectangle holding every point, controls and ready shapes too.
    RectF Bounds() const noexcept;

private:
    void Begin();   // a contour begun where the pen is, when none is open

    std::vector<Step>   StepList;
    std::vector<PointF> PointList;
    PointF              Start{};
    PointF              At{};
    bool                Open = false;
};

}  // namespace painter::core

#endif  // PAINTER_CORE_PATH_H
