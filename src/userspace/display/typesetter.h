#ifndef BOXOS_DISPLAY_TYPESETTER_H
#define BOXOS_DISPLAY_TYPESETTER_H

#include <cstdint>
#include <optional>

#include "typecase/chain.h"
#include "typecase/font.h"
#include "typecase/glyph_cache.h"
#include "typecase/glyph_image.h"
#include "typecase/size.h"

// The typesetter — what each character looks like in a cell of a
// framebuffer: Typecase's sources in the order a screen of cells asks them —
// BoxOS's own symbols and geometry first, so Duppo's halves and a frame's
// line meet the next cell's exactly whatever the font, then the person's
// font, then the imprint the hands that cannot read a font draw with
// (boxos_imprint.h), and a character none of them draws shown as its code —
// and the pictures kept for when they are asked again. It reads no file and
// touches no pixel: the display gives it a font already read, and the cells
// are drawn with its pictures (cells_draw.h).
class Typesetter {
public:
    // Type for cells Height pixels tall: Face's, the cell as wide as its
    // proportions make it; with no face, the imprint's, the cell of its
    // largest size no taller than Height.
    Typesetter(std::optional<typecase::Font> Face, std::int32_t Height);

    Typesetter(const Typesetter &) = delete;
    Typesetter &operator=(const Typesetter &) = delete;

    const typecase::Size &Cell() const noexcept { return At; }

    // The character's picture against the cell's baseline: none for 0, a
    // cell that shows nothing. It holds until the next Glyph.
    const typecase::GlyphImage &Glyph(char32_t Character) noexcept;

    // The grids of mosaics a cell draws exactly (box/items.h ITEM_MOSAIC_*).
    std::uint8_t Mosaics() const noexcept { return Grids; }

private:
    typecase::Chain      Sources;
    typecase::Size       At;
    typecase::GlyphCache Kept;
    std::uint8_t         Grids = 0;
};

#endif
