#ifndef TYPECASE_GLYPH_READER_H
#define TYPECASE_GLYPH_READER_H

/* A glyph's outline out of glyf, found through loca: a simple glyph's
 * contours of points on and off the curve, or a composite's components,
 * each another glyph moved, scaled or turned into place. */

#include <cstdint>
#include <expected>

#include "bytes.h"
#include "typecase/font.h"

namespace typecase {

struct GlyphTables {
    Bytes         Loca;
    Bytes         Glyf;
    bool          LongOffsets = false;
    std::uint16_t Glyphs      = 0;
};

std::expected<Outline, Fault> ReadOutline(const GlyphTables &Tables, GlyphId Glyph);

}  // namespace typecase

#endif  // TYPECASE_GLYPH_READER_H
