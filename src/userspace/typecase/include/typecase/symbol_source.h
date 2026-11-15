#ifndef TYPECASE_SYMBOL_SOURCE_H
#define TYPECASE_SYMBOL_SOURCE_H

/* BoxOS's own characters (boxos_symbols.h), worked out for the cell as
 * geometry is: Duppo, two cells wide — its one-colour mark (boxos_mascot.h)
 * drawn across a pair of cells, as large as the pair holds it, and cut in
 * two, the left half one character and the right half the other. Small, the
 * mark's line round the eye would be thinner than a pixel and the eye a
 * smudge: then the mark is drawn without it, the pupil and its spark alone;
 * and the paper round the medium bubble and the spark are never thinner
 * than a pixel. A source to ask before geometry: it draws nothing geometry
 * does. */

#include <optional>

#include "typecase/glyph_source.h"
#include "typecase/rasterizer.h"

namespace typecase {

class SymbolSource final : public GlyphSource {
public:
    bool                      Has(char32_t Character) const override;
    std::optional<GlyphImage> Draw(char32_t Character, const Size &At) override;

private:
    Rasterizer Pen;
};

}  // namespace typecase

#endif  // TYPECASE_SYMBOL_SOURCE_H
