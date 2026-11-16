#ifndef TYPECASE_GLYPH_SOURCE_H
#define TYPECASE_GLYPH_SOURCE_H

/* Something that draws characters: a font, a bitmap font, geometry worked
 * out for the cell, BoxOS's own symbols. A source says which characters it
 * has and draws those; for any other, and for one it has but cannot draw,
 * it draws nothing, and a chain asks the next. */

#include <optional>

#include "typecase/glyph_image.h"
#include "typecase/size.h"

namespace typecase {

class GlyphSource {
public:
    virtual ~GlyphSource() = default;

    virtual bool                      Has(char32_t Character) const      = 0;
    virtual std::optional<GlyphImage> Draw(char32_t Character, const Size &At) = 0;
};

}  // namespace typecase

#endif  // TYPECASE_GLYPH_SOURCE_H
