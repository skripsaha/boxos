#ifndef PAINTER_TUI_SUBCELL_H
#define PAINTER_TUI_SUBCELL_H

/* A cell seen as four sub-pixels, two across and two down, and the choice of
 * the glyph and the two colours that show them best. The font draws a cell in
 * two colours in the shapes it has: a block or a space, the upper half, the
 * left half, and on pixels the shades. Nothing in it is a quarter, so a cell
 * of four colours is approximated, and ResolveQuad is the one place that says
 * how. FineCanvas (fine_canvas.h) draws on it. */

#include <array>
#include <cstddef>

#include "painter/core/color.h"
#include "painter/tui/frame.h"

namespace painter::tui {

/* The sub-pixels of a cell in reading order, by these places. */
using Quad = std::array<core::Color, 4>;

namespace QuadPlace {
inline constexpr std::size_t TopLeft     = 0;
inline constexpr std::size_t TopRight    = 1;
inline constexpr std::size_t BottomLeft  = 2;
inline constexpr std::size_t BottomRight = 3;
}  // namespace QuadPlace

/* The sub-pixels a cell shows: exactly for a space, the blocks and the halves,
 * the shade's mix of its two colours for a shade, and the background for any
 * other glyph, whose strokes four sub-pixels cannot hold. A colour of the
 * cell that is see-through is a sub-pixel see-through. */
Quad QuadOf(const Cell &Shown) noexcept;

/* The opaque cell whose picture is nearest to Wanted. The candidates are the
 * upper half (rows), the left half (columns), one colour, and where Glass is
 * pixels also the shades: a diagonal of two colours is the half shade, and one
 * sub-pixel against three is dust of its colour on the other. A shade is a
 * texture, so its error is raised by how far its two colours lie apart. The
 * candidate of least error is chosen, a split before one colour when equal.
 * A text page has sixteen colours and a dither there is only noise, so it has
 * no shades, and its colours are those of the sub-pixels, never their mixes.
 *
 * A sub-pixel may be see-through (Color::Through): a part of the picture no
 * shape reached. A cell with some such is never a shade, which would paint
 * them all; it is the split — upper half or left half — that leaves the most
 * of them see-through, a side of nothing but see-through sub-pixels being
 * see-through whole, and the colour of a side the mean of its real ones. A
 * lone see-through sub-pixel among three real ones is given their colour. */
Cell ResolveQuad(const Quad &Wanted, GlassKind Glass) noexcept;

}  // namespace painter::tui

#endif  // PAINTER_TUI_SUBCELL_H
