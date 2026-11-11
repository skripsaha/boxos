/* hud.h — the words and lines round the picture, in the glass's cells: the bar
 * at the top, the keys at the bottom, frames of box rules, panels with a
 * title in their edge, and the fade that darkens whatever is on the glass. */
#ifndef SEABATTLE_HUD_H
#define SEABATTLE_HUD_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

#include "look.h"

namespace seabattle {

// A key and what it does, as the bottom line has them. Rank is how soon it is dropped when
// the line is too long: the lowest rank stays longest.
struct Hint {
    std::string Key;
    std::string Does;
    int         Rank = 0;
    bool        Urgent = false;
};

// The columns on the right of the top row that the display's clock and load meter cover.
inline constexpr int OverlayCols = 46;

// A line of Text no wider than Width cells, broken between words.
std::vector<std::string> Wrapped(std::string_view Text, std::size_t Width);

// The top row: Left in the name's colour, then Note quietly.
void PaintBar(painter::tui::Frame &Out, const Look &Shown, std::string_view Left, std::string_view Note);
// The bottom row: the hints that fit, and Right at its end.
void PaintHints(painter::tui::Frame &Out, const Look &Shown, const std::vector<Hint> &Hints, std::string_view Right = {});
// A line of words on the row above the hints, centred.
void PaintSaid(painter::tui::Frame &Out, std::int32_t Row, std::string_view Text, Color Ink, Color Under);

// A frame of single rules round Cells, drawn in the cells' own edge.
void PaintFrame(painter::tui::Frame &Out, painter::core::Rect Cells, Color Rule, Color Under);
// A panel over Area: its paper, a frame of double rules, Title in the top edge and Foot in the bottom, a
// shadow beneath. What is inside the frame is returned.
painter::core::Rect PaintPanel(painter::tui::Frame &Out, const Look &Shown, painter::core::Rect Area, std::string_view Title,
                               std::string_view Foot);
// Text centred across Width cells from Left on a row.
void PutCentred(painter::tui::Frame &Out, std::int32_t Left, std::int32_t Width, std::int32_t Row, std::string_view Text,
                Color Ink, Color Under);
// Every cell's colours kept by a share of 256: the glass darkens towards black.
void Darken(painter::tui::Frame &Out, int Share);

}  // namespace seabattle

#endif
