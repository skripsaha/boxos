#ifndef TYPECASE_CHARACTER_MAP_H
#define TYPECASE_CHARACTER_MAP_H

/* The character map (cmap): which glyph draws a character. A font carries
 * several subtables, each for a platform and an encoding; the one read is
 * the widest repertoire of Unicode whose structure holds — format 12 or 13
 * (any character), then 4 (the Basic Multilingual Plane), 6, 0, and a
 * symbol font's map last. */

#include <cstdint>
#include <expected>

#include "bytes.h"
#include "typecase/font.h"

namespace typecase {

struct MapChoice {
    std::uint32_t Offset = 0;      /* the subtable, from the cmap table's start */
    std::uint16_t Format = 0;
    bool          Symbol = false;  /* platform 3, encoding 0 */
};

/* The subtable to read, of the cmap table Map. */
std::expected<MapChoice, Fault> ChooseMap(Bytes Map);

/* The glyph a subtable of Format names for Character; 0 for none, or for one
 * past the font's Glyphs. Subtable runs from the subtable's start to the
 * end of the cmap table. */
GlyphId FindInMap(Bytes Subtable, std::uint16_t Format, char32_t Character, std::uint16_t Glyphs) noexcept;

}  // namespace typecase

#endif  // TYPECASE_CHARACTER_MAP_H
