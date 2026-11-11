/* board_view.h — one player's waters as a picture: the sea, the grid, the
 * ships that are to be seen, and what the shots have left — rings where a
 * shell fell in the water, flames and smoke where a ship burns, a wreck
 * where it sank, dust round it where no ship can lie. The picture is in three
 * parts, as everything is: pixels, finer round shapes, and the words round
 * the grid. */
#ifndef SEABATTLE_BOARD_VIEW_H
#define SEABATTLE_BOARD_VIEW_H

#include <cstdint>
#include <string_view>

#include "painter/tui/fine_canvas.h"
#include "painter/tui/frame.h"

#include "board.h"
#include "canvas.h"
#include "layout.h"
#include "look.h"

namespace seabattle {

struct BoardScene {
    const Board         &Waters;
    GridView             At;
    const Look          &Shown;
    std::uint64_t        Ms = 0;
    bool                 Text = false;        // the glass is a text page
    bool                 ShowShips = false;   // the fleet is laid bare: one's own, or all of them at the end
    int                  Sinking = -1;        // the ship breaking up, and how far
    float                Sink = 0.0f;
    int                  HideRow = -1;        // a cell whose mark waits for its blast to end
    int                  HideCol = -1;
};

void PaintBoard(Canvas &Out, const BoardScene &Scene);
void ShapeBoard(painter::tui::FineCanvas &Out, const BoardScene &Scene);
// The frame, the letters over the columns and the numbers beside the rows, and the title over them.
// A column and a row given are marked as the one the cursor is on.
void LabelBoard(painter::tui::Frame &Out, const Look &Shown, const GridView &At, std::string_view Title, int Row = -1, int Col = -1);

}  // namespace seabattle

#endif
