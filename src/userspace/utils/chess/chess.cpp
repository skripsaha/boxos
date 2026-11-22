/* chess — the game of chess on a walnut board, against the computer or a
 * friend at the same keyboard. Every rule: castling, taking in passing,
 * promotion to any piece, check, mate, stalemate, the third repetition,
 * fifty moves, too little left to mate. The computer thinks on a strand of
 * its own at six levels, from a beginner to a few seconds of the machine's
 * best; moves can be taken back and the board turned. Drawn in the screen's
 * pixels as items the display moves (painter::gui).
 *
 *   arrows         the cursor            u    take back
 *   enter, space   pick, and put         f    turn the board
 *   esc            put back, or leave    n    a new game
 *
 * What it is made of: position (the rules — no screen), engine (the
 * computer's judgement and search), thinker (the engine on its own strand),
 * pieces (their shapes), board (the board and its marks), panel (the card
 * beside it), cards (the menu and the verdict), game (the keys, the moves,
 * the turns). */

#include <expected>
#include <new>
#include <string>
#include <string_view>

#include "box/form.h"
#include "box/print.h"
#include "painter/gui/area.h"
#include "painter/tui/keyboard.h"

#include "game.h"

BOX_FORM("");

namespace {

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

}  // namespace

int main()
{
    try {
        if (form_screen_file() != 0) {
            Say("chess is played on the glass: there is nothing it could put into a file through ->");
            return 1;
        }
        std::expected<painter::gui::Area, box::error> Opened = painter::gui::Area::Open(painter::gui::Placement::Whole);
        if (!Opened || Opened->On() != painter::gui::Glass::Pixels) {
            Say("chess is played in a framebuffer's pixels; this screen is a text page");
            return 1;
        }
        std::expected<painter::tui::Keyboard, painter::tui::StageError> Keys = painter::tui::Keyboard::Open();
        if (!Keys) {
            Say(Keys.error().Describe("chess"));
            return 1;
        }
        chess::Game Play(std::move(*Opened), std::move(*Keys));
        Play.Run();
        return 0;
    } catch (const std::bad_alloc &) {
        Say("chess: there is no memory left for the game");
        return 1;
    } catch (const std::system_error &) {
        Say("chess: the computer could not be given a strand to think on");
        return 1;
    }
}
