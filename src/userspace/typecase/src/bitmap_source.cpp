/* bitmap_source.cpp — a bitmap font as a source. */

#include "typecase/bitmap_source.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace typecase {

BitmapSource::BitmapSource(std::span<const std::uint8_t> Rows, std::int32_t Width, std::int32_t Height,
                           Indexer Index)
    : Rows(Rows), Width(std::clamp(Width, 1, 8)), Height(std::max(Height, 1)), Index(std::move(Index))
{
}

/* The glyph Index names for the character, if the rows hold it. */
std::optional<std::uint32_t> BitmapSource::GlyphOf(char32_t Character) const
{
    const auto Glyph = Index(Character);
    if (!Glyph || (std::size_t{*Glyph} + 1) * static_cast<std::size_t>(Height) > Rows.size()) return std::nullopt;
    return Glyph;
}

bool BitmapSource::Has(char32_t Character) const { return GlyphOf(Character).has_value(); }

std::optional<GlyphImage> BitmapSource::Draw(char32_t Character, const Size &At)
{
    const auto Glyph = GlyphOf(Character);
    if (!Glyph) return std::nullopt;

    GlyphImage Image;
    Image.Top     = -At.Baseline;
    Image.Width   = std::max(At.Width, 0);
    Image.Height  = std::max(At.Height, 0);
    Image.Advance = static_cast<float>(Image.Width);
    Image.Coverage.assign(static_cast<std::size_t>(Image.Width) * static_cast<std::size_t>(Image.Height), 0);

    const std::int32_t Times = std::max(1, std::min(Image.Width / Width, Image.Height / Height));
    const std::int32_t Left  = (Image.Width - Width * Times) / 2;
    const std::int32_t Top   = (Image.Height - Height * Times) / 2;
    const std::uint8_t *Of   = Rows.data() + std::size_t{*Glyph} * static_cast<std::size_t>(Height);
    for (std::int32_t Y = 0; Y < Image.Height; Y++) {
        const std::int32_t Row = (Y - Top) / Times;
        if (Y < Top || Row >= Height) continue;
        for (std::int32_t X = 0; X < Image.Width; X++) {
            const std::int32_t Column = (X - Left) / Times;
            if (X < Left || Column >= Width || !(Of[Row] & (0x80u >> Column))) continue;
            Image.Coverage[static_cast<std::size_t>(Y) * static_cast<std::size_t>(Image.Width) +
                           static_cast<std::size_t>(X)] = 255;
        }
    }
    return Image;
}

}  // namespace typecase
