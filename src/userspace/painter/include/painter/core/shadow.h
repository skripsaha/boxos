#ifndef PAINTER_CORE_SHADOW_H
#define PAINTER_CORE_SHADOW_H

#include <cstdint>
#include <optional>

#include "painter/core/color.h"
#include "painter/core/geometry.h"
#include "painter/core/mask.h"
#include "painter/core/path.h"
#include "painter/core/transform.h"

namespace painter::core {

// The shadow a shape casts — outside it, as a thing raised above what lies
// beneath, or within it (Inner), as a thing pressed in: Offset away, grown by
// Spread (shrunk when it is less than none), its edge fading over Blur pixels
// on either side, in Color. As CSS's box-shadow, for any shape.
struct Shadow {
    PointF      Offset{};
    float       Blur   = 0.0f;
    float       Spread = 0.0f;
    core::Color Color  = core::Color::Rgba(0, 0, 0, 96);
    bool        Inner  = false;

    // The shadow's coverage for Shape laid by Place — made once, and kept
    // while the shape keeps its size, then laid any number of times
    // (Canvas::DrawShadow); none when the shape is refused.
    std::optional<Mask> MaskFor(const Path &Shape, const Transform &Place = {}) const;
    // How many whole pixels its blur reaches past the shape grown and moved:
    // what the shadow may cover beyond it.
    std::int32_t Reach() const noexcept;
};

}  // namespace painter::core

#endif  // PAINTER_CORE_SHADOW_H
