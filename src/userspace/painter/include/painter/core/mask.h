#ifndef PAINTER_CORE_MASK_H
#define PAINTER_CORE_MASK_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "painter/core/fill.h"
#include "painter/core/geometry.h"
#include "painter/core/path.h"
#include "painter/core/transform.h"

namespace painter::core {

// How much of each pixel of a rectangle something covers, 0 to 255 — a
// glyph, a shadow, a clip: made once, kept, and laid with any fill any number
// of times (Canvas::FillMask).
class Mask {
public:
    Mask() = default;
    // Area, covering nothing yet.
    explicit Mask(Rect Area);

    // A shape's coverage, as a canvas covers it, within Within; none when the
    // shape is refused (past the reach of a shape, or a transform stretching
    // too far).
    static std::optional<Mask> Of(const Path &Shape, const Transform &Place, FillRule Rule, Rect Within);

    Rect Area() const noexcept { return Box; }
    bool Empty() const noexcept { return Box.Empty(); }
    // The coverage of pixel X, Y; none outside the area.
    std::uint8_t At(std::int32_t X, std::int32_t Y) const noexcept;
    // Row Y of the area, Area().Width values from its left; Y within it.
    std::uint8_t       *Row(std::int32_t Y) noexcept { return &Values[Index(Y)]; }
    const std::uint8_t *Row(std::int32_t Y) const noexcept { return &Values[Index(Y)]; }

    // Blurred as a shadow is: by a Gaussian whose σ is half of Radius — the
    // edge fading over Radius pixels on either side, as CSS blurs a shadow —
    // the area grown by how far the blur reaches.
    void Blur(float Radius);
    // The same coverage By pixels away.
    Mask Moved(Point By) const;

private:
    std::size_t Index(std::int32_t Y) const noexcept
    {
        return static_cast<std::size_t>(Y - Box.Y) * static_cast<std::size_t>(Box.Width);
    }

    Rect                      Box;
    std::vector<std::uint8_t> Values;
};

}  // namespace painter::core

#endif  // PAINTER_CORE_MASK_H
