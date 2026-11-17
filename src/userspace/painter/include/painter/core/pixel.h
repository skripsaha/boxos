#ifndef PAINTER_CORE_PIXEL_H
#define PAINTER_CORE_PIXEL_H

#include <cstdint>

#include "painter/core/color.h"

namespace painter::core {

// A pixel of a canvas: how much red, green and blue light it gives, each
// already multiplied by how opaque it is, and how opaque it is — four numbers
// of sixteen bits, 0 to 65535, light on the scale of sRGB's own curve
// (BoxColorLight, boxos_color.h). Laid over another, a pixel adds its light to
// what it leaves of the other's: the one way the system lays a colour over
// another, with sixteen bits of room, so a dark gradient keeps its steps and a
// picture laid many times keeps its colours. Clear is all four 0.
struct Pixel {
    std::uint16_t Red   = 0;
    std::uint16_t Green = 0;
    std::uint16_t Blue  = 0;
    std::uint16_t Alpha = 0;

    // A colour as a pixel. The screen's own colour is the display's to draw,
    // never a canvas's: it is clear.
    static Pixel Of(Color From) noexcept;
    // The colour nearest the pixel: its light over its opacity, written back
    // as sRGB's numbers; a clear pixel is see-through.
    Color ToColor() const noexcept;

    friend constexpr bool operator==(Pixel, Pixel) noexcept = default;
};

}  // namespace painter::core

#endif  // PAINTER_CORE_PIXEL_H
