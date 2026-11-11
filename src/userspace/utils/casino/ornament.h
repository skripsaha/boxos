/* ornament.h — what the casino draws with the font, not with pixels: thin
 * rules from the box glyphs, framed boxes with a shadow, the cloth of a table
 * with its weave and rail, and the bar of hints. Pictures stay in pixels
 * (canvas.h); everything that must be sharp is text. */
#ifndef CASINO_ORNAMENT_H
#define CASINO_ORNAMENT_H

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "canvas.h"
#include "look.h"
#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

namespace casino {

// A key and what it does, for the bar of hints.
struct Hint {
    std::string Key;
    std::string What;
};

// The bar on the last row: as many of the hints as fit, in order, then the
// two that are always there (the other table, and the keys).
void HintBar(painter::tui::Frame &Out, const Look &Shown, std::span<const Hint> Hints);

// A framed panel: a thin rule round it, the title set into the top edge.
void Panel(painter::tui::Frame &Out, painter::core::Rect Area, Color Edge, Color Fill, std::string_view Title, Color TitleInk);
// The same over what is under it, with a shadow to the right and below.
void Box(painter::tui::Frame &Out, painter::core::Rect Area, Color Edge, Color Fill, std::string_view Title, Color TitleInk);
// A rule across the panel or box at Row, joined to its edges.
void BoxRule(painter::tui::Frame &Out, painter::core::Rect Area, std::int32_t Row, Color Edge, Color Fill);
// The cloth: the felt color, lighter in the middle and darker toward the
// edges, over Area of the frame.
void Cloth(painter::tui::Frame &Out, painter::core::Rect Area, Color Felt);
// The weave: a fine grain over the cloth still bare after the pictures are
// drawn on it.
void Weave(painter::tui::Frame &Out, painter::core::Rect Area);
// A rail of wood round the edge of Area, with a thin inlay inside it when
// there is room for one.
void Rail(painter::tui::Frame &Out, painter::core::Rect Area, Color Wood, Color Inlay, bool WithInlay);

// Words written over what is there: each cell keeps its own background.
void Print(painter::tui::Frame &Out, painter::core::Point At, std::string_view Words, Color Ink);

// A thin rule of Length cells from At, across or down, over what is there.
void Rule(painter::tui::Frame &Out, painter::core::Point At, std::int32_t Length, bool Down, Color Ink);

// A cell where two colors meet, drawn with the glyph of the font whose picture
// is nearest to the shape: Mask holds a row of 8 pixels in each byte, the most
// significant bit the leftmost, a set bit where the pixel is In. Dither false
// keeps to the blocks and the halves above and below, where the 16 colors of
// the text console would turn the shades and thin strokes into noise.
void Settle(painter::tui::Frame &Out, painter::core::Point At, Color In, Color Outside, const std::array<std::uint8_t, 16> &Mask,
            bool Dither = true);

// The same for a shape given as Covers(X, Y), true where the point of the cell
// (X across from 0 to 8, Y down from 0 to 16, in the pixels of the glass) is In.
template <class Shape>
void FitCell(painter::tui::Frame &Out, painter::core::Point At, Color In, Color Outside, Shape Covers, bool Dither = true)
{
    std::array<std::uint8_t, 16> Mask{};
    for (int Y = 0; Y < 16; Y++)
        for (int X = 0; X < 8; X++)
            if (Covers(static_cast<float>(X) + 0.5f, static_cast<float>(Y) + 0.5f)) Mask[static_cast<std::size_t>(Y)] |= static_cast<std::uint8_t>(0x80 >> X);
    Settle(Out, At, In, Outside, Mask, Dither);
}

// How light a color is, 0 to 255.
std::int32_t Lightness(Color Ink);

}  // namespace casino

#endif
