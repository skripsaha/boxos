/* font_source.cpp — a TrueType font as a source. */

#include "typecase/font_source.h"

#include <cmath>
#include <utility>

namespace typecase {

FontSource::FontSource(Font Face) : Face(std::move(Face)) {}

/* Glyphs are not hinted: an outline lands on the pixels where its design
 * puts it, and a flat stroke at the height of the lowercase falls across two
 * rows as two grey ones. So the type is set a hair taller or shorter — the
 * lowercase a whole number of pixels high over a baseline that is a row's
 * edge — and the tops of a, n, x, z are one sharp edge. Across, the scale is
 * kept, and with it every advance. */
float FontSource::RiseAt(float Scale) const noexcept
{
    const float Lowercase = Face.Metrics().XHeight;
    if (Lowercase <= 0) return Scale;
    const float Whole = std::round(Lowercase * Scale);
    return Whole >= 1 ? Whole / Lowercase : Scale;
}

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
    return Pen.Draw(*Shape, Scale, RiseAt(Scale), Limit, static_cast<float>(Face.AdvanceOf(Glyph)) * Scale);
}

}  // namespace typecase
