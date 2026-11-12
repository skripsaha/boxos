#ifndef PAINTER_CORE_COVERAGE_H
#define PAINTER_CORE_COVERAGE_H

#include <algorithm>

namespace painter::core {

// How much of a pixel, a unit square, a shape covers, for a pixel whose middle
// lies Beyond the shape's edge, in pixels, seen as a straight line: negative
// within the shape, 0 on the edge. It is the share a program gives a pixel as
// its opacity (Color::WithOpacity) to make the shape's edge soft.

// The share of the pixel inside the shape.
inline float InsideShare(float Beyond) noexcept
{
    return std::clamp(0.5f - Beyond, 0.0f, 1.0f);
}

// The share of the pixel inside a band that lies between Near and Far beyond
// the edge: the line drawn round a shape is the band from 0 to its width.
inline float BandShare(float Beyond, float Near, float Far) noexcept
{
    return std::clamp(std::min(Beyond + 0.5f, Far) - std::max(Beyond - 0.5f, Near), 0.0f, 1.0f);
}

}  // namespace painter::core

#endif  // PAINTER_CORE_COVERAGE_H
