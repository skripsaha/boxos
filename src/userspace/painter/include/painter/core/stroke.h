#ifndef PAINTER_CORE_STROKE_H
#define PAINTER_CORE_STROKE_H

#include <cstdint>
#include <vector>

namespace painter::core {

// How a line ends: where its path ends, half a circle past it, or half its
// width past it, square.
enum class LineCap : std::uint8_t { Butt, Round, Square };
// How a line turns a corner: its sides carried on until they meet (cut to a
// bevel past the miter limit), round, or cut straight across.
enum class LineJoin : std::uint8_t { Miter, Round, Bevel };
// Where a line lies against a closed shape's edge: across it, wholly within,
// or wholly without.
enum class StrokeAlign : std::uint8_t { Center, Inside, Outside };

// A line drawn along a path (boxos_stroke.h): Width wide in the path's units,
// solid or dashed — Dashes on, off, on…, an odd count gone round twice, begun
// DashOffset into the pattern at each contour; MiterLimit the longest a
// miter may be, in widths, before it is cut to a bevel.
struct Stroke {
    float              Width      = 1.0f;
    LineCap            Cap        = LineCap::Butt;
    LineJoin           Join       = LineJoin::Miter;
    float              MiterLimit = 4.0f;
    std::vector<float> Dashes;
    float              DashOffset = 0.0f;
    StrokeAlign        Align      = StrokeAlign::Center;
};

}  // namespace painter::core

#endif  // PAINTER_CORE_STROKE_H
