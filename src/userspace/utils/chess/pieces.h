/* pieces.h — the pieces as the game draws them: each a group of outlined
 * parts on one turned foot — a pawn's round head on its collar, a rook's
 * tower and battlements, a horse's head, a bishop's cleft mitre, a queen's
 * crown of five points, a king's crown and cross — ivory or ebony lit from
 * above, a soft shadow on the square under it. Worked out in a box 100 units
 * square and laid at any size. */
#ifndef CHESS_PIECES_H
#define CHESS_PIECES_H

#include <vector>

#include "painter/gui/items.h"

#include "position.h"

namespace chess {

namespace gui = painter::gui;

class Figure {
public:
    /* A piece Size points square, its group made in Into, at its corner. */
    Figure(gui::Group &Into, Piece Of, float Size);

    gui::Group &Whole() noexcept { return Group; }
    Piece       Is() const noexcept { return Of; }
    /* Drawn as another piece, the group kept where it is. */
    void        Become(Piece Other);

private:
    void Draw();

    gui::Group              Group;
    std::vector<gui::Shape> Parts;
    Piece                   Of;
    float                   Size;
};

}  // namespace chess

#endif
