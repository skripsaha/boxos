/* ornament.h — what the casino draws with the font, not with pixels: thin
 * rules from the box glyphs, framed boxes with a shadow, the cloth of a table
 * with its weave and rail, and the bar of hints. Pictures stay in pixels
 * (canvas.h); everything that must be sharp is text. */
#ifndef CASINO_ORNAMENT_H
#define CASINO_ORNAMENT_H

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

// How light a color is, 0 to 255.
std::int32_t Lightness(Color Ink);

}  // namespace casino

#endif
