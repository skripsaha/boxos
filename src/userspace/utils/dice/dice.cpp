/* dice — dice rolled on a table, several at once, for the games played around one.
 *
 *   dice                   what settings dice says: one d6 at first
 *   dice 3                 three d6                    dice 3d6     the same
 *   dice d20               one die of 20 sides
 *   dice 2d6+3             a number added              dice d20-1   and taken away
 *   dice 4d6kh3            the highest three of four   dice 4d6dl1  all but the lowest
 *   dice adv               a d20 with advantage        dice dis     with disadvantage
 *   dice 4d6 1d8 d4        groups apart by a space are added up
 *   dice help              the grammar
 *
 * The dice tumble across the table and settle; below it each die, each
 * group's sum and the total. Type another roll and press enter, or press
 * enter or space on an empty line for the same again; ctrl+? shows every key,
 * Esc leaves. What it is made of: expression (what is written), roll (what is
 * thrown), source (where the randomness is from), look (the settings), shape,
 * table and panel (the picture) and game (all of it together, and the keys).
 * Under an arrow the roll is the lines of text the file takes, and nothing is
 * drawn. */

#include <new>
#include <string>
#include <string_view>

#include "box/form.h"
#include "box/print.h"
#include "painter/tui/stage.h"

#include "expression.h"
#include "game.h"
#include "look.h"
#include "roll.h"
#include "source.h"

BOX_FORM("takes optional words \"what to roll: d20, 3d6, 2d6+3, 4d6kh3, adv — `dice help` says how it is written\"\n");

namespace {

// The least glass the table is of use on: a few dice, the result and the lines of keys.
constexpr std::int32_t LeastCols = 48;
constexpr std::int32_t LeastRows = 16;

void Say(const std::string &Text)
{
    print_to_screen((Text + "\n").c_str());
}

void SayGrammar()
{
    Say("dice [what to roll]");
    for (const std::string_view Line : dice::GrammarLines()) Say("  " + std::string(Line));
    Say("");
    Say("  dice 3d6        three dice     dice d20+5    one die and a number");
    Say("  dice 4d6dl1     roll four and drop the lowest");
}

std::string Joined(const char *const *Words, std::size_t Count)
{
    std::string Whole;
    for (std::size_t Each = 0; Each < Count; Each++) {
        if (Each) Whole += ' ';
        Whole += Words[Each];
    }
    return Whole;
}

}  // namespace

int main()
{
    try {
        const char *const *Words = nullptr;
        std::size_t        Count = 0;
        if (form_words(0, &Words, &Count) != 0) return 1;
        const std::string Typed = Joined(Words, Count);
        if (Typed == "help") {
            SayGrammar();
            return 0;
        }
        const dice::Look Shown = dice::ReadLook();
        std::string      Notice;
        std::expected<dice::Expression, dice::ParseError> Wanted = dice::Parse(Typed.empty() ? Shown.Expression : Typed);
        if (!Wanted && Typed.empty()) {
            Notice = "settings dice expression: " + Wanted.error().Message + " — d6 is rolled";
            Wanted = dice::Parse("1d6");
        }
        if (!Wanted) {
            Say("dice: " + Wanted.error().Message);
            Say("      dice help says how a roll is written");
            return 1;
        }

        dice::Source Random;
        if (form_screen_file() != 0) {
            const std::string Text = dice::PlainText(dice::Throw(*Wanted, Random));
            print_bytes(Text.data(), Text.size());
            return 0;
        }
        std::expected<painter::tui::Stage, painter::tui::StageError> Opened =
            painter::tui::Stage::Open(LeastCols, LeastRows);
        if (!Opened) {
            Say(Opened.error().Describe("dice"));
            return 1;
        }
        dice::Game Game(std::move(*Opened), Random, std::move(*Wanted), Notice);
        Game.Run();
        return 0;
    } catch (const std::bad_alloc &) {
        Say("dice: there is no memory left for the table");
        return 1;
    }
}
