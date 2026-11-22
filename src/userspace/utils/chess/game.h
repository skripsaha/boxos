/* game.h — a game of chess from the menu to its end and the next: the
 * person's keys on the board, the computer's moves taken when it answers,
 * moves taken back, the board turned, and every change shown on the board
 * and the card beside it.
 *
 *   arrows        the cursor over the board
 *   enter, space  pick a piece, put it where it may go; pick another
 *   u             take back — against the computer, your last move and its
 *                 answer
 *   f             turn the board
 *   n             the menu, for a new game
 *   esc           put the picked piece back; with none picked, leave
 *   q, ctrl+q     leave */
#ifndef CHESS_GAME_H
#define CHESS_GAME_H

#include <optional>
#include <string>
#include <vector>

#include "painter/gui/area.h"
#include "painter/gui/items.h"
#include "painter/tui/keyboard.h"

#include "board.h"
#include "cards.h"
#include "panel.h"
#include "position.h"
#include "thinker.h"

namespace chess {

class Game {
public:
    Game(gui::Area Screen, painter::tui::Keyboard Keys);

    void Run();

private:
    /* Everything laid for the screen as it is now. */
    void Build();
    void OpenMenu();
    void Begin();
    /* A move was played or taken back: the marks, the card, the end, and the
     * computer set thinking when the move is its. */
    void Turned();
    /* The computer's move, when it has answered and its time has come. */
    void Answer();
    void Play(Move One);
    void TakeBack();
    void Pick();
    void Step(int Right, int Down);
    void Flip();
    bool Human(Side Of) const noexcept { return !Rules.Computer || Of == Person; }
    std::string Status(Outcome Now) const;

    bool OnEvent(const gui::Event &Told);
    bool OnKey(const painter::tui::KeyEvent &Key);
    bool OnMenuKey(const painter::tui::KeyEvent &Key);
    void OnOfferKey(const painter::tui::KeyEvent &Key);

    gui::Area                 Screen;
    painter::tui::Keyboard    Keys;
    Position                  Board;
    std::vector<std::string>  Written;
    Choice                    Rules, Asked;   // the game's, and the menu's as it is being chosen
    Ask                       Row     = Ask::Opponent;
    Side                      Person  = White;
    std::string               Names[2];
    bool                      Started = false, Over = false, Flipped = false;
    Thinker                   Mind;
    std::uint64_t             Seed = 0;
    gui::Shape                Backdrop;
    std::optional<BoardView>  View;
    std::optional<SidePanel>  Beside;
    std::optional<Verdict>    Ending;
    std::optional<Menu>       Options;
    painter::core::PointF     BuiltFor{};
    Square                    CursorAt = SquareAt(4, 1);
    std::optional<Square>     From;
    MoveList                  Reach;
    std::optional<Move>       Promoting;
    int                       Offered = 0;
};

}  // namespace chess

#endif
