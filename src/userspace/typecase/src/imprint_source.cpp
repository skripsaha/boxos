/* imprint_source.cpp — an imprint as a source. */

#include "typecase/imprint_source.h"

#include <algorithm>
#include <cstddef>

namespace typecase {

bool ImprintSource::Has(char32_t Character) const
{
    return BoxImprintFind(&Of, static_cast<std::uint32_t>(Character)) < Of.Count;
}

std::optional<GlyphImage> ImprintSource::Draw(char32_t Character, const Size &At)
{
    const std::uint32_t Glyph = BoxImprintFind(&Of, static_cast<std::uint32_t>(Character));
    if (Glyph >= Of.Count || Of.SizeCount == 0 || At.Width <= 0 || At.Height <= 0) return std::nullopt;

    std::uint32_t Size = 0;
    for (std::uint32_t Each = 1; Each < Of.SizeCount; Each++)
        if (Of.Sizes[Each].Width <= At.Width && Of.Sizes[Each].Height <= At.Height) Size = Each;
    const BoxImprintSize &Cell = Of.Sizes[Size];

    GlyphImage Image;
    Image.Top     = -At.Baseline;
    Image.Width   = At.Width;
    Image.Height  = At.Height;
    Image.Advance = static_cast<float>(At.Width);
    Image.Coverage.assign(static_cast<std::size_t>(At.Width) * static_cast<std::size_t>(At.Height), 0);
    const std::int32_t Left = (At.Width - Cell.Width) / 2, Top = (At.Height - Cell.Height) / 2;
    for (std::int32_t Y = 0; Y < Cell.Height; Y++)
        for (std::int32_t X = 0; X < Cell.Width; X++) {
            if (Left + X < 0 || Left + X >= At.Width || Top + Y < 0 || Top + Y >= At.Height) continue;
            const std::uint32_t Shade = BoxImprintShade(&Of, Size, Glyph, static_cast<std::uint32_t>(X),
                                                        static_cast<std::uint32_t>(Y));
            Image.Coverage[static_cast<std::size_t>((Top + Y) * At.Width + Left + X)] =
                static_cast<std::uint8_t>(Shade * 255 / BOXOS_IMPRINT_INKED);
        }
    return Image;
}

}  // namespace typecase
