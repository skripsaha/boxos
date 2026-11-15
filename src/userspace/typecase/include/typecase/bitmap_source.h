#ifndef TYPECASE_BITMAP_SOURCE_H
#define TYPECASE_BITMAP_SOURCE_H

/* A bitmap font as a source: glyphs of rows up to eight pixels wide, a byte
 * to a row, the leftmost pixel in its high bit, glyph after glyph — the
 * shape of the fonts BoxOS carries in its programs, read by no reader that
 * can fail. Which glyph draws a character its owner says (Index), and the
 * rows stay the owner's, alive as long as the source. A glyph fills the
 * cell at the largest whole multiple of its size that fits, centred: never
 * smoothed, and never smaller than itself. */

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "typecase/glyph_source.h"

namespace typecase {

class BitmapSource final : public GlyphSource {
public:
    using Indexer = std::function<std::optional<std::uint32_t>(char32_t)>;

    BitmapSource(std::span<const std::uint8_t> Rows, std::int32_t Width, std::int32_t Height, Indexer Index);

    bool                      Has(char32_t Character) const override;
    std::optional<GlyphImage> Draw(char32_t Character, const Size &At) override;

private:
    std::optional<std::uint32_t> GlyphOf(char32_t Character) const;

    std::span<const std::uint8_t> Rows;
    std::int32_t                  Width;
    std::int32_t                  Height;
    Indexer                       Index;
};

}  // namespace typecase

#endif  // TYPECASE_BITMAP_SOURCE_H
