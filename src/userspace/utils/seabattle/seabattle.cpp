/* seabattle — the classic sea battle for two people at one screen, the pencil
 * and paper game of the ships on a grid. Each player lays a fleet in secret,
 * and they take turns to call a cell of the other's waters: a miss, a hit, or a
 * ship sunk. Whoever sinks the whole fleet first wins.
 *
 *   seabattle                 the title: new game, rules, the hall of records
 *   seabattle Anna Boris      the setup, with those two names in it
 *
 * Before a game the setup chooses the names, the size of the grid, the fleet,
 * the rules (ships may touch or not; a hit earns another shot or not; a salvo
 * of as many shots as ships afloat) and how fast the shells fly; or the second
 * player is the computer. The screen is one: after every turn it darkens, asks
 * for the screen to be passed to the other player, and the turn begins when
 * that player presses enter, so neither sees the other's fleet.
 *
 * What it is made of: look (the settings), luck (the chances), rules, board,
 * match and hunter (the game, with nothing of the picture), records (the hall
 * of records, kept in a file of its own), canvas, sea, ship_art, effects,
 * board_view and layout (the pictures), hud (the words round them), context,
 * page and app (the loop), and the pages: title, setup, rules, hall, handover,
 * place, battle and victory.
 */

#include <new>
#include <string>
#include <string_view>

#include "box/form.h"
#include "box/print.h"
#include "painter/tui/stage.h"

#include "app.h"
#include "context.h"

BOX_FORM("takes optional words \"the two players' names, to go straight to the setup with them\"\n");

namespace {

// The least glass the battle is played on: the text console's.
constexpr std::int32_t LeastCols = 80;
constexpr std::int32_t LeastRows = 25;

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
        const char *const *Words = nullptr;
        std::size_t        Count = 0;
        if (form_words(0, &Words, &Count) != 0) return 1;
        if (Count > 2) {
            Say("seabattle: there are two players: say their names, or none");
            return 1;
        }
        if (form_screen_file() != 0) {
            Say("seabattle is played on the glass: there is nothing it could put into a file through ->");
            return 1;
        }
        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("seabattle"));
            return 1;
        }
        seabattle::Context Shared(std::move(*Opened), Count > 0 ? Words[0] : "", Count > 1 ? Words[1] : "");
        seabattle::Run(Shared, Count > 0 ? seabattle::PageId::Setup : seabattle::PageId::Title);
        return 0;
    } catch (const std::bad_alloc &) {
        Say("seabattle: there is no memory left for the battle");
        return 1;
    }
}
