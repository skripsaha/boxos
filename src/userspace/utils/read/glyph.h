#ifndef READ_GLYPH_H
#define READ_GLYPH_H

/* Text as cells: a character of UTF-8 is one cell, or two the console font
 * folds it into (boxos_glyph_fold.h), and a byte that is no character is a
 * box. A text read holds no tab by the time it is drawn: the book turns tabs
 * into blanks as it reads them. */

#include <cstddef>
#include <string_view>

namespace read {

struct Glyph {
    std::size_t Length = 1;          /* its bytes */
    std::size_t Width  = 1;          /* its cells */
    char        Cells[2] = {' ', ' '};
};

Glyph       GlyphAt(std::string_view Text, std::size_t Byte) noexcept;
std::size_t CellsOf(std::string_view Text) noexcept;
/* The bytes of Text that fit in Cells cells, whole characters only. */
std::size_t BytesWithin(std::string_view Text, std::size_t Cells) noexcept;

}  // namespace read

#endif
