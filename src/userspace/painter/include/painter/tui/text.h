#ifndef PAINTER_TUI_TEXT_H
#define PAINTER_TUI_TEXT_H

/* Text as cells of the glass. A character is one UTF-8 sequence, or one byte
 * that is none, and takes one cell, which holds it; how it looks is the
 * display's to draw. A tab runs to the next stop of four, and a control or a
 * byte that is no character is U+FFFD, in OddInk. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "painter/core/color.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

namespace painter::tui {

inline constexpr std::size_t TabStop = 4;

/* The ink of a control, or of a byte that is no UTF-8: told from the text. */
inline constexpr core::Color OddInk = core::Color::Rgb(0xE0, 0x9A, 0x30);

/* One character as the glass shows it. */
struct Glyph {
    std::size_t Length    = 1;         /* its bytes */
    std::size_t Width     = 1;         /* its cells */
    char32_t    Character = U' ';      /* what each of them holds */
    bool        Odd       = false;     /* a control, or a byte that is no UTF-8: U+FFFD */
};

/* The character at Byte of Text, when it stands at Column. */
Glyph        GlyphAt(std::string_view Text, std::size_t Byte, std::size_t Column = 0) noexcept;
/* The cells Text takes on the glass. */
std::size_t  CellsOf(std::string_view Text) noexcept;
/* The bytes of Text that fit in Cells cells, whole characters only. */
std::size_t  BytesWithin(std::string_view Text, std::size_t Cells) noexcept;
/* Text in no more than Width cells: its end cut off for "..." when it is longer. */
std::string  Shortened(std::string_view Text, std::size_t Width);
/* Text from At, no further than Width cells; the cells it took. */
std::int32_t PutText(Frame &Out, core::Point At, std::string_view Text, std::int32_t Width, core::Color Ink,
                     core::Color Under);

}  // namespace painter::tui

#endif  // PAINTER_TUI_TEXT_H
