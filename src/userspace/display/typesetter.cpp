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
#include "box/items.h"
#include "boxos_imprint_data.h"
#include "boxos_mosaic.h"
#include "typecase/font_source.h"
#include "typecase/geometry_source.h"
#include "typecase/imprint_source.h"
#include "typecase/symbol_source.h"

namespace {

// What the pictures kept may hold: a screenful of different characters many
// times over — a cell of 12 by 27 is some 330 bytes a picture.
constexpr std::size_t KeptBytes = std::size_t{1} << 20;

const typecase::GlyphImage Nothing{};

// The cell of the imprint's type: its largest size no taller than Height,
// the smallest when even that is taller.
typecase::Size ImprintCell(std::int32_t Height)
{
    const BoxImprintSize &Cell =
        BoxSystemImprint.Sizes[BoxImprintSizeFor(&BoxSystemImprint, static_cast<std::uint32_t>(std::max(Height, 0)))];
    return typecase::Size{0, Cell.Width, Cell.Height, Cell.Baseline};
}

// The grids every pattern of which Geometry draws: to the cell, exactly.
std::uint8_t GridsOf(const typecase::GeometrySource &Geometry)
{
    const auto Whole = [&](std::span<const std::uint32_t> Patterns) {
        return std::all_of(Patterns.begin(), Patterns.end(), [&](std::uint32_t Character) {
            return Character == U' ' || Geometry.Has(static_cast<char32_t>(Character));
        });
    };
    return static_cast<std::uint8_t>((Whole(BoxMosaicQuadrants) ? ITEM_MOSAIC_QUADRANTS : 0) |
                                     (Whole(BoxMosaicSextants) ? ITEM_MOSAIC_SEXTANTS : 0) |
                                     (Whole(BoxMosaicOctants) ? ITEM_MOSAIC_OCTANTS : 0));
}

}  // namespace

Typesetter::Typesetter(std::optional<typecase::Font> Face, std::int32_t Height)
    : At(Face ? typecase::FitCell(*Face, Height) : ImprintCell(Height)), Kept(KeptBytes)
{
    auto Geometry = std::make_unique<typecase::GeometrySource>();
    Grids         = GridsOf(*Geometry);
    Sources.Add(std::make_unique<typecase::SymbolSource>());
    Sources.Add(std::move(Geometry));
    if (Face) Sources.Add(std::make_unique<typecase::FontSource>(std::move(*Face)));
    Sources.Add(std::make_unique<typecase::ImprintSource>(BoxSystemImprint));
}

const typecase::GlyphImage &Typesetter::Glyph(char32_t Character) noexcept
{
    if (Character == 0) return Nothing;
    // No room for its picture: what is kept is let go and the picture asked
    // once more with the room that gives; with none even then, the cell shows
    // nothing rather than the display falling.
    try {
        return Kept.Get(Sources, Character, At);
    } catch (...) {
        Kept.Clear();
    }
    try {
        return Kept.Get(Sources, Character, At);
    } catch (...) {
        return Nothing;
    }
}
