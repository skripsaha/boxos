/* board.h — the board as the display shows it: a walnut frame with a line of
 * brass and the files and ranks named on it, maple and walnut squares, the
 * marks laid on them — the last move, the picked piece and where it may go,
 * a king in check glowing red — the pieces, the cursor, and the choice of
 * piece a pawn becomes on the last rank.
 *
 * The pieces follow the position: whatever moved glides to its square over
 * the others, what was taken fades, what comes back with a move taken back
 * fades in, and a pawn reaching the last rank becomes what it was promoted
 * to. Turned over, everything goes to the other side. */
#ifndef CHESS_BOARD_H
#define CHESS_BOARD_H

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include "painter/gui/area.h"
#include "painter/gui/items.h"

#include "pieces.h"
#include "position.h"

namespace chess {

class BoardView {
public:
    /* Square points a square, the frame's corner at Corner. */
    BoardView(gui::Area &Screen, float Square, painter::core::PointF Corner, bool Flipped);

    /* The whole board, frame and all, in points. */
    static float Across(float Square);

    /* The pieces where At has them: Glide, moving there, else at once. */
    void Show(const Position &At, bool Glide);
    void Flip(bool On);
    void Cursor(Square At, bool Shown);
    /* The picked piece's square and the moves it has, or none. */
    void Pick(std::optional<Square> From, const MoveList &Reach, const Position &At);
    void Last(Move One);
    void Check(std::optional<Square> King);
    /* The four pieces a pawn reaching To may become, Chosen marked; Close
     * puts them away. */
    void Offer(Square To, Side Of, int Chosen);
    void Close();

private:
    struct Placed {
        std::unique_ptr<Figure> Art;
        int                     Cell = -1;   // 0 to 63 where it stands, -1 taken
    };

    painter::core::PointF Corner(int Cell) const;
    void                  MakeFrame();
    void                  MakeSquares();
    void                  Label();
    void                  Glide(Placed &One, int Cell, bool Smooth);
    void                  Mark(gui::Shape &Mark, std::optional<Square> On);

    gui::Area                 &Screen;
    float                      S, F;      // a square; the frame round them
    bool                       Turned;
    gui::Group                 Whole, Marks, Reaches, Pieces, Offered;
    std::vector<gui::Shape>    Still;
    std::array<gui::Text, 16>  Names;     // a to h, then 1 to 8
    gui::Shape                 LastFrom, LastTo, Picked, Glow, Pointer, Card, Chosen;
    std::vector<gui::Shape>    Dots;
    std::vector<Placed>        Figures;
    std::vector<std::unique_ptr<Figure>> Choices;
    Move                       Shown{};
    std::optional<Square>      PickedAt, KingAt;
    Square                     CursorAt = 0;
};

}  // namespace chess

#endif
