#ifndef TYPECASE_FONT_SOURCE_H
#define TYPECASE_FONT_SOURCE_H

/* A TrueType font as a source: the characters its map names to a glyph
 * that reads, drawn to the size's em with the pen at the cell's baseline as
 * text draws them (glyph_look.h), the height of its lowercase a whole
 * number of pixels. */

#include <optional>

#include "typecase/font.h"
#include "typecase/glyph_source.h"
#include "typecase/rasterizer.h"

namespace typecase {

class FontSource final : public GlyphSource {
public:
    explicit FontSource(Font Face);

    const Font &Of() const noexcept { return Face; }

    bool                      Has(char32_t Character) const override;
    std::optional<GlyphImage> Draw(char32_t Character, const Size &At) override;

private:
    Font       Face;
    Rasterizer Pen;
};

}  // namespace typecase

#endif  // TYPECASE_FONT_SOURCE_H
