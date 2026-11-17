#ifndef PAINTER_TUI_MOSAIC_H
#define PAINTER_TUI_MOSAIC_H

/* A cell seen as sub-pixels, two across and two, three or four down, and the
 * choice of the glyph and the two colours that show them best. A glass that
 * draws a grid exactly (box/display.h DisplayMosaic, the characters of
 * boxos_mosaic.h) shows any two colours on it as one character, and nothing
 * is approximated. A glass that does not has, of the shapes a cell can take,
 * a block or a space, the upper half, the left half, and on pixels the
 * shades; nothing in it is a quarter, so a cell of four colours is
 * approximated, and ResolveMosaic is the one place that says how. FineCanvas
 * (fine_canvas.h) draws on it. */

#include <array>
#include <cstddef>
#include <cstdint>

#include "painter/core/color.h"
#include "painter/tui/frame.h"

namespace painter::tui {

/* The sub-pixels of a cell in reading order from the top left: two across, so
 * the place of a sub-pixel is its row times two and its column. */
struct Mosaic {
    std::uint8_t               Rows = 2;   /* 2, 3 or 4 */
    std::array<core::Color, 8> At{};

    std::size_t Count() const noexcept { return static_cast<std::size_t>(Rows) * 2; }
};

/* The sub-pixels a cell shows on a grid of Rows: for a space, a block, a
 * half and any other mosaic character, each sub-pixel the foreground laid
 * over the background by the share of its area the character fills
 * (Color::Over); the shade's two colours laid so for a shade; and the
 * background for any other glyph, whose strokes sub-pixels cannot hold. A
 * colour of the cell that is see-through or translucent is so in its
 * sub-pixels. */
Mosaic MosaicOf(const Cell &Shown, std::uint8_t Rows) noexcept;

/* The opaque cell whose picture is nearest to Wanted, on a glass that draws
 * the grids Mosaics says (DisplayMosaic flags).
 *
 * Where Glass is pixels and it draws the grid of Wanted.Rows, the cell is
 * exact: the two colours and the character that split the sub-pixels as
 * the least error says, every split being a character. The colour at the last
 * sub-pixel is always the background, so a split and its complement, one
 * picture, are one answer, and a cell that one colour shows as well is
 * the space.
 *
 * Otherwise the picture is of two rows, a grid of more being first brought to
 * two by the means of its sub-pixels' light over the area each covers. The
 * candidates are the upper half (rows), the left half (columns), one colour,
 * and where Glass is pixels also the shades: a diagonal of two colours is the
 * half shade, and one sub-pixel against three is dust of its colour on the
 * other.
 * A shade is a texture, so its error is raised by how far its two colours lie
 * apart. The candidate of least error is chosen, a split before one colour
 * when equal. A text page has sixteen colours and a dither there is only
 * noise, so it has no shades, and its colours are those of the sub-pixels,
 * never their mixes.
 *
 * A sub-pixel may have an opacity (Color::Opacity, none being Color::Through):
 * a part of the picture a shape reached by some share, or none reached. The
 * opacity is part of what a candidate is measured by, beside the colour, as
 * far as both are seen; a side is drawn in the mean of its colours' light
 * weighted by their opacities — sub-pixels seen together are seen as their
 * light — at the mean of the opacities, and a side nothing seen lies
 * on is see-through whole. A cell with any such sub-pixel is never a shade,
 * which would paint them all, so it is one of the splits or one colour. The
 * display blends what is not whole with what lies beneath the cell. */
Cell ResolveMosaic(const Mosaic &Wanted, GlassKind Glass, std::uint8_t Mosaics) noexcept;

}  // namespace painter::tui

#endif  // PAINTER_TUI_MOSAIC_H
