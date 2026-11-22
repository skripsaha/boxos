/* flappybird — the classic: a little bird flaps through gaps in green pipes,
 * one point for each pair it gets past, and the first touch of a pipe or the
 * ground ends the round. Drawn in the screen's pixels as items the display
 * moves (painter::gui): a sky of the day or of the night as the machine's
 * clock says, clouds, a city, trees and bushes going by slower than the
 * ground, the bird — a finch of its own — tilting as it climbs and dives; bronze, silver, gold and platinum at
 * ten, twenty, thirty and forty; the best score kept in flappybird.record.
 *
 *   space, up, enter, w     flap — the first one starts the round
 *   p                       pause, and go on
 *   esc, q                  leave
 *
 * What it is made of: world (the bird, the pipes, the score — no screen),
 * skyline and scene (the picture), bird (the bird's figure), boards (the
 * words and the board over it), game (the frames, the keys, the rounds). */

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
            Say("flappybird is played on the glass: there is nothing it could put into a file through ->");
            return 1;
        }
        std::expected<painter::gui::Area, box::error> Opened = painter::gui::Area::Open(painter::gui::Placement::Whole);
        if (!Opened || Opened->On() != painter::gui::Glass::Pixels) {
            Say("flappybird flies in a framebuffer's pixels; this screen is a text page");
            return 1;
        }
        std::expected<painter::tui::Keyboard, painter::tui::StageError> Keys = painter::tui::Keyboard::Open();
        if (!Keys) {
            Say(Keys.error().Describe("flappybird"));
            return 1;
        }
        std::string Trouble;
        {
            flappybird::Game Play(std::move(*Opened), std::move(*Keys));
            Play.Run();
            Trouble = Play.Trouble();
        }
        if (!Trouble.empty()) Say("flappybird: the best score was not kept: " + Trouble);
        return 0;
    } catch (const std::bad_alloc &) {
        Say("flappybird: there is no memory left for the game");
        return 1;
    }
}
