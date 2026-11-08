#ifndef KIN_GLYPH_H
#define KIN_GLYPH_H

/* Text as cells of the glass. A character is one UTF-8 sequence, or one byte
 * that is none; the console font draws it in one cell, or in two it folds it
 * into (boxos_glyph_fold.h). A tab runs to the next stop of four, and a
 * control or a byte that is no character is drawn as a box. */

#include <cstddef>
#include <string_view>

namespace kin {

inline constexpr std::size_t TabStop = 4;

/* One character as the glass shows it. */
struct Glyph {
    std::size_t Length = 1;            /* its bytes */
    std::size_t Width  = 1;            /* its cells */
    char        Cells[2] = {' ', ' '};
    bool        Odd = false;           /* a control, or a byte that is no UTF-8: drawn as a box */
};

/* The character at Byte of Text, when it stands at Column. */
Glyph       GlyphAt(std::string_view Text, std::size_t Byte, std::size_t Column = 0) noexcept;
/* The cells Text takes on the glass. */
std::size_t CellsOf(std::string_view Text) noexcept;
/* The bytes of Text that fit in Cells cells, whole characters only. */
std::size_t BytesWithin(std::string_view Text, std::size_t Cells) noexcept;

}  // namespace kin

#endif
