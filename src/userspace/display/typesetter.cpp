/*
 * typesetter.cpp — what each character looks like in a cell of a
 * framebuffer. See typesetter.h.
 */

#include "typesetter.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include "box/types.h"
#include "box/display.h"
#include "boxos_font.h"
#include "boxos_glyph_fold.h"
#include "boxos_mosaic.h"
#include "typecase/bitmap_source.h"
#include "typecase/font_source.h"
#include "typecase/geometry_source.h"
#include "typecase/symbol_source.h"

namespace {

// What the pictures kept may hold: a screenful of different characters many
// times over — a cell of 12 by 27 is some 330 bytes a picture.
constexpr std::size_t KeptBytes = std::size_t{1} << 20;

const typecase::GlyphImage Nothing{};

// The cell of the 8x16 font's type: half as wide as it is tall, its baseline
// a quarter of the height up from the foot, where sixteen rows put it.
typecase::Size BitmapCell(std::int32_t Height)
{
    return typecase::Size{0, std::max(Height / 2, 1), Height, Height * 3 / 4};
}

// The glyph of the 8x16 font that draws Character as itself, by the fold
// every hand that draws with it shares (boxos_glyph_fold.h): none for a
// character it would show as the box in its place, nor for 0.
std::optional<std::uint32_t> FoldedGlyph(char32_t Character)
{
    if (Character == 0) return std::nullopt;
    const unsigned char Glyph = BoxGlyphFoldOne(Character);
    if (Glyph == static_cast<unsigned char>(BOXOS_GLYPH_FOLD_BOX) && Character != U'■') return std::nullopt;
    return Glyph;
}

// The grids every pattern of which Geometry draws: to the cell, exactly.
std::uint8_t GridsOf(const typecase::GeometrySource &Geometry)
{
    const auto Whole = [&](std::span<const std::uint32_t> Patterns) {
        return std::all_of(Patterns.begin(), Patterns.end(), [&](std::uint32_t Character) {
            return Character == U' ' || Geometry.Has(static_cast<char32_t>(Character));
        });
    };
    return static_cast<std::uint8_t>((Whole(BoxMosaicQuadrants) ? DISPLAY_MOSAIC_QUADRANTS : 0) |
                                     (Whole(BoxMosaicSextants) ? DISPLAY_MOSAIC_SEXTANTS : 0) |
                                     (Whole(BoxMosaicOctants) ? DISPLAY_MOSAIC_OCTANTS : 0));
}

}  // namespace

Typesetter::Typesetter(std::optional<typecase::Font> Face, std::int32_t Height)
    : At(Face ? typecase::FitCell(*Face, Height) : BitmapCell(Height)), Kept(Sources, KeptBytes)
{
    auto Geometry = std::make_unique<typecase::GeometrySource>();
    Grids         = GridsOf(*Geometry);
    Sources.Add(std::make_unique<typecase::SymbolSource>());
    Sources.Add(std::move(Geometry));
    if (Face) Sources.Add(std::make_unique<typecase::FontSource>(std::move(*Face)));
    Sources.Add(std::make_unique<typecase::BitmapSource>(
        std::span<const std::uint8_t>(&BoxFontGlyphs[0][0], sizeof(BoxFontGlyphs)), BOXOS_FONT_WIDTH,
        BOXOS_FONT_HEIGHT, FoldedGlyph));
}

const typecase::GlyphImage &Typesetter::Glyph(char32_t Character) noexcept
{
    if (Character == 0) return Nothing;
    // No room for its picture: what is kept is let go and the picture asked
    // once more with the room that gives; with none even then, the cell shows
    // nothing rather than the display falling.
    try {
        return Kept.Get(Character, At);
    } catch (...) {
        Kept.Clear();
    }
    try {
        return Kept.Get(Character, At);
    } catch (...) {
        return Nothing;
    }
}
