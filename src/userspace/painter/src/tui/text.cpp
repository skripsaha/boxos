/* text.cpp — text as cells of the glass. */

#include "painter/tui/text.h"

#include <cstdint>

#include "boxos_glyph_fold.h"

namespace painter::tui {

namespace {

/* The UTF-8 sequence at Bytes[At]: its length and its codepoint, or 0 when
 * the bytes there are no sequence — an overlong form, a surrogate, a
 * continuation with no lead, or one cut short. */
std::size_t Decode(std::string_view Bytes, std::size_t At, std::uint32_t &Codepoint) noexcept
{
    const auto Lead = static_cast<unsigned char>(Bytes[At]);
    if (Lead < 0x80) {
        Codepoint = Lead;
        return 1;
    }

    std::size_t   Need;
    std::uint32_t Value, Least;
    if (Lead >= 0xC2 && Lead <= 0xDF)      Need = 2, Value = Lead & 0x1Fu, Least = 0x80;
    else if (Lead >= 0xE0 && Lead <= 0xEF) Need = 3, Value = Lead & 0x0Fu, Least = 0x800;
    else if (Lead >= 0xF0 && Lead <= 0xF4) Need = 4, Value = Lead & 0x07u, Least = 0x10000;
    else return 0;
    if (Need > Bytes.size() - At) return 0;

    for (std::size_t Index = 1; Index < Need; Index++) {
        const auto Next = static_cast<unsigned char>(Bytes[At + Index]);
        if ((Next & 0xC0u) != 0x80u) return 0;
        Value = Value << 6 | (Next & 0x3Fu);
    }
    if (Value < Least || Value > 0x10FFFF || (Value >= 0xD800 && Value <= 0xDFFF)) return 0;
    Codepoint = Value;
    return Need;
}

}  // namespace

Glyph GlyphAt(std::string_view Text, std::size_t Byte, std::size_t Column) noexcept
{
    Glyph Out;
    if (Text[Byte] == '\t') {
        Out.Width = TabStop - Column % TabStop;
        return Out;
    }

    std::uint32_t     Codepoint = 0;
    const std::size_t Length    = Decode(Text, Byte, Codepoint);
    if (Length == 0 || Codepoint < 0x20 || Codepoint == 0x7F) {
        Out.Cells[0] = BOXOS_GLYPH_FOLD_BOX;
        Out.Odd      = true;
        return Out;
    }
    Out.Length = Length;
    Out.Width  = static_cast<std::size_t>(BoxGlyphFoldCodepoint(Codepoint, Out.Cells));
    return Out;
}

std::size_t CellsOf(std::string_view Text) noexcept
{
    std::size_t Cells = 0;
    for (std::size_t Byte = 0; Byte < Text.size();) {
        const Glyph Shape = GlyphAt(Text, Byte, Cells);
        Cells += Shape.Width;
        Byte += Shape.Length;
    }
    return Cells;
}

std::size_t BytesWithin(std::string_view Text, std::size_t Cells) noexcept
{
    std::size_t Used = 0, Byte = 0;
    while (Byte < Text.size()) {
        const Glyph Shape = GlyphAt(Text, Byte, Used);
        if (Used + Shape.Width > Cells) break;
        Used += Shape.Width;
        Byte += Shape.Length;
    }
    return Byte;
}

std::string Shortened(std::string_view Text, std::size_t Width)
{
    if (CellsOf(Text) <= Width) return std::string(Text);
    if (Width <= 3) return std::string(Width, '.');
    return std::string(Text.substr(0, BytesWithin(Text, Width - 3))) + "...";
}

std::int32_t PutText(Frame &Out, core::Point At, std::string_view Text, std::int32_t Width, core::Color Ink,
                     core::Color Under)
{
    std::int32_t Col = 0;
    for (std::size_t Byte = 0; Byte < Text.size();) {
        const Glyph Shape = GlyphAt(Text, Byte, static_cast<std::size_t>(Col));
        if (Col + static_cast<std::int32_t>(Shape.Width) > Width) break;
        const bool Blank = Text[Byte] == '\t';
        for (std::size_t Part = 0; Part < Shape.Width; Part++)
            (void)Out.Put({At.X + Col++, At.Y},
                          Cell{Blank ? ' ' : Shape.Cells[Part], Shape.Odd ? OddInk : Ink, Under});
        Byte += Shape.Length;
    }
    return Col;
}

}  // namespace painter::tui
