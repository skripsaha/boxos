#ifndef PAINTER_CORE_TEXT_H
#define PAINTER_CORE_TEXT_H

// Text drawn: a text Typecase laid out (typecase/layout.h) filled on a canvas
// (Canvas::FillText), or taken as a shape — its glyphs' outlines and the lines
// under and through it — to stroke, to cast a shadow, to clip by: anything a
// Path is drawn with.

#include <cstdint>
#include <optional>

#include "painter/core/geometry.h"
#include "painter/core/path.h"
#include "typecase/layout.h"

namespace painter::core {

// The text's shape, its layout's top left at At: every glyph's outline as it
// is drawn (typecase/glyph_look.h) — a character no font has as the frame of
// its box — and the lines under and through it; of all of it, or only of what
// is drawn in Ink.
Path TextPath(const typecase::Layout &Text, PointF At, std::optional<std::uint32_t> Ink = std::nullopt);

}  // namespace painter::core

#endif  // PAINTER_CORE_TEXT_H
