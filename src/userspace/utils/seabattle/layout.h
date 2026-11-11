/* layout.h — where each player's waters are on a glass of a given size: the
 * pixels of the grid and of its cells, and the cells of text round it. The
 * glass is 160 by 50 cells on a framebuffer and 80 by 25 on a text page, so
 * the cell of the grid is as large as the glass lets it be. */
#ifndef SEABATTLE_LAYOUT_H
#define SEABATTLE_LAYOUT_H

#include "painter/core/geometry.h"

namespace seabattle {

// A grid of Size by Size cells, each Cell pixels across, from pixel (X, Y). Y is always a
// whole row of text, two pixels to a row.
struct GridView {
    int X = 0;
    int Y = 0;
    int Cell = 2;
    int Size = 10;

    int  Pixels() const { return Cell * Size; }
    int  CentreX(int Col) const { return X + Col * Cell + Cell / 2; }
    int  CentreY(int Row) const { return Y + Row * Cell + Cell / 2; }
    // The text cells of the frame round the grid, its rules included.
    painter::core::Rect Frame() const { return {X - 1, Y / 2 - 1, Pixels() + 2, (Pixels() + 1) / 2 + 2}; }
    // The text row under the frame.
    int  RowBelow() const { return Frame().Y + Frame().Height; }
};

// The battle: the waters being shot at, larger, and the player's own fleet.
struct Arena {
    GridView Aim;
    GridView Own;
};

// What the glass of Glass cells shows of a grid Size by Size: the battle's two boards.
Arena ArenaFor(painter::core::Size Glass, int Size);
// The one board of the placement, at the left of its panel.
GridView PlacementFor(painter::core::Size Glass, int Size);
// Two boards of the same size side by side, with room beneath them for the figures.
Arena RevealFor(painter::core::Size Glass, int Size);

}  // namespace seabattle

#endif
