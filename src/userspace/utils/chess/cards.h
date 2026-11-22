/* cards.h — the two cards laid over the game: the menu a game begins from —
 * whom to play, which side, how strong the computer is — and the verdict
 * when a game ends: mate and who won, or a draw and why. */
#ifndef CHESS_CARDS_H
#define CHESS_CARDS_H

#include <array>
#include <memory>
#include <string_view>

#include "painter/gui/area.h"
#include "painter/gui/items.h"

#include "pieces.h"

namespace chess {

/* How the next game is played. */
struct Choice {
    enum class Plays : std::uint8_t { White, Black, Either };
    bool  Computer = true;
    Plays Side     = Plays::White;
    int   Level    = 3;
};

/* The menu's rows; with a friend to play, only the first. */
enum class Ask : std::uint8_t { Opponent, Side, Level };

class Menu {
public:
    explicit Menu(gui::Area &Screen);

    /* Over everything, Row marked; Resumable: a game is under it to go back
     * to. */
    void Show(const Choice &Now, Ask Row, bool Resumable);
    void Hide();
    bool Open() const noexcept { return Shown; }

private:
    struct Line {
        gui::Text Name, Value;
    };

    gui::Area                             &Screen;
    gui::Group                             Whole;
    gui::Shape                             Dim, Card, Marker;
    gui::Text                              Title, Hint;
    std::array<Line, 3>                    Lines;
    std::array<gui::Shape, 6>              Pips;
    std::array<std::unique_ptr<Figure>, 2> Ornaments;
    bool                                   Shown = false;
    Ask                                    Marked = Ask::Opponent;
};

class Verdict {
public:
    /* Laid over the board, Middle its middle. */
    Verdict(gui::Area &Screen, painter::core::PointF Middle);

    void Show(std::string_view Title, std::string_view Line);
    void Hide();

private:
    gui::Area            &Screen;
    painter::core::PointF Middle;
    gui::Group            Whole;
    gui::Shape            Card;
    gui::Text             Title, Line, Hint;
};

}  // namespace chess

#endif
