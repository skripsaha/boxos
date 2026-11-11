/* panel.h — the result below the table: each group with its dice — the ones
 * that count lit, the highest and lowest faces in their colors, the ones that
 * do not in parentheses and quiet — its sum, the plain numbers, and the total,
 * in digits of pixels as tall as three cells when the panel has room. */
#ifndef DICE_PANEL_H
#define DICE_PANEL_H

#include <string>
#include <vector>

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

#include "look.h"
#include "roll.h"

namespace dice {

struct Run {
    std::string Text;
    Color       Ink;
};

using Line = std::vector<Run>;

struct Panel {
    std::vector<Line> Lines;
    int               Rows = 0;        // the rule on top, the lines, and room for the total
    bool              Big = false;     // the total in pixel digits, beside the lines
};

// The panel for Thrown on a glass Width wide, no more than MostRows high.
Panel Compose(const Roll &Thrown, int Width, int MostRows, const Look &Shown);

// The panel in Area; Revealed false while the dice are still tumbling.
void PaintPanel(painter::tui::Frame &Out, painter::core::Rect Area, const Panel &Of, const Roll &Thrown,
                bool Revealed, const Look &Shown);

}  // namespace dice

#endif
