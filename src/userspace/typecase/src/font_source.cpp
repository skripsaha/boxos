/* font_source.cpp — a TrueType font as a source. */

#include "typecase/font_source.h"

#include <cmath>
#include <utility>

namespace typecase {

FontSource::FontSource(Font Face) : Face(std::move(Face)) {}

bool FontSource::Has(char32_t Character) const
{
    const GlyphId Glyph = Face.Find(Character);
    return Glyph != 0 && Face.Readable(Glyph);
}

std::optional<GlyphImage> FontSource::Draw(char32_t Character, const Size &At)
{
    const GlyphId Glyph = Face.Find(Character);
    if (Glyph == 0 || !Face.Readable(Glyph)) return std::nullopt;
    const auto Shape = Face.OutlineOf(Glyph);
    if (!Shape) return std::nullopt;

    const float        Scale  = At.Em / Face.Metrics().UnitsPerEm;
    const std::int32_t Margin = static_cast<std::int32_t>(std::ceil(At.Em));
    const Bounds       Limit{-Margin, -At.Baseline - Margin, At.Width + Margin, At.Height - At.Baseline + Margin};
    return Pen.Draw(*Shape, Scale, Scale, Limit, static_cast<float>(Face.AdvanceOf(Glyph)) * Scale);
}

}  // namespace typecase
