#ifndef TYPECASE_IMPRINT_SOURCE_H
#define TYPECASE_IMPRINT_SOURCE_H

/* An imprint as a source (boxos_imprint.h): its characters as tools/imprint
 * drew them, at the largest of its sizes whose cell the cell asked for holds,
 * in the middle of that cell — never smoothed again and never smaller than a
 * size it has. What a hand that cannot read a font draws with, and the
 * rescue of one that can when its font will not read. The imprint stays its
 * owner's, alive as long as the source. */

#include <cstdint>
#include <optional>

#include "boxos_imprint.h"
#include "typecase/glyph_source.h"

namespace typecase {

class ImprintSource final : public GlyphSource {
public:
    explicit ImprintSource(const BoxImprint &Of) : Of(Of) {}

    bool                      Has(char32_t Character) const override;
    std::optional<GlyphImage> Draw(char32_t Character, const Size &At) override;

private:
    const BoxImprint &Of;
};

}  // namespace typecase

#endif  // TYPECASE_IMPRINT_SOURCE_H
