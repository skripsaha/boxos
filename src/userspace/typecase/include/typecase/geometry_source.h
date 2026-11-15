#ifndef TYPECASE_GEOMETRY_SOURCE_H
#define TYPECASE_GEOMETRY_SOURCE_H

/* Characters worked out for the cell instead of drawn from a font: the
 * lines of frames (U+2500..U+257F), the blocks and their shades
 * (U+2580..U+259F), the mosaics of quadrants, sextants and octants
 * (boxos_mosaic.h), the wedges of Symbols for Legacy Computing
 * (U+1FB3C..U+1FB6F) and braille's dots (U+2800..U+28FF). A font draws them
 * to its own em and the next cell's line meets this one's wherever the em
 * puts it; drawn to the cell, a line leaves every cell at the same pixels,
 * so frames close and blocks tile whatever the cell's shape.
 *
 * A light line is a tenth of the cell's width, at least a pixel; a heavy
 * one twice that; a double one two light lines a light line apart. A
 * sub-cell's edge is the nearest pixel to its share of the cell, the same
 * for every character, so a mosaic and its complement fill the cell
 * exactly. Shades are patterns of a quarter, a half and three quarters of
 * the pixels. */

#include <optional>

#include "typecase/glyph_source.h"
#include "typecase/rasterizer.h"

namespace typecase {

class GeometrySource final : public GlyphSource {
public:
    bool                      Has(char32_t Character) const override;
    std::optional<GlyphImage> Draw(char32_t Character, const Size &At) override;

private:
    Rasterizer Pen;   /* the curves, slants and dots, smoothed */
};

}  // namespace typecase

#endif  // TYPECASE_GEOMETRY_SOURCE_H
